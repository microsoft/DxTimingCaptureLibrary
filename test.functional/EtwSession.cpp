// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "pch.h"

#include "EtwSession.h"

#include <algorithm>
#include <atomic>

#include <DxTimingCaptureLibrary/DxTimingCaptureEventHandler.h>
#include <DxTimingCaptureLibrary/EtwProviders.h>

using namespace DirectX::Etw;

namespace
{
    void ThrowIfFailed(ULONG error, const char* operation)
    {
        if (error != ERROR_SUCCESS)
        {
            throw std::system_error(
                static_cast<int>(error),
                std::system_category(),
                operation);
        }
    }
}

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

// The ETW consumer callback is a C-style function pointer, so it cannot capture
// state. We thread the owning session through EVENT_TRACE_LOGFILE::Context, which
// ETW hands back on each record as EVENT_RECORD::UserContext.
static void WINAPI OnEvent(EVENT_RECORD* record)
{
    static_cast<SimpleEtwSession*>(record->UserContext)->OnEventRecord(record);
}

void SimpleEtwSession::OnEventRecord(_EVENT_RECORD* record)
{
    try
    {
        m_handler->HandleEventRecord(record);
    }
    catch (...)
    {
        // A real consumer must not let an exception unwind through ProcessTrace (it
        // runs through C code). Capture the first one so the rest of the trace still
        // drains and the test can assert on it after the session ends.
        if (!m_consumerException)
        {
            m_consumerException = std::current_exception();
        }
    }
}

void SimpleEtwSession::OnDiagnostic(DiagnosticSeverity severity, DiagnosticCode code, std::wstring_view message)
{
    m_diagnostics.push_back({ severity, code, std::wstring(message) });
}

size_t SimpleEtwSession::CountDiagnostics(DiagnosticCode code) const
{
    return static_cast<size_t>(std::ranges::count(m_diagnostics, code, &SessionDiagnostic::Code));
}

void SimpleEtwSession::ConsumerThread()
{
    EVENT_TRACE_LOGFILE trace = {};
    trace.LoggerName = const_cast<LPWSTR>(m_sessionName.c_str());
    trace.ProcessTraceMode =
        PROCESS_TRACE_MODE_REAL_TIME |
        PROCESS_TRACE_MODE_EVENT_RECORD |
        // Without this, ProcessTrace converts EventHeader.TimeStamp to FILETIME
        // (100 ns since 1601) regardless of the session's QPC clock, which a naive
        // ticks->ns converter would overflow. Asking for raw timestamps keeps them in
        // the session's QPC domain (Wnode.ClientContext == 1) so QpcTimestampConverter
        // converts them to a sane absolute nanosecond value.
        PROCESS_TRACE_MODE_RAW_TIMESTAMP;
    trace.EventRecordCallback = OnEvent;
    trace.Context = this;

    TRACEHANDLE traceHandle = OpenTrace(&trace);
    if (traceHandle == INVALID_PROCESSTRACE_HANDLE)
    {
        ThrowIfFailed(GetLastError(), "OpenTrace");
    }

    ULONG status = ProcessTrace(&traceHandle, 1, nullptr, nullptr);
    ThrowIfFailed(status, "ProcessTrace");

    // ProcessTrace has returned, so every captured record has been delivered. Report
    // the session's data-loss totals (which the library surfaces through our
    // DiagnosticsSink) and flush any entries the correlation logic was holding until
    // end-of-stream. Doing this here (rather than on the test thread) keeps all
    // handler interaction on a single thread.
    m_handler->ReportTraceStatistics(trace);
    m_handler->OnDataComplete();

    status = CloseTrace(traceHandle);
    ThrowIfFailed(status, "CloseTrace");
}

static void EnableProvider(TRACEHANDLE traceHandle, GUID const& providerGuid, ULONGLONG keyword, UCHAR level)
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

    ThrowIfFailed(err, "EnableTraceEx2 (enable provider)");

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

    ThrowIfFailed(err, "EnableTraceEx2 (capture state)");
}

static void EnableD3D12Provider(TRACEHANDLE sessionHandle, bool captureGpuMemoryUsage)
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

static void EnableDxgkProvider(TRACEHANDLE sessionHandle, bool captureGpuTiming, bool captureGpuMemoryUsage)
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

void SimpleEtwSession::Begin()
{
    // Give every session instance a unique name so leftover state from a prior test
    // (or another instance) cannot collide on StartTrace.
    static std::atomic<unsigned long long> s_sessionCounter{ 0 };
    m_sessionName = L"DxTimingCaptureLibrary.test.functional.session."
        + std::to_wstring(GetCurrentProcessId())
        + L"." + std::to_wstring(++s_sessionCounter);

    DxTimingCaptureLibraryOptions libraryOptions{};
    libraryOptions.TrackApiObjects = true;
    libraryOptions.TrackGpuTiming = (m_gpuTimingsCallbacks != nullptr);
    libraryOptions.TrackGpuEngineActivity = (m_gpuEngineActivityCallbacks != nullptr);

    // Only wire up the callbacks a test actually overrides; the library
    // substitutes a no-op for every slot left null. The handler borrows these,
    // so they stay owned by the session (which outlives m_handler).
    DxTimingCaptureEventCallbacks callbacks;
    callbacks.ApiObjectCallbacks = m_apiObjectCallbacks.get();
    callbacks.GpuTimingsCallbacks = m_gpuTimingsCallbacks.get();
    callbacks.GpuEngineActivityCallbacks = m_gpuEngineActivityCallbacks.get();
    callbacks.DirectStorageCallbacks = m_directStorageCallbacks.get();
    callbacks.RuntimeFailureCallbacks = m_runtimeFailureCallbacks.get();
    callbacks.DiagnosticsSink = this;

    m_handler = DxTimingCaptureEventHandler::Create(GetCurrentProcessId(), libraryOptions, callbacks);

    EventTraceProperties properties;
    TRACEHANDLE sessionHandle = 0;
    auto u = ::StartTraceW(&sessionHandle, m_sessionName.c_str(), properties);
    ThrowIfFailed(u, "StartTraceW");
    m_sessionHandle = sessionHandle;

    EnableD3D12Provider(sessionHandle, libraryOptions.TrackApiObjects);
    EnableDxgkProvider(sessionHandle, libraryOptions.TrackGpuTiming, libraryOptions.TrackApiObjects);

    static constexpr ULONGLONG MatchAllEventsEtwKeyword = 0xFFFFFFFFFFFFFFFF;
    EnableProvider(sessionHandle, DirectStorage::DStorageEtwProvider, MatchAllEventsEtwKeyword, TRACE_LEVEL_VERBOSE);

    m_thread = std::thread(&SimpleEtwSession::ConsumerThread, this);

    m_running = true;
}

void SimpleEtwSession::End()
{
    EventTraceProperties properties;
    auto u = ControlTraceW(
        static_cast<TRACEHANDLE>(m_sessionHandle),
        m_sessionName.c_str(),
        properties,
        EVENT_TRACE_CONTROL_STOP
    );
    ThrowIfFailed(u, "ControlTraceW");

    m_thread.join();

    m_running = false;
}
