// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "pch.h"

#include <string>
#include <vector>

// MarkerOp.h (pulled in by GpuTimingData.h) expands the D3D12_MARKER_API_*
// constants from the D3D12 ETW manifest headers, so those must be included
// first — the library's own pch.h includes them in this same order.
#include <d3d12.h>
#include <D3D12Events.h>

#include <DxTimingCaptureLibrary/TicksToNanoseconds.h>
#include <DxTimingCaptureLibrary/GpuTimingData.h>

#include <DxTimingCaptureLibrary/DxTimingCaptureEventHandler.h>

using namespace DirectX::Etw;

#include "NoOpCallbacks.h"

namespace
{
    struct CapturedDiagnostic
    {
        DiagnosticSeverity Severity;
        DiagnosticCode Code;
        std::wstring Message;
    };

    // Records every diagnostic so a test can assert on what the library reported.
    class CapturingDiagnosticsSink : public DiagnosticsSink
    {
    public:
        std::vector<CapturedDiagnostic> Diagnostics;

        void OnDiagnostic(DiagnosticSeverity severity, DiagnosticCode code, std::wstring_view message) override
        {
            Diagnostics.push_back({ severity, code, std::wstring(message) });
        }
    };

    std::unique_ptr<DxTimingCaptureEventHandler> MakeHandler(TimestampConverter& converter, DiagnosticsSink* sink)
    {
        DxTimingCaptureEventCallbacks callbacks;
        callbacks.DiagnosticsSink = sink;
        return DxTimingCaptureEventHandler::Create(1234u, {}, callbacks, converter);
    }

    EVENT_TRACE_LOGFILE MakeLogfile(ULONG eventsLost, ULONG buffersLost)
    {
        EVENT_TRACE_LOGFILE logfile = {};
        logfile.LogfileHeader.EventsLost = eventsLost;
        logfile.LogfileHeader.BuffersLost = buffersLost;
        return logfile;
    }
}

// --- CalibratedClock diagnostics ------------------------------------------

TEST(CalibratedClockDiagnostics, InOrderCalibrationsReportNothing)
{
    CapturingDiagnosticsSink sink;
    CalibratedClock clock;

    clock.AddCalibration(1000000000, 100, 100, &sink);
    clock.AddCalibration(1000000000, 200, 200, &sink);
    clock.AddCalibration(1000000000, 300, 300, &sink);

    EXPECT_TRUE(sink.Diagnostics.empty());
}

TEST(CalibratedClockDiagnostics, BackwardsCpuTickReportsError)
{
    CapturingDiagnosticsSink sink;
    CalibratedClock clock;

    clock.AddCalibration(1000000000, 100, 200, &sink);
    clock.AddCalibration(1000000000, 200, 100, &sink); // CPU tick goes backwards

    ASSERT_EQ(1u, sink.Diagnostics.size());
    EXPECT_EQ(DiagnosticSeverity::Error, sink.Diagnostics[0].Severity);
    EXPECT_EQ(DiagnosticCode::ClockCalibrationCpuWentBackwards, sink.Diagnostics[0].Code);
}

TEST(CalibratedClockDiagnostics, BackwardsGpuTickReportsError)
{
    CapturingDiagnosticsSink sink;
    CalibratedClock clock;

    clock.AddCalibration(1000000000, 200, 100, &sink);
    clock.AddCalibration(1000000000, 100, 200, &sink); // GPU tick goes backwards

    ASSERT_EQ(1u, sink.Diagnostics.size());
    EXPECT_EQ(DiagnosticSeverity::Error, sink.Diagnostics[0].Severity);
    EXPECT_EQ(DiagnosticCode::ClockCalibrationGpuWentBackwards, sink.Diagnostics[0].Code);
}

// --- Lost-event reporting --------------------------------------------------

TEST(LostEventReporting, NoLossReportsNothing)
{
    NoOpTimestampConverter converter;
    CapturingDiagnosticsSink sink;
    auto handler = MakeHandler(converter, &sink);

    handler->ReportTraceStatistics(MakeLogfile(0, 0));

    EXPECT_TRUE(sink.Diagnostics.empty());
}

TEST(LostEventReporting, NewLossReportsErrorsAndAccumulates)
{
    NoOpTimestampConverter converter;
    CapturingDiagnosticsSink sink;
    auto handler = MakeHandler(converter, &sink);

    handler->ReportTraceStatistics(MakeLogfile(5, 2));

    ASSERT_EQ(2u, sink.Diagnostics.size());
    EXPECT_EQ(DiagnosticCode::EventsLost, sink.Diagnostics[0].Code);
    EXPECT_EQ(DiagnosticCode::BuffersLost, sink.Diagnostics[1].Code);
    EXPECT_EQ(DiagnosticSeverity::Error, sink.Diagnostics[0].Severity);
}

TEST(LostEventReporting, CumulativeCountsReportOnlyTheDelta)
{
    NoOpTimestampConverter converter;
    CapturingDiagnosticsSink sink;
    auto handler = MakeHandler(converter, &sink);

    // The logfile header carries cumulative totals, so re-reporting the same
    // totals must not re-report a loss, and only the increase should fire.
    handler->ReportTraceStatistics(MakeLogfile(5, 0));
    handler->ReportTraceStatistics(MakeLogfile(5, 0)); // unchanged: silent
    handler->ReportTraceStatistics(MakeLogfile(8, 0)); // +3 events

    ASSERT_EQ(2u, sink.Diagnostics.size());
    EXPECT_EQ(DiagnosticCode::EventsLost, sink.Diagnostics[0].Code);
    EXPECT_EQ(DiagnosticCode::EventsLost, sink.Diagnostics[1].Code);
}
