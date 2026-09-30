// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "pch.h"

#include <cstdlib> // strtoul

#include <DxTimingCaptureLibrary/DxTimingCaptureLibrary.h>
#include <DxTimingCaptureLibrary/EtwProviders.h>

#include "EtwConfig.h"
#include "PrintfCallbacks.h"

using namespace DirectX::Etw;

static std::wstring g_etwSessionName;
static std::atomic<bool> g_running(true);
static TRACEHANDLE g_sessionHandle = 0;

static ApiObjectCallbacksPrintf g_apiObjectCallbacks;
static PixCounterCallbacksPrintf g_pixCounterCallbacks;
static ResidencyEventCallbacksPrintf g_residencyEventCallbacks;
static PixEventCallbacksPrintf g_pixEventCallbacks;
static MonitorCallbacksPrintf g_monitorEventCallbacks;
static GpuTimingsCallbacksPrintf g_gpuTimingsCallbacks;
static DxgkObjectCallbacksPrintf g_dxgkObjectCallbacks;
static PipelineStateEventCallbacksPrintf g_pipelineStateEventCallbacks;
static DirectStorageCallbacksPrintf g_directStorageCallbacks;
static DiagnosticsSinkPrintf g_diagnosticsSink;
static RuntimeFailureCallbacksPrintf g_runtimeFailureCallbacks;

std::unique_ptr<DxTimingCaptureEventHandler> g_dxTimingCaptureEventHandler;

void WINAPI OnEvent(EVENT_RECORD* record)
{
    // HandleEventRecord throws on a malformed event or a callback that returns a
    // failure HRESULT. This runs inside ProcessTrace's C call frames, where
    // letting an exception unwind is undefined behaviour, so the consumer must
    // contain it here: drop the event and keep processing the trace.
    try
    {
        g_dxTimingCaptureEventHandler->HandleEventRecord(record);
    }
    catch (...)
    {
        printf("Dropped an event: HandleEventRecord threw.\n");
    }
}

// ETW calls this once per delivered buffer. The logfile header carries the
// running EventsLost / BuffersLost totals, so this is where we feed them to the
// handler; it reports any newly observed loss through the diagnostics sink.
ULONG WINAPI OnBuffer(PEVENT_TRACE_LOGFILE logfile)
{
    g_dxTimingCaptureEventHandler->ReportTraceStatistics(*logfile);
    return TRUE; // keep processing
}

static void ConsumerThread()
{
    EVENT_TRACE_LOGFILE trace = {};
    trace.LoggerName = const_cast<LPWSTR>(g_etwSessionName.c_str());
    trace.ProcessTraceMode =
        PROCESS_TRACE_MODE_REAL_TIME |
        PROCESS_TRACE_MODE_EVENT_RECORD |
        // Keep EventHeader.TimeStamp in the session's raw QPC domain rather than letting
        // ProcessTrace convert it to FILETIME (100 ns since 1601), which the ticks->ns
        // converter would overflow.
        PROCESS_TRACE_MODE_RAW_TIMESTAMP;
    trace.EventRecordCallback = OnEvent;
    trace.BufferCallback = OnBuffer;

    TRACEHANDLE traceHandle = OpenTrace(&trace);
    if (traceHandle == INVALID_PROCESSTRACE_HANDLE)
    {
        printf("OpenTrace failed: %lu\n", GetLastError());
        DebugBreak();
    }

    // This blocks until the session stops
    ULONG status = ProcessTrace(&traceHandle, 1, nullptr, nullptr);
    if (status != ERROR_SUCCESS)
    {
        printf("ProcessTrace failed: %lu\n", status);
        DebugBreak();
    }

    // Flush any pending correlation before reading the final loss totals.
    g_dxTimingCaptureEventHandler->OnDataComplete();
    g_dxTimingCaptureEventHandler->ReportTraceStatistics(trace);

    status = CloseTrace(traceHandle);
    if (status != ERROR_SUCCESS)
    {
        printf("CloseTrace failed: %lu\n", status);
        DebugBreak();
    }
}

BOOL WINAPI ConsoleHandler(DWORD signal)
{
    if (signal == CTRL_C_EVENT)
    {
        g_running = false;
        return TRUE;
    }
    return FALSE;
}

int __cdecl main(int argc, char** argv)
{
    SetConsoleCtrlHandler(ConsoleHandler, TRUE);

    // The library decodes events for a single target process: the process whose
    // D3D12 work you want to observe. Pass its PID as the first command-line
    // argument; with no argument the sample observes its own process. (A single
    // target PID is required because WinPixEventDecoder currently decodes one
    // process at a time.)
    DWORD filterToProcessId = GetCurrentProcessId();
    if (argc > 1)
    {
        filterToProcessId = static_cast<DWORD>(strtoul(argv[1], nullptr, 10));
        if (filterToProcessId == 0)
        {
            printf("Usage: %s [processId]\n", argv[0]);
            return 1;
        }
    }
    printf("Observing process %lu\n", filterToProcessId);

    g_etwSessionName = L"DxTimingCaptureLibrary.sample.session" + std::to_wstring(GetCurrentProcessId());

    DxTimingCaptureLibraryOptions libraryOptions{};
    libraryOptions.TrackApiObjects = true;

    DxTimingCaptureEventCallbacks callbacks;
    callbacks.ApiObjectCallbacks = &g_apiObjectCallbacks;
    callbacks.PixCounterCallbacks = &g_pixCounterCallbacks;
    callbacks.ResidencyEventCallbacks = &g_residencyEventCallbacks;
    callbacks.PixEventCallbacks = &g_pixEventCallbacks;
    callbacks.MonitorEventCallbacks = &g_monitorEventCallbacks;
    callbacks.GpuTimingsCallbacks = &g_gpuTimingsCallbacks;
    callbacks.DxgkObjectCallbacks = &g_dxgkObjectCallbacks;
    callbacks.PipelineStateEventCallbacks = &g_pipelineStateEventCallbacks;
    callbacks.DirectStorageCallbacks = &g_directStorageCallbacks;
    callbacks.DiagnosticsSink = &g_diagnosticsSink;
    callbacks.RuntimeFailureCallbacks = &g_runtimeFailureCallbacks;

    g_dxTimingCaptureEventHandler = DxTimingCaptureEventHandler::Create(filterToProcessId, libraryOptions, callbacks);

    // Start Trace
    EventTraceProperties properties{};
  
    auto u = ::StartTraceW(&g_sessionHandle, g_etwSessionName.c_str(), properties);
    if (u != ERROR_SUCCESS)
    {
        printf("StartTraceW failed: %lu\n", u);
        DebugBreak();
    }

    EnableD3D12Provider(g_sessionHandle, libraryOptions.TrackApiObjects);
    EnableDxgkProvider(g_sessionHandle, libraryOptions.TrackGpuTiming, libraryOptions.TrackApiObjects);

    EnableProvider(g_sessionHandle, DirectStorage::DStorageEtwProvider, 0xFFFFFFFFFFFFFFFFULL, TRACE_LEVEL_VERBOSE);

    std::thread consumer(ConsumerThread);

    printf("Listening for events (Ctrl+C to stop)...\n");

    // Wait until Ctrl+C
    while (g_running)
    {
        Sleep(200);
    }

    printf("Stopping session...\n");

    u = ControlTraceW(
        g_sessionHandle,
        g_etwSessionName.c_str(),
        properties,
        EVENT_TRACE_CONTROL_STOP
    );
    if (u != ERROR_SUCCESS)
    {
        printf("ControlTraceW failed: %lu\n", u);
        DebugBreak();
    }

    consumer.join();

    printf("Done\n");
    return 0;
}
