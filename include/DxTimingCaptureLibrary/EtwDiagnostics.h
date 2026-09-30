// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#pragma once

// Diagnostics types for errors/info that the library hits, that are reported to your
// DiagnosticsSink interface. Note that implementing the sink is optional.
//
// Examples of errors/info reported: an unmatched GPU EndEvent, a backwards clock
// calibration, lost ETW events, ...

#include <cstdint>
#include <string_view>

namespace DirectX::Etw
{
    enum class DiagnosticSeverity
    {
        // A problem the library worked around; data may be slightly incomplete.
        Warning,
        // A problem that means some data is missing or could not be trusted.
        Error,
    };

    enum class DiagnosticCode
    {
        // A GPU EndEvent arrived with no matching BeginEvent on the command list/queue.
        GpuEndEventWithoutBeginEvent,

        // A command list execution had no matching D3D12 command queue, so some
        // GPU timing data is missing.
        CommandListExecutionMissingQueue,

        // A computed end-of-pipe timestamp preceded its top-of-pipe timestamp.
        TimestampEndBeforeStart,

        // A clock calibration reported a CPU tick earlier than the previous one.
        ClockCalibrationCpuWentBackwards,

        // A clock calibration reported a GPU tick earlier than the previous one.
        ClockCalibrationGpuWentBackwards,

        // A pipeline-state compilation start event occured before its API
        // object id arrived, so the in-flight compilation was dropped.
        PipelineStateCompilationStartWithoutObjectId,

        // A pipeline-state compilation end event arrived with no matching start.
        PipelineStateCompilationEndWithoutStart,

        // The ETW session reported that event records were lost (buffer overrun).
        EventsLost,

        // The ETW session reported that whole buffers were lost.
        BuffersLost,

        // Command list executions were still waiting on events that never arrived
        // when the trace ended, so their GPU timing data is missing.
        CommandListExecutionsUnfinishedAtEndOfTrace,
    };

    // DiagnosticsSink: this is the interface you can optionally implement, to be told
    // when errors occur.
    class DiagnosticsSink
    {
    public:
        virtual ~DiagnosticsSink() = default;

        virtual void OnDiagnostic(
            DiagnosticSeverity severity,
            DiagnosticCode code,
            std::wstring_view message) = 0;
    };

    // Running totals of ETW data loss observed for a trace.
    struct LostEventStatistics
    {
        uint64_t EventsLost = 0;
        uint64_t BuffersLost = 0;
    };

    // Default sink used when a consumer supplies none
    class NullDiagnosticsSink final : public DiagnosticsSink
    {
    public:
        void OnDiagnostic(DiagnosticSeverity, DiagnosticCode, std::wstring_view) override
        {
        }
    };
}
