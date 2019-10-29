// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

#include "stdafx.h"

#include "DebuggingHelpers.h"
#include "DetouredFunctions.h"
#include "DetoursHelpers.h"
#include "DetoursServices.h"
#include "FileAccessHelpers.h"
#include "StringOperations.h"
#include "UnicodeConverter.h"
#include "SubstituteProcessExecution.h"

using std::wstring;
using std::unique_ptr;
using std::vector;

/// Runs an injected substitute shim instead of the actual child process, passing the
/// original command and arguments to the shim along with, implicitly,
/// the current working directory and environment.
static BOOL WINAPI InjectShim(
    wstring               &commandWithoutQuotes,
    wstring               &argumentsWithoutCommand,
    bool                  replaceCommandNameForTrackedBuildEngine,
    LPSECURITY_ATTRIBUTES lpProcessAttributes,
    LPSECURITY_ATTRIBUTES lpThreadAttributes,
    BOOL                  bInheritHandles,
    DWORD                 dwCreationFlags,
    LPVOID                lpEnvironment,
    LPCWSTR               lpCurrentDirectory,
    LPSTARTUPINFOW        lpStartupInfo,
    LPPROCESS_INFORMATION lpProcessInformation)
{
    // Create a final buffer for the original command line - we prepend the original command
    // (if present) in quotes for easier parsing in the shim, ahead of the original argument list if provided.
    // This is an over-allocation because if lpCommandLine is non-null, lpCommandLine starts with
    // the contents of lpApplicationName, which we'll remove and replace with a quoted version.
    size_t fullCmdLineSizeInChars =
        commandWithoutQuotes.length() + argumentsWithoutCommand.length() +
        4;  // Command quotes and space and trailing null
    wchar_t *fullCommandLine = new wchar_t[fullCmdLineSizeInChars];
    if (fullCommandLine == nullptr)
    {
        Dbg(L"Failure running substitute shim process - failed to allocate buffer of size %d.", fullCmdLineSizeInChars * sizeof(WCHAR));
        SetLastError(ERROR_OUTOFMEMORY);
        return FALSE;
    }

    fullCommandLine[0] = L'"';
    wcscpy_s(fullCommandLine + 1, fullCmdLineSizeInChars, commandWithoutQuotes.c_str());
    wcscat_s(fullCommandLine, fullCmdLineSizeInChars, L"\" ");
    wcscat_s(fullCommandLine, fullCmdLineSizeInChars, argumentsWithoutCommand.c_str());

    const wchar_t* shimCommandLine = g_substituteProcessExecutionShimPath;
    wstring shimWithChangedFileName;  // Keep ref outside of if to avoid early delete.
    if (replaceCommandNameForTrackedBuildEngine)
    {
        // Get the filename of the command, copy the shim path and replace its tool name with the command.
        // TODO: This hack needs a feature flag like 'ConformShimFileNameToToolName' and allow a list of tool names this is allowed for.
        // We assume the fake tool file has already been created by the caller, hence it must know the full extent of executable names that can be replaced here.
        size_t commandFileNameStartIndex = commandWithoutQuotes.find_last_of(L'\\');
        if (commandFileNameStartIndex != wstring::npos)
        {
            commandFileNameStartIndex++;
        }
        else
        {
            commandFileNameStartIndex = 0;  // Whole string.
        }

        wchar_t* shimFileNameStart = wcsrchr(g_substituteProcessExecutionShimPath, L'\\');
        if (shimFileNameStart != nullptr)
        {
            shimFileNameStart++;
        }
        else
        {
            shimFileNameStart = g_substituteProcessExecutionShimPath;
        }

        shimWithChangedFileName = wstring(g_substituteProcessExecutionShimPath, (size_t)(shimFileNameStart - g_substituteProcessExecutionShimPath));
        shimWithChangedFileName += commandWithoutQuotes.c_str() + commandFileNameStartIndex;
        shimCommandLine = shimWithChangedFileName.c_str();
    }

    Dbg(L"Injecting substitute shim '%s' for process command line '%s'", shimWithChangedFileName.c_str(), fullCommandLine);
    BOOL rv = Real_CreateProcessW(
        /*lpApplicationName:*/ shimWithChangedFileName.c_str(),
        /*lpCommandLine:*/ fullCommandLine,
        lpProcessAttributes,
        lpThreadAttributes,
        bInheritHandles,
        dwCreationFlags,
        lpEnvironment,
        lpCurrentDirectory,
        lpStartupInfo,
        lpProcessInformation);

    delete[] fullCommandLine;
    return rv;
}

// https://stackoverflow.com/questions/216823/whats-the-best-way-to-trim-stdstring
static inline const wchar_t *trim_start(const wchar_t *str)
{
    while (wmemchr(L" \t\n\r", *str, 4))  ++str;
    return str;
}

// https://stackoverflow.com/questions/216823/whats-the-best-way-to-trim-stdstring
static inline const wchar_t *trim_end(const wchar_t *end)
{
    while (wmemchr(L" \t\n\r", end[-1], 4)) --end;
    return end;
}

// https://stackoverflow.com/questions/216823/whats-the-best-way-to-trim-stdstring
static inline std::wstring trim(const wchar_t *buffer, size_t len) // trim a buffer (input?)
{
    return std::wstring(trim_start(buffer), trim_end(buffer + len));
}

// https://stackoverflow.com/questions/216823/whats-the-best-way-to-trim-stdstring
static inline void trim_inplace(std::wstring& str)
{
    str.assign(trim_start(str.c_str()),
        trim_end(str.c_str() + str.length()));
}

// Returns in 'command' the command from lpCommandLine without quotes, and in commandArgs the arguments from the remainder of the string.
static const void FindApplicationNameFromCommandLine(const wchar_t *lpCommandLine, _Out_ wstring &command, _Out_ wstring &commandArgs)
{
    wstring fullCommandLine(lpCommandLine);
    if (fullCommandLine.length() == 0)
    {
        command = wstring();
        commandArgs = wstring();
        return;
    }

    if (fullCommandLine[0] == L'"')
    {
        // Find the close quote. Might not be present which means the command
        // is the full command line minus the initial quote.
        size_t closeQuoteIndex = fullCommandLine.find('"', 1);
        if (closeQuoteIndex == wstring::npos)
        {
            // No close quote. Take everything through the end of the command line as the command.
            command = fullCommandLine.substr(1);
            trim_inplace(command);
            commandArgs = wstring();
        }
        else
        {
            if (closeQuoteIndex == fullCommandLine.length() - 1)
            {
                // Quotes cover entire command line.
                command = fullCommandLine.substr(1, fullCommandLine.length() - 2);
                trim_inplace(command);
                commandArgs = wstring();
            }
            else
            {
                wstring noQuoteCommand = fullCommandLine.substr(1, closeQuoteIndex - 1);

                // Find the next delimiting space after the close double-quote.
                // For example a command like "c:\program files"\foo we need to
                // keep \foo and cut the quotes to produce c:\program files\foo
                size_t spaceDelimiterIndex = fullCommandLine.find(L' ', closeQuoteIndex + 1);
                if (spaceDelimiterIndex == wstring::npos)
                {
                    // No space, take everything through the end of the command line.
                    spaceDelimiterIndex = fullCommandLine.length();
                }

                command = (noQuoteCommand +
                    fullCommandLine.substr(closeQuoteIndex + 1, spaceDelimiterIndex - closeQuoteIndex - 1));
                trim_inplace(command);
                commandArgs = fullCommandLine.substr(spaceDelimiterIndex + 1);
                trim_inplace(commandArgs);
            }
        }
    }
    else
    {
        // No open quote, pure space delimiter.
        size_t spaceDelimiterIndex = fullCommandLine.find(' ');
        if (spaceDelimiterIndex == wstring::npos)
        {
            // No space, take everything through the end of the command line.
            spaceDelimiterIndex = fullCommandLine.length();
        }

        command = fullCommandLine.substr(0, spaceDelimiterIndex);
        commandArgs = fullCommandLine.substr(spaceDelimiterIndex + 1);
        trim_inplace(commandArgs);
    }
}

static bool CommandArgsContainMatch(const wchar_t *commandArgs, const wchar_t *argMatch)
{
    if (argMatch == nullptr)
    {
        // No optional match, meaning always match.
        return true;
    }

    return wcsstr(commandArgs, argMatch) != nullptr;
}

static int CountMatches(const wchar_t *s, const wchar_t *find, size_t findLen)
{
    int numMatches = 0;
    const wchar_t *current = s;
    while ((current = wcsstr(current, find)) != nullptr)  // TODO: StrStrIW
    {
        numMatches++;
        current += findLen;
    }

    return numMatches;
}

static int CountMatches(const char *s, const char *find, size_t findLen)
{
    int numMatches = 0;
    const char *current = s;
    while ((current = strstr(current, find)) != nullptr)  // TODO: StrStrIA
    {
        numMatches++;
        current += findLen;
    }

    return numMatches;
}

static bool g_ParsedMinParallelism = false;
static int g_MinParallelism;
static int GetMinParallelism()
{
    if (!g_ParsedMinParallelism)
    {
        wchar_t minParallelismStr[16];
        DWORD ret = GetEnvironmentVariable(L"__ANYBUILD_MINPARALLELISM", minParallelismStr, ARRAYSIZE(minParallelismStr));
        if (ret <= ARRAYSIZE(minParallelismStr))
        {
            g_MinParallelism = _wtoi(minParallelismStr);
            g_ParsedMinParallelism = true;
            Dbg(L"Shim: Found MinParallelism %d", g_MinParallelism);
        }
        else
        {
            Dbg(L"Shim: Error: Buffer size needed for __ANYBUILD_MINPARALLELISM was %u", ret);
        }
    }

    return g_MinParallelism;
}

static bool CallPluginFunc(const wstring& command, const wchar_t* commandArgs, LPVOID lpEnvironment, LPCWSTR lpWorkingDirectory)
{
    assert(g_SubstituteProcessExecutionPluginFunc != nullptr);

    if (lpEnvironment == nullptr)
    {
        lpEnvironment = GetEnvironmentStrings();
    }

    wchar_t curDir[MAX_PATH];
    if (lpWorkingDirectory == nullptr)
    {
        GetCurrentDirectory(ARRAYSIZE(curDir), curDir);
        lpWorkingDirectory = curDir;
    }

    return g_SubstituteProcessExecutionPluginFunc(command.c_str(), commandArgs, lpEnvironment, lpWorkingDirectory) != 0;
}

static bool ShouldSubstituteShim(const wstring &command, wstring &commandArgs, LPVOID lpEnvironment, LPCWSTR lpWorkingDirectory, bool &replaceCommandNameForTrackedBuildEngine)
{
    assert(g_SubstituteProcessExecutionShimPath != nullptr);

    replaceCommandNameForTrackedBuildEngine = false;

    // Easy cases.
    if (g_pShimProcessMatches == nullptr || g_pShimProcessMatches->empty())
    {
        if (g_SubstituteProcessExecutionPluginFunc != nullptr)
        {
            // Filter meaning is exclusive if we're shimming all processes, inclusive otherwise.
            bool filterMatch = CallPluginFunc(command.c_str(), commandArgs, lpEnvironment, lpWorkingDirectory);
            return (filterMatch && !g_ProcessExecutionShimAllProcesses) || (!filterMatch && g_ProcessExecutionShimAllProcesses);
        }

        // Shim everything or shim nothing if there are no matches to compare and no filter DLL.
        return g_ProcessExecutionShimAllProcesses;
    }

    size_t commandLen = command.length();

    bool foundMatch = false;

    for (std::vector<ShimProcessMatch*>::iterator it = g_pShimProcessMatches->begin(); it != g_pShimProcessMatches->end(); ++it)
    {
        ShimProcessMatch* pMatch = *it;

        const wchar_t* processName = pMatch->ProcessName.get();
        size_t processLen = wcslen(processName);

        // lpAppName is longer than e.g. "cmd.exe", see if lpAppName ends with e.g. "\cmd.exe"
        if (processLen < commandLen)
        {
            if (command[commandLen - processLen - 1] == L'\\' &&
                _wcsicmp(command.c_str() + commandLen - processLen, processName) == 0)
            {
                if (CommandArgsContainMatch(commandArgs.c_str(), pMatch->ArgumentMatch.get()))
                {
                    foundMatch = true;
                    break;
                }
            }

            continue;
        }

        if (processLen == commandLen)
        {
            if (_wcsicmp(processName, command.c_str()) == 0)
            {
                if (CommandArgsContainMatch(commandArgs.c_str(), pMatch->ArgumentMatch.get()))
                {
                    foundMatch = true;
                    break;
                }
            }
        }
    }

    // Filter meaning is exclusive if we're shimming all processes, inclusive otherwise.
    bool filterMatch = g_ProcessExecutionShimAllProcesses;
    if (g_SubstituteProcessExecutionPluginFunc != nullptr)
    {
        filterMatch = CallPluginFunc(command.c_str(), commandArgs, lpEnvironment, lpWorkingDirectory) != 0;
    }

    if (g_ProcessExecutionShimAllProcesses)
    {
        // A process or filter match mean we don't want to shim - an opt-out list.
        return !foundMatch && !filterMatch;
    }

    //erik: Begin filtering hackage.
    if (foundMatch && commandLen >= 6 && _wcsicmp(command.c_str() + commandLen - 6, L"cl.exe") == 0)  // TODO: Should check for prefix \ or check len == 6 since cl.exe is run by itself sometimes.
    {
        replaceCommandNameForTrackedBuildEngine = true;

        int numInputs =
            CountMatches(commandArgs.c_str(), L".cpp", 4) +
            CountMatches(commandArgs.c_str(), L".c ", 3) +  // TOOD: Misses .c files at end of string.
            CountMatches(commandArgs.c_str(), L".idl", 4);

        size_t responseFileArgStart = commandArgs.find_first_of(L'@');
        char *pText = nullptr;
        wchar_t *wideRsp = nullptr;
        size_t responseFileArgEnd = 0;
        DWORD fileSize = 0;
        if (responseFileArgStart != wstring::npos)
        {
            wstring responseFilePath;
            if (commandArgs[responseFileArgStart + 1] == L'"')  // @"path"
            {
                responseFileArgEnd = commandArgs.find_first_of(L'"', responseFileArgStart + 2);
                responseFilePath = commandArgs.substr(responseFileArgStart + 2, responseFileArgEnd - responseFileArgStart - 2);
                responseFileArgEnd++;  // Skip trailing quote.
            }
            else  // @path
            {
                responseFileArgEnd = commandArgs.find_first_of(L' ', responseFileArgStart + 1);
                if (responseFileArgEnd == wstring::npos)
                {
                    responseFileArgEnd = commandArgs.length();
                }
                responseFilePath = commandArgs.substr(responseFileArgStart + 1, responseFileArgEnd - responseFileArgStart - 1);
            }
            // Dbg(L"Shim: erik: cl.exe Found rsp file '%s' from args='%s'", responseFilePath.c_str(), commandArgs.c_str());

            // Read as a char/byte buffer, but MSBuild often uses Unicode, so we check for a Unicode BOM and switch behavior.
            HANDLE hFile = CreateFile(responseFilePath.c_str(), GENERIC_READ, 0, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
            fileSize = GetFileSize(hFile, NULL);
            pText = new char[fileSize + 2];  // Add 2 null characters at the end in case this is Unicode and we cast the pointer.
            DWORD bytesRead;
            // TODO: Check bytesRead to ensure it was all read in one block
            ReadFile(hFile, pText, fileSize, &bytesRead, NULL);
            CloseHandle(hFile);
            pText[fileSize] = '\0';
            pText[fileSize + 1] = '\0';

            // Dbg(L"Shim: erik: cl.exe Found rsp contents '%S' or '%s', [0]=%d, [1]=%d", pText, (wchar_t *)pText, pText[0], pText[1]);
            if (fileSize >= 2 && (byte)pText[0] == 0xFF && (byte)pText[1] == 0xFE)
            {
                wideRsp = (wchar_t *)(pText + 2);  // Skip BOM in reinterpret.
                // Dbg(L"Shim: erik: cl.exe Found BOM on file, casting to wchar_t* as'%s'", wideRsp);
                numInputs +=
                    CountMatches(wideRsp, L".cpp", 4) +
                    CountMatches(wideRsp, L".c ", 3) +  // TOOD: Misses .c files at end of string.
                    CountMatches(wideRsp, L".idl", 4);
            }
            else
            {
                // Dbg(L"Shim: erik: cl.exe No BOM, interpreting as char*");
                numInputs +=
                    CountMatches(pText, ".cpp", 4) +
                    CountMatches(pText, ".c ", 3) +  // TOOD: Misses .c files at end of string.
                    CountMatches(pText, ".idl", 4);
            }
        }

        if (numInputs < 1)
        {
            // Conform to managed code semantics - MinParallelism setting sent via environment assumes each command has at least parallelism 1.
            numInputs = 1;
        }
        
        const int minParallelism = GetMinParallelism();
        if (numInputs >= minParallelism)
        {
            if (pText != nullptr)
            {
                // We already went to the trouble of reading the rsp file, avoid doing it again in ClSimulator.cs.
                // Paste the rsp file contents into its original parameter location in commandArgs.
                if (wideRsp != nullptr)
                {
                    commandArgs.replace(responseFileArgStart, responseFileArgEnd - responseFileArgStart, wideRsp, fileSize / 2 - 1);
                }
                else
                {
                    int reqLength = ::MultiByteToWideChar(CP_UTF8, 0, pText, (int)fileSize, 0, 0);
                    wstring rsp((size_t)reqLength, L'\0');
                    ::MultiByteToWideChar(CP_UTF8, 0, pText, (int)fileSize, &rsp[0], (int)rsp.length());
                    commandArgs.replace(responseFileArgStart, responseFileArgEnd - responseFileArgStart, wideRsp, fileSize / 2);
                }

                delete[] pText;
            }

            Dbg(L"Shim: Found %d inputs, injecting shim since matches min %d, from args='%s'", numInputs, minParallelism, commandArgs.c_str());
            return true; 
        }
        else
        {
            Dbg(L"Shim: Found %d inputs, running locally since min is %d, from args='%s'", numInputs, minParallelism, commandArgs.c_str());
        }

        if (pText != nullptr)
        {
            delete[] pText;
        }
        
        return false;
    }

    // An opt-in list, shim if matching.
    return foundMatch || filterMatch;
}

BOOL WINAPI MaybeInjectSubstituteProcessShim(
    _In_opt_    LPCWSTR               lpApplicationName,
    _In_opt_    LPCWSTR               lpCommandLine,
    _In_opt_    LPSECURITY_ATTRIBUTES lpProcessAttributes,
    _In_opt_    LPSECURITY_ATTRIBUTES lpThreadAttributes,
    _In_        BOOL                  bInheritHandles,
    _In_        DWORD                 dwCreationFlags,
    _In_opt_    LPVOID                lpEnvironment,
    _In_opt_    LPCWSTR               lpCurrentDirectory,
    _In_        LPSTARTUPINFOW        lpStartupInfo,
    _Out_       LPPROCESS_INFORMATION lpProcessInformation,
    _Out_       bool&                 injectedShim)
{
    if (g_SubstituteProcessExecutionShimPath != nullptr && (lpCommandLine != nullptr || lpApplicationName != nullptr))
    {
        // When lpCommandLine is null we just use lpApplicationName as the command line to parse.
        // When lpCommandLine is not null, it contains the command, possibly with quotes containing spaces,
        // as the first whitespace-delimited token; we can ignore lpApplicationName in this case.
        Dbg(L"Shim: Finding command and args from lpApplicationName='%s', lpCommandLine='%s'", lpApplicationName, lpCommandLine);
        LPCWSTR cmdLine = lpCommandLine == nullptr ? lpApplicationName : lpCommandLine;
        wstring command;
        wstring commandArgs;
        FindApplicationNameFromCommandLine(cmdLine, command, commandArgs);
        Dbg(L"Shim: Found command='%s', args='%s' from lpApplicationName='%s', lpCommandLine='%s'", command.c_str(), commandArgs.c_str(), lpApplicationName, lpCommandLine);

        bool replaceCommandNameForTrackedBuildEngine = false;
        if (ShouldSubstituteShim(command, commandArgs, lpEnvironment, lpCurrentDirectory, replaceCommandNameForTrackedBuildEngine))
        {
            // Instead of Detouring the child, run the requested shim
            // passing the original command line, but only for appropriate commands.
            injectedShim = true;
            return InjectShim(
                command,
                commandArgs,
                replaceCommandNameForTrackedBuildEngine,
                lpProcessAttributes,
                lpThreadAttributes,
                bInheritHandles,
                dwCreationFlags,
                lpEnvironment,
                lpCurrentDirectory,
                lpStartupInfo,
                lpProcessInformation);
        }
    }

    injectedShim = false;
    return FALSE;
}
