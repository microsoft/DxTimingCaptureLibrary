// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
//
// Like the printf sample, this listens for D3D12 / DXGK ETW events for a target
// process; instead of printing them it writes them into a Perfetto trace
// (dxtimingcapture.perfetto-trace) that opens directly in https://ui.perfetto.dev.

#include "pch.h"

#include <cstdlib>
#include <map>
#include <optional>

#include <DxTimingCaptureLibrary/DxTimingCaptureLibrary.h>

#include "EtwConfig.h"               // shared with the basic sample (samples/basic/)
#include "PerfettoCallbacks.h"
#include "PerfettoTraceWriter.h"

using namespace DirectX::Etw;

namespace
{
    const wchar_t* DiagnosticName(DiagnosticCode code)
    {
        switch (code)
        {
        case DiagnosticCode::GpuEndEventWithoutBeginEvent: return L"GPU end event without a begin event";
        case DiagnosticCode::CommandListExecutionMissingQueue: return L"command list execution with no command queue";
        case DiagnosticCode::TimestampEndBeforeStart: return L"end timestamp before start timestamp";
        case DiagnosticCode::ClockCalibrationCpuWentBackwards: return L"clock calibration CPU went backwards";
        case DiagnosticCode::ClockCalibrationGpuWentBackwards: return L"clock calibration GPU went backwards";
        case DiagnosticCode::PipelineStateCompilationStartWithoutObjectId: return L"pipeline state compilation start without an object id";
        case DiagnosticCode::PipelineStateCompilationEndWithoutStart: return L"pipeline state compilation end without a start";
        case DiagnosticCode::EventsLost: return L"ETW events lost";
        case DiagnosticCode::BuffersLost: return L"ETW buffers lost";
        case DiagnosticCode::CommandListExecutionsUnfinishedAtEndOfTrace: return L"command list executions unfinished at end of trace";
        }
        return L"unknown diagnostic";
    }

    // Counts rather than prints - a busy machine reports thousands of these.
    class CountingDiagnosticsSink final : public DiagnosticsSink
    {
        std::map<DiagnosticCode, size_t> m_counts;

    public:
        void OnDiagnostic(DiagnosticSeverity, DiagnosticCode code, std::wstring_view) override
        {
            ++m_counts[code];
        }

        void PrintSummary() const
        {
            for (const auto& [code, count] : m_counts)
            {
                std::printf("Diagnostic: %ls (%zu)\n", DiagnosticName(code), count);
            }
        }
    };
}

static std::wstring g_etwSessionName;
static std::wstring g_etlFilePath; // when non-empty, consume this .etl offline instead of a live session
static std::atomic<bool> g_running(true);
static TRACEHANDLE g_sessionHandle = 0;

std::unique_ptr<DxTimingCaptureEventHandler> g_dxTimingCaptureEventHandler;

void WINAPI OnEvent(EVENT_RECORD* record)
{
    // HandleEventRecord runs inside ProcessTrace's C frames; an exception must not
    // unwind through ETW, so contain it here (drop the event, keep going).
    try
    {
        g_dxTimingCaptureEventHandler->HandleEventRecord(record);
    }
    catch (...)
    {
        printf("Dropped an event: HandleEventRecord threw.\n");
    }
}

ULONG WINAPI OnBuffer(PEVENT_TRACE_LOGFILE logfile)
{
    g_dxTimingCaptureEventHandler->ReportTraceStatistics(*logfile);
    return TRUE;
}

static void ConsumerThread()
{
    EVENT_TRACE_LOGFILE trace = {};
    const bool offline = !g_etlFilePath.empty();
    if (offline)
    {
        trace.LogFileName = const_cast<LPWSTR>(g_etlFilePath.c_str());
        trace.ProcessTraceMode =
            PROCESS_TRACE_MODE_EVENT_RECORD |
            PROCESS_TRACE_MODE_RAW_TIMESTAMP;
    }
    else
    {
        trace.LoggerName = const_cast<LPWSTR>(g_etwSessionName.c_str());
        trace.ProcessTraceMode =
            PROCESS_TRACE_MODE_REAL_TIME |
            PROCESS_TRACE_MODE_EVENT_RECORD |
            PROCESS_TRACE_MODE_RAW_TIMESTAMP; // keep raw QPC timestamps
    }
    trace.EventRecordCallback = OnEvent;
    trace.BufferCallback = OnBuffer;

    TRACEHANDLE traceHandle = OpenTrace(&trace);
    if (traceHandle == INVALID_PROCESSTRACE_HANDLE)
    {
        printf("OpenTrace failed: %lu\n", GetLastError());
        return;
    }

    ULONG status = ProcessTrace(&traceHandle, 1, nullptr, nullptr); // blocks until the session stops
    if (status != ERROR_SUCCESS)
        printf("ProcessTrace failed: %lu\n", status);

    g_dxTimingCaptureEventHandler->OnDataComplete(); // flush pending correlation
    g_dxTimingCaptureEventHandler->ReportTraceStatistics(trace);

    CloseTrace(traceHandle);
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

int __cdecl wmain(int argc, wchar_t** argv)
{
    SetConsoleCtrlHandler(ConsoleHandler, TRUE);

    // Usage: DxTimingCaptureLibrary.sample.perfetto.exe [targetProcessId|all] [seconds] [--all-categories] [--no-other-processes] [--etl <path>]
    // No targetProcessId means machine-wide: every D3D12 process gets its own tracks.
    std::optional<DWORD> targetProcessId;
    unsigned int runSeconds = 0;
    bool captureAllCategories = false;
    bool captureOtherProcesses = true;
    std::wstring etlPath;
    int positional = 0;
    for (int i = 1; i < argc; ++i)
    {
        std::wstring arg = argv[i];
        if (arg == L"--all-categories" || arg == L"-c")
        {
            captureAllCategories = true;
        }
        else if (arg == L"--no-other-processes")
        {
            captureOtherProcesses = false;
        }
        else if (arg == L"--etl")
        {
            if (i + 1 >= argc)
            {
                printf("--etl requires a path. Use --help for usage.\n");
                return 1;
            }
            etlPath = argv[++i];
        }
        else if (arg == L"--help" || arg == L"-h" || arg == L"/?")
        {
            wprintf(L"Usage: %ls [targetProcessId|all] [seconds] [--all-categories] [--no-other-processes] [--etl <path>]\n"
                    L"  targetProcessId   process to observe in detail; other processes appear\n"
                    L"                    only on the engine activity tracks\n"
                    L"  all               show every D3D12 process in detail; D3D12 object\n"
                    L"                    tracking is off in this mode (default)\n"
                    L"  seconds           auto-stop after N seconds (default: run until Ctrl+C)\n"
                    L"  --all-categories, -c\n"
                    L"                    also capture D3D12 object lifetimes, API command\n"
                    L"                    queues, and PSO compiles\n"
                    L"                    (without it: GPU Queue work and API Markers)\n"
                    L"  --no-other-processes\n"
                    L"                    drop the other-process GPU activity tracks, which\n"
                    L"                    are on by default. They cover the whole machine, so\n"
                    L"                    on a busy one they can crowd the target out of the\n"
                    L"                    trace buffer\n"
                    L"  --etl <path>      consume a previously-captured .etl file offline\n"
                    L"                    instead of a live session;\n"
                    L"                    'seconds' is ignored in this mode\n",
                    argv[0]);
            return 0;
        }
        else if (positional == 0)
        {
            // 'all' keeps the second positional (seconds) unambiguous when there is no target.
            if (arg != L"all")
            {
                DWORD parsedProcessId = static_cast<DWORD>(wcstoul(arg.c_str(), nullptr, 10));
                if (parsedProcessId == 0)
                {
                    wprintf(L"Invalid process id '%ls'. Use --help for usage.\n", arg.c_str());
                    return 1;
                }
                targetProcessId = parsedProcessId;
            }
            positional = 1;
        }
        else if (positional == 1)
        {
            runSeconds = static_cast<unsigned int>(wcstoul(arg.c_str(), nullptr, 10));
            positional = 2;
        }
    }
    const bool offline = !etlPath.empty();
    g_etlFilePath = etlPath;
    std::string categories = captureAllCategories ? "all categories" : "GPU Queue + API Markers";
    if (captureOtherProcesses)
        categories += " + other processes";
    if (offline)
    {
        if (targetProcessId)
            wprintf(L"Reading %ls, filtering to process %lu (%hs), writing dxtimingcapture.perfetto-trace\n",
                    etlPath.c_str(), *targetProcessId, categories.c_str());
        else
            wprintf(L"Reading %ls, all processes (%hs), writing dxtimingcapture.perfetto-trace\n",
                    etlPath.c_str(), categories.c_str());
    }
    else if (targetProcessId)
    {
        printf("Observing process %lu (%s), writing dxtimingcapture.perfetto-trace\n",
               *targetProcessId, categories.c_str());
    }
    else
    {
        printf("Observing all processes (%s), writing dxtimingcapture.perfetto-trace\n", categories.c_str());
    }

    g_etwSessionName = L"DxTimingCaptureLibrary.sample.perfetto.session" + std::to_wstring(GetCurrentProcessId());

    auto writer = std::make_unique<PerfettoTraceWriter>("dxtimingcapture.perfetto-trace", captureAllCategories);
    if (targetProcessId)
    {
        writer->MarkProcessDetailed(*targetProcessId);
    }

    DxTimingCaptureLibraryOptions libraryOptions{};
    // Object tracking is scoped to a single process, so it needs a target.
    libraryOptions.TrackApiObjects = targetProcessId.has_value();
    libraryOptions.TrackGpuTiming = true;
    libraryOptions.TrackGpuEngineActivity = captureOtherProcesses;

    QpcTimestampConverter timestampConverter = QpcTimestampConverter::ForCurrentMachine();
    PerfettoApiObjectCallbacks apiObjectCallbacks(*writer);
    PerfettoGpuTimingsCallbacks gpuTimingsCallbacks(*writer, targetProcessId);
    PerfettoDxgkObjectCallbacks dxgkObjectCallbacks(*writer);
    PerfettoPipelineStateEventCallbacks pipelineStateEventCallbacks(*writer);
    PerfettoGpuEngineActivityCallbacks gpuEngineActivityCallbacks(*writer);

    DxTimingCaptureEventCallbacks callbacks;
    callbacks.ApiObjectCallbacks = &apiObjectCallbacks;
    callbacks.GpuTimingsCallbacks = &gpuTimingsCallbacks;
    callbacks.DxgkObjectCallbacks = &dxgkObjectCallbacks;
    callbacks.PipelineStateEventCallbacks = &pipelineStateEventCallbacks;
    callbacks.GpuEngineActivityCallbacks = &gpuEngineActivityCallbacks;

    CountingDiagnosticsSink diagnosticsSink;
    callbacks.DiagnosticsSink = &diagnosticsSink;

    g_dxTimingCaptureEventHandler = DxTimingCaptureEventHandler::Create(targetProcessId.value_or(0), libraryOptions, callbacks, timestampConverter);

    if (offline)
    {
        // ProcessTrace consumes the whole .etl and returns; just wait for it.
        std::thread consumer(ConsumerThread);
        printf("Processing ETL file...\n");
        consumer.join();
    }
    else
    {
        EventTraceProperties properties{};
        auto u = ::StartTraceW(&g_sessionHandle, g_etwSessionName.c_str(), properties);
        if (u != ERROR_SUCCESS)
        {
            printf("StartTraceW failed: %lu\n", u);
            return 1;
        }

        EnableD3D12Provider(g_sessionHandle, libraryOptions.TrackApiObjects);
        EnableDxgkProvider(g_sessionHandle, libraryOptions.TrackGpuTiming, libraryOptions.TrackApiObjects);

        std::thread consumer(ConsumerThread);

        if (runSeconds)
            printf("Listening for %u seconds (or Ctrl+C)...\n", runSeconds);
        else
            printf("Listening for events (Ctrl+C to stop)...\n");

        unsigned int elapsedMs = 0;
        while (g_running)
        {
            Sleep(200);
            elapsedMs += 200;
            if (runSeconds && elapsedMs >= runSeconds * 1000)
                break;
        }

        printf("Stopping session...\n");
        u = ControlTraceW(g_sessionHandle, g_etwSessionName.c_str(), properties, EVENT_TRACE_CONTROL_STOP);
        if (u != ERROR_SUCCESS)
            printf("ControlTraceW failed: %lu\n", u);

        consumer.join();
    }

    writer->Finish();
    diagnosticsSink.PrintSummary();
    printf("Done\n");
    return 0;
}
