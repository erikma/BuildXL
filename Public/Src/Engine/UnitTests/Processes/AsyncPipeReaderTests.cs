// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

using System;
using System.Collections.Generic;
using System.IO;
using System.Text;
using System.Threading.Tasks;
using BuildXL.Native.IO;
using BuildXL.Native.Streams;
using BuildXL.Processes;
using BuildXL.Processes.Internal;
using BuildXL.Utilities;
using BuildXL.Utilities.Qualifier;
using BuildXL.Utilities.Instrumentation.Common;
using Microsoft.Win32.SafeHandles;
using Test.BuildXL.TestUtilities.Xunit;
using Xunit;

namespace Test.BuildXL.Processes
{
    public class AsyncPipeReaderTests    
    {
        private class MockAsyncFile : IAsyncFile
        {
            public SafeFileHandle Handle { get; }
            public string Path { get; }
            public FileDesiredAccess Access { get; } = FileDesiredAccess.FileReadAttributes;
            public bool CanRead { get; } = true;
            public bool CanWrite { get; }
            public FileKind Kind { get; } = FileKind.Pipe;

            public void Dispose()
            {
            }

            public void Close()
            {
            }

            public long GetCurrentLength() => 1234;

            public AsyncFileStream CreateReadableStream(bool closeFileOnStreamClose = false)
            {
                return null;
            }

            public Queue<byte[]> ReadsToReturn { get; } = new Queue<byte[]>();
            public int NumReadOverlappedCalls { get; private set; }
            public unsafe void ReadOverlapped(IIOCompletionTarget target, byte* pinnedBuffer, int bytesToRead, long fileOffset)
            {
                NumReadOverlappedCalls++;
                byte[] read = ReadsToReturn.Dequeue();
                for (int i = 0; i < read.Length; i++)
                {
                    pinnedBuffer[i] = read[i];
                }
            }

            public unsafe Task<FileAsyncIOResult> ReadAsync(byte[] buffer, int bytesToRead, long fileOffset)
            {
                return Task.FromResult(new FileAsyncIOResult());
            }

            public unsafe void WriteOverlapped(IIOCompletionTarget target, byte* pinnedBuffer, int bytesToWrite, long fileOffset)
            {
            }
        }
    
        [Fact]
        public void ReadReturnsPartialUtf16CharDoesNotCallCallback()
        {
            var file = new MockAsyncFile();

            // Partial \r wide char, then EOF, should not be able to assemble a widechar out of 1 byte.
            file.ReadsToReturn.Enqueue(new byte[] { (byte)'\r' });
            file.ReadsToReturn.Enqueue(new byte[0]);

            var callbacks = new List<string>();
            var reader = new AsyncPipeReader(
                file,
                receivedLine =>
                {
                    callbacks.Add(receivedLine);
                    return true;
                },
                Encoding.Unicode,
                4 * 1024);
            using (reader)
            {
                reader.BeginReadLine();
            }

            XAssert.AreEqual(2, file.NumReadOverlappedCalls);
            XAssert.AreEqual(0, callbacks.Count);
        }

        [Fact]
        public void ReadReturnsUtf8CarriageReturnThenLineFeedSingleCallback()
        {
            var file = new MockAsyncFile();

            // \r in first write, \n second write, then EOF, should not be able to assemble a widechar out of 1 byte.
            file.ReadsToReturn.Enqueue(new byte[] { (byte)'t', (byte)'e', (byte)'s', (byte)'t', (byte)'\r' });
            file.ReadsToReturn.Enqueue(new byte[] { (byte)'\n' });
            file.ReadsToReturn.Enqueue(new byte[0]);

            var callbacks = new List<string>();
            var reader = new AsyncPipeReader(
                file,
                receivedLine =>
                {
                    callbacks.Add(receivedLine);
                    return true;
                },
                Encoding.UTF8,
                4 * 1024);
            using (reader)
            {
                reader.BeginReadLine();
            }

            XAssert.AreEqual(3, file.NumReadOverlappedCalls);
            XAssert.AreEqual(1, callbacks.Count);
            XAssert.AreEqual("test", callbacks[0]);
        }
    }
}
