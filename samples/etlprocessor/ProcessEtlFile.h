// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#pragma once

#include <exception>
#include <stdexcept>
#include <string>

#include <windows.h>
#include <evntrace.h>
#include <evntcons.h>

namespace EtlProcessor
{
    template <typename DispatcherType>
    struct EtlProcessContext
    {
        DispatcherType* Dispatcher;
        std::exception_ptr Exception;

        static void EventCallback(EVENT_RECORD* record) noexcept
        {
            auto* context = static_cast<EtlProcessContext<DispatcherType>*>(record->UserContext);
            if (context->Exception)
            {
                // We've hit an exception previously, so we skip over any
                // remaining records.
                return;
            }

            try
            {
                context->Dispatcher->Dispatch(record);
            }
            catch (...)
            {
                // We mustn't throw from inside the callback (since we're inside
                // ProcessTrace and can't assume that it is exception safe), so
                // we store the exception.  After ProcessTrace is complete we
                // can rethrow it.
                context->Exception = std::current_exception();
            }
        }
    };

    template <typename DispatcherType>
    void ProcessEtlFile(std::wstring const& etlFilePath, DispatcherType* dispatcher)
    {
        EtlProcessContext<DispatcherType> context;
        context.Dispatcher = dispatcher;

        EVENT_TRACE_LOGFILE logFile{};
        logFile.LogFileName = const_cast<wchar_t*>(etlFilePath.c_str());
        logFile.ProcessTraceMode = PROCESS_TRACE_MODE_EVENT_RECORD | PROCESS_TRACE_MODE_RAW_TIMESTAMP;
        logFile.EventRecordCallback = EtlProcessContext<DispatcherType>::EventCallback;
        logFile.Context = &context;

        TRACEHANDLE trace = OpenTrace(&logFile);
        if (trace == INVALID_PROCESSTRACE_HANDLE)
        {
            throw std::runtime_error(
                "Failed to open the trace file (OpenTrace error " +
                std::to_string(GetLastError()) + ").");
        }

        dispatcher->SetTraceInfo(logFile);

        ULONG processStatus = ProcessTrace(&trace, 1, nullptr, nullptr);
        CloseTrace(trace);

        if (context.Exception)
        {
            // If we threw during a dispatch then we rethrow it now.
            std::rethrow_exception(context.Exception);
        }

        if (processStatus != ERROR_SUCCESS)
        {
            throw std::runtime_error(
                "Failed to process the trace file (ProcessTrace error " +
                std::to_string(processStatus) + ").");
        }
    }
}
