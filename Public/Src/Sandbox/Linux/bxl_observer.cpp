// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "dirent.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <unistd.h>
#include <sys/types.h>

#include "bxl_observer.hpp"
#include "IOHandler.hpp"

static std::string empty_str("");

static void HandleAccessReport(AccessReport report, int _)
{
    BxlObserver::GetInstance()->SendReport(report);
}

AccessCheckResult BxlObserver::sNotChecked = AccessCheckResult::Invalid();

BxlObserver* BxlObserver::GetInstance()
{
    static BxlObserver s_singleton;
    return &s_singleton;
}

BxlObserver::BxlObserver()
{
    real_readlink("/proc/self/exe", progFullPath_, PATH_MAX);

    const char *rootPidStr = getenv(BxlEnvRootPid);
    rootPid_ = (rootPidStr && *rootPidStr) ? atoi(rootPidStr) : -1;

    InitFam();
    InitLogFile();
}

void BxlObserver::InitFam()
{
    // read FAM env var
    const char *famPath = getenv(BxlEnvFamPath);
    if (!(famPath && *famPath))
    {
        real_fprintf(stderr, "[%s] ERROR: Env var '%s' not set\n", __func__, BxlEnvFamPath);
        return;
    }

    // read FAM 
    FILE *famFile = real_fopen(famPath, "rb");
    if (!famFile)
    {
        _fatal("Could not open file '%s'; errno: %d", famPath, errno);
    }

    fseek(famFile, 0, SEEK_END);
    long famLength = ftell(famFile);
    rewind(famFile);

    char *famPayload = (char *)malloc(famLength);
    real_fread(famPayload, famLength, 1, famFile);
    real_fclose(famFile);

    // create SandboxedPip (which parses FAM and throws on error)
    pip_ = std::shared_ptr<SandboxedPip>(new SandboxedPip(getpid(), famPayload, famLength));

    // create sandbox
    sandbox_ = new Sandbox(0, Configuration::DetoursLinuxSandboxType);

    // initialize sandbox
    if (!sandbox_->TrackRootProcess(pip_))
    {
        _fatal("Could not track root process %s:%d", __progname, getpid());
    }

    process_ = sandbox_->FindTrackedProcess(getpid());
    process_->SetPath(progFullPath_);
    sandbox_->SetAccessReportCallback(HandleAccessReport);
}

void BxlObserver::InitLogFile()
{
    const char *logPath = getenv(BxlEnvLogPath);
    if (logPath && *logPath)
    {
        strlcpy(logFile_, logPath, PATH_MAX);
        logFile_[PATH_MAX-1] = '\0';
    }
    else
    {
        logFile_[0] = '\0';
    }
}

bool BxlObserver::Send(const char *buf, size_t bufsiz)
{
    if (!real_open)
    {
        _fatal("syscall 'open' not found; errno: %d", errno);
    }

    // TODO: instead of failing, implement a critical section
    if (bufsiz > PIPE_BUF)
    {
        _fatal("Cannot atomically send a buffer whose size (%ld) is greater than PIPE_BUF (%d)", bufsiz, PIPE_BUF);
    }

    const char *reportsPath = GetReportsPath();
    int logFd = real_open(reportsPath, O_WRONLY | O_APPEND, 0);
    if (logFd == -1)
    {
        _fatal("Could not open file '%s'; errno: %d", reportsPath, errno);
    }

    ssize_t numWritten = real_write(logFd, buf, bufsiz);
    if (numWritten < bufsiz)
    {
        _fatal("Wrote only %ld bytes out of %ld", numWritten, bufsiz);
    }

    real_close(logFd);
    return true;
}

bool BxlObserver::SendReport(AccessReport &report)
{
    // there is no central sendbox process here (i.e., there is an instance of this 
    // guy in every child process), so counting process tree size is not feasible
    if (report.operation == FileOperation::kOpProcessTreeCompleted)
    {
        return true;
    }

    const int PrefixLength = sizeof(uint);
    char buffer[PIPE_BUF] = {0};
    int maxMessageLength = PIPE_BUF - PrefixLength;
    int numWritten = snprintf(
        &buffer[PrefixLength], maxMessageLength, "%s|%d|%d|%d|%d|%d|%d|%s\n", 
        __progname, getpid(), report.requestedAccess, report.status, report.reportExplicitly, report.error, report.operation, report.path);
    if (numWritten == maxMessageLength)
    {
        // TODO: once 'send' is capable of sending more than PIPE_BUF at once, allocate a bigger buffer and send that
        _fatal("Message truncated to fit PIPE_BUF (%d): %s", PIPE_BUF, buffer);
    }

    LOG_DEBUG("Sending report: %s", &buffer[PrefixLength]);
    *(uint*)(buffer) = numWritten;
    return Send(buffer, numWritten + PrefixLength);
}

void BxlObserver::report_exec(const char *syscallName, const char *procName, const char *file)
{
    // first report 'procName' as is (without trying to resolve it) to ensure that a process name is reported before anything else
    report_access(syscallName, ES_EVENT_TYPE_NOTIFY_EXEC, std::string(procName), empty_str);
    report_access(__func__, ES_EVENT_TYPE_NOTIFY_EXEC, file);
}

AccessCheckResult BxlObserver::report_access(const char *syscallName, es_event_type_t eventType, std::string reportPath, std::string secondPath)
{
    // TODO: don't stat all the time
    mode_t mode = get_mode(reportPath.c_str());

    std::string execPath = eventType == ES_EVENT_TYPE_NOTIFY_EXEC
        ? reportPath
        : std::string(progFullPath_);

    IOEvent event(getpid(), 0, getppid(), eventType, reportPath, secondPath, execPath, mode, false);
    return report_access(syscallName, event);
}

AccessCheckResult BxlObserver::report_access(const char *syscallName, IOEvent &event)
{
    es_event_type_t eventType = event.GetEventType();

    AccessCheckResult result = sNotChecked;

    if (IsEnabled())
    {
        IOHandler handler(sandbox_);
        handler.SetProcess(process_);
        result = handler.HandleEvent(event);
    }

    LOG_DEBUG("(( %10s:%2d )) %s %s%s", syscallName, event.GetEventType(), event.GetEventPath(), 
        !result.ShouldReport() ? "[Ignored]" : result.ShouldDenyAccess() ? "[Denied]" : "[Allowed]",
        result.ShouldDenyAccess() && IsFailingUnexpectedAccesses() ? "[Blocked]" : "");

    return result;
}

AccessCheckResult BxlObserver::report_access(const char *syscallName, es_event_type_t eventType, const char *pathname, int flags)
{
    return report_access(syscallName, eventType, normalize_path(pathname, flags), "");
}

AccessCheckResult BxlObserver::report_access_fd(const char *syscallName, es_event_type_t eventType, int fd)
{
    char fullpath[PATH_MAX] = {0};
    fd_to_path(fd, fullpath, PATH_MAX);

    return fullpath[0] == '/'
        ? report_access(syscallName, eventType, std::string(fullpath), empty_str)
        : sNotChecked; // this file descriptor is not a non-file (e.g., a pipe, or socket, etc.) so we don't care about it
}

AccessCheckResult BxlObserver::report_access_at(const char *syscallName, es_event_type_t eventType, int dirfd, const char *pathname, int flags)
{
    char fullpath[PATH_MAX] = {0};
    ssize_t len = 0;

    if (dirfd == AT_FDCWD)
    {
        getcwd(fullpath, PATH_MAX);
        len = strlen(fullpath);
    }
    else
    {
        len = fd_to_path(dirfd, fullpath, PATH_MAX);
    }

    if (len <= 0)
    {
        _fatal("Could not get path for fd %d; errno: %d", dirfd, errno);
    }

    snprintf(&fullpath[len], PATH_MAX - len, "/%s", pathname);
    return report_access(syscallName, eventType, fullpath, flags);
}

ssize_t BxlObserver::fd_to_path(int fd, char *buf, size_t bufsiz)
{
    char procPath[100] = {0};
    sprintf(procPath, "/proc/self/fd/%d", fd);
    ssize_t result = real_readlink(procPath, buf, bufsiz);
    return result;
}

std::string BxlObserver::normalize_path_at(int dirfd, const char *pathname, int oflags)
{
    char fullpath[PATH_MAX] = {0};
    size_t len = 0;

    // no pathname given --> read path for dirfd
    if (pathname == NULL)
    {
        fd_to_path(dirfd, fullpath, PATH_MAX);
        return fullpath;
    }

    // if relative path --> resolve it against dirfd
    if (*pathname != '/' && *pathname != '~')
    {
        if (dirfd == AT_FDCWD)
        {
            getcwd(fullpath, PATH_MAX);
            len = strlen(fullpath);
        }
        else
        {
            len = fd_to_path(dirfd, fullpath, PATH_MAX);
        }

        if (len <= 0)
        {
            _fatal("Could not get path for fd %d; errno: %d", dirfd, errno);
        }

        fullpath[len] = '/';
        strcpy(fullpath + len + 1, pathname);
    }
    else
    {
        strcpy(fullpath, pathname);
    }

    bool followFinalSymlink = (oflags & O_NOFOLLOW) == 0;
    resolve_path(fullpath, followFinalSymlink);

    return fullpath;
}

static void shift_left(char *str, int n)
{
    do
    {
        *(str - n) = *str;
    } while (*str++);
}

static char* find_prev_slash(char *pStr)
{
    while (*--pStr != '/');
    return pStr;
}

// resolve any intermediate directory symlinks 
//   - TODO: cache this
//   - TODO: break symlink cycles
void BxlObserver::resolve_path(char *fullpath, bool followFinalSymlink)
{
    assert(fullpath[0] == '/');

    char readlinkBuf[PATH_MAX];
    char *pFullpath = fullpath + 1;
    while (true)
    {
        // first handle "/../", "/./", and "//"
        if (*pFullpath == '/')
        {
            char *pPrevSlash = find_prev_slash(pFullpath);
            int parentDirLen = pFullpath - pPrevSlash - 1;
            if (parentDirLen == 0)
            {
                shift_left(pFullpath + 1, 1);
                continue;
            }
            else if (parentDirLen == 1 && *(pFullpath - 1) == '.')
            {
                shift_left(pFullpath + 1, 2);
                --pFullpath;
                continue;
            }
            else if (parentDirLen == 2 && *(pFullpath - 1) == '.' && *(pFullpath - 2) == '.')
            {
                // find previous slash unless already at the beginning 
                if (pPrevSlash > fullpath) 
                {
                    pPrevSlash = find_prev_slash(pPrevSlash);
                }
                int shiftLen = pFullpath - pPrevSlash;
                shift_left(pFullpath + 1, shiftLen);
                pFullpath = pPrevSlash + 1;
                continue;
            }
        }

        // call readlink for intermediate dirs and the final path if followSymlink is true
        ssize_t nReadlinkBuf = -1;
        char ch = *pFullpath;
        if (*pFullpath == '/' || (*pFullpath == '\0' && followFinalSymlink))
        {
            *pFullpath = '\0';
            nReadlinkBuf = real_readlink(fullpath, readlinkBuf, PATH_MAX);
            *pFullpath = ch;
        }

        // if not a symlink --> either continue or exit if at the end of the path
        if (nReadlinkBuf == -1)
        {
            if (*pFullpath == '\0')
            {
                break;
            }
            else
            {
                ++pFullpath;
                continue;
            }
        }

        // current path is a symlink
        readlinkBuf[nReadlinkBuf] = '\0';

        // report readlink for the current path
        *pFullpath = '\0';
        report_access("_readlink", ES_EVENT_TYPE_NOTIFY_READLINK, std::string(fullpath), empty_str);
        *pFullpath = ch;

        // append the rest of the original path to the readlink target
        strcpy(
            readlinkBuf + nReadlinkBuf, 
            (readlinkBuf[nReadlinkBuf-1] == '/' && *pFullpath == '/') ? pFullpath + 1 : pFullpath);

        // if readlink target is an absolute path -> overwrite fullpath with it and start from the beginning
        if (readlinkBuf[0] == '/')
        {
            strcpy(fullpath, readlinkBuf);
            pFullpath = fullpath + 1;
            continue;
        }

        // readlink target is a relative path -> replace the current dir in fullpath with the target
        pFullpath = find_prev_slash(pFullpath);
        strcpy(++pFullpath, readlinkBuf);
    }
}