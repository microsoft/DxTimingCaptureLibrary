// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#pragma once

#include <windows.h>
#include <evntrace.h>
#include <cstdio>

#include <D3D12Events.h>

struct EventTraceProperties
{
    static constexpr size_t MAX_SESSION_NAME_LENGTH = 1024;
    static constexpr size_t MAX_LOG_FILE_NAME_LENGTH = 1024;

    //
    // https://docs.microsoft.com/en-us/windows/desktop/ETW/event-trace-properties
    //
    // m_properties contains offsets to the m_loggerName and m_logFileName
    // relative to the start of m_properties.
    //
    // Both the logger name and file name are max length 1024.  The logger
    // name must be before the log file name in memory (!).  We must fill in
    // the log file name, but StartTrace will fill in the session name.
    //
    EVENT_TRACE_PROPERTIES Properties;
    wchar_t LoggerName[MAX_SESSION_NAME_LENGTH];
    wchar_t LogFileName[MAX_LOG_FILE_NAME_LENGTH];

    EventTraceProperties()
        : Properties{}
        , LoggerName{}
        , LogFileName{}
    {
        Properties.Wnode.BufferSize = sizeof(EventTraceProperties);
        Properties.Wnode.ClientContext = 1; // From the documentation: a value of 1 specifies QPC clock resolution.
        Properties.Wnode.Flags = WNODE_FLAG_TRACED_GUID;
        Properties.LogFileMode = EVENT_TRACE_REAL_TIME_MODE;
        Properties.EnableFlags = 0u;

        Properties.MaximumFileSize = 0;

        Properties.LogFileNameOffset = 0;

        // This sample emits only a handful of journal events, so a small buffer pool is plenty.
        Properties.BufferSize = 64;
        Properties.MinimumBuffers = 4;
        Properties.MaximumBuffers = 16;

        Properties.LoggerNameOffset = FIELD_OFFSET(EventTraceProperties, LoggerName);
        // The LoggerName itself is filled in by StartTrace
    }

    EVENT_TRACE_PROPERTIES* operator ->()
    {
        return &Properties;
    }

    operator EVENT_TRACE_PROPERTIES* ()
    {
        return &Properties;
    }
};

inline void EnableProvider(TRACEHANDLE traceHandle, GUID const& providerGuid, ULONGLONG keyword, UCHAR level)
{
    ENABLE_TRACE_PARAMETERS enableTraceParameters = { ENABLE_TRACE_PARAMETERS_VERSION_2, 0 };

    ULONG constexpr timeout = 5000; // 5 seconds, seems a reasonable default

    auto err = EnableTraceEx2(
        traceHandle,
        &providerGuid,
        EVENT_CONTROL_CODE_ENABLE_PROVIDER,
        level,
        keyword,
        0,
        timeout,
        &enableTraceParameters);

    if (err != ERROR_SUCCESS)
    {
        printf("EnableTraceEx2 failed: %lu\n", err);
        DebugBreak();
    }
}

inline void EnableD3D12Provider(TRACEHANDLE sessionHandle)
{
    // Journal entries are the only thing this sample consumes.
    EnableProvider(
        sessionHandle,
        Direct3D12EtwProviderGuid,
        D3D12_ETW_LOG_FLAGS_JOURNAL_ENTRIES,
        TRACE_LEVEL_RESERVED6);
}
