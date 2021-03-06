// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

using System;
using BuildXL.Cache.ContentStore.Interfaces.Logging;
using Microsoft.Practices.TransientFaultHandling;

namespace BuildXL.Cache.ContentStore.Service
{
    /// <nodoc />
    public class TransientErrorDetectionStrategy : ITransientErrorDetectionStrategy
    {
        /// <inheritdoc />
        public bool IsTransient(Exception ex)
        {
            var e = ex as ClientCanRetryException;
            if (e == null)
            {
                return false;
            }

            e.Context?.TraceMessage(Severity.Debug, $"Retryable error: {e.Message}");
            return true;
        }
    }
}
