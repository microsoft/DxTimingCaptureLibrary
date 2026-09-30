// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#pragma once

#include <DxTimingCaptureLibrary/EtwProviders.h>

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

        // Use the maximum buffer size for improved performance and reduced likelihood of lost events
        // Use lower MinimumBuffers so that ETW uses less memory when it does not need it. 
        // Use High MaximumBuffers so that we can still support very high data loads.
        Properties.BufferSize = 1024;
        Properties.MinimumBuffers = 64;
        Properties.MaximumBuffers = 1290;

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

    // Do rundown events too
    err = EnableTraceEx2(
        traceHandle,
        &providerGuid,
        EVENT_CONTROL_CODE_CAPTURE_STATE,
        TRACE_LEVEL_VERBOSE,
        0,
        0,
        INFINITE, // synchronous
        nullptr);

    if (err != ERROR_SUCCESS)
    {
        printf("EnableTraceEx2 failed: %lu\n", err);
        DebugBreak();
    }
}

inline void EnableD3D12Provider(TRACEHANDLE sessionHandle, bool captureGpuMemoryUsage)
{
    ULONGLONG d3d12Keywords = D3D12_ETW_LOG_FLAGS_RENDER_OPERATION_MARKERS |
        D3D12_ETW_LOG_FLAGS_DRIVER_CUSTOM_MARKERS |
        D3D12_ETW_LOG_FLAGS_BUNDLE_MARKERS |
        D3D12_ETW_LOG_FLAGS_NAMES |
        D3D12_ETW_LOG_FLAGS_DEVICES |
        D3D12_ETW_LOG_FLAGS_OBJECT_LIFETIME |
        D3D12_ETW_LOG_FLAGS_JOURNAL_ENTRIES; // runtime failure journal entries
    
    if (captureGpuMemoryUsage)
    {
        // D3D12_ETW_LOG_FLAGS_RESOURCES is required for tracking API objects in general.
        // D3D12_ETW_LOG_APIS is required for PSO compilation timing events.
        d3d12Keywords |= D3D12_ETW_LOG_FLAGS_RESOURCES | D3D12_ETW_LOG_APIS;
    }
    
    EnableProvider(sessionHandle, Direct3D12EtwProviderGuid, d3d12Keywords, TRACE_LEVEL_RESERVED6);
}

inline void EnableDxgkProvider(TRACEHANDLE sessionHandle, bool captureGpuTiming, bool captureGpuMemoryUsage)
{
    ULONGLONG dxgKernelKeywords = DXGK_KEYWORD_LOG_FLAGS_LONG_HAUL |
        DXGK_KEYWORD_LOG_FLAGS_BASE |
        DXGK_KEYWORD_LOG_FLAGS_PROFILER;

    if (captureGpuTiming)
    {
        dxgKernelKeywords |= DXGK_KEYWORD_LOG_FLAGS_HISTORY_BUFFER;
    }

    if (captureGpuMemoryUsage)
    {
        dxgKernelKeywords |= DXGK_KEYWORD_LOG_FLAGS_RESOURCE;   // DeviceAllocation, AdapterAllocation, PagingOpVirtualTransfer
    }

    if (captureGpuMemoryUsage || captureGpuTiming)
    {
        dxgKernelKeywords |= DXGK_KEYWORD_LOG_FLAGS_ALLOCATIONS_REFERENCES;  // VidMmMakeResident, VidMmEvict
    }

    EnableProvider(sessionHandle, DxgkControlGuid, dxgKernelKeywords, TRACE_LEVEL_VERBOSE);
}
