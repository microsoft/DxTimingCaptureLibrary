// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "pch.h"

#include "GpuWorkload.h"
#include "SampleProcess.h"

using namespace DxTimingCaptureLibraryTest;

// Runs the sample as its own process against a live ETW session and checks the
// trace it writes. The unit tests next door drive the same policy through the
// sample's classes; these cover the parts only a real run exercises - the command
// line, the session, and whether real GPU work lands where it should.

namespace
{
    // Long enough for the session to settle and for the workload to be seen, short
    // enough that a handful of these do not dominate the suite.
    constexpr unsigned int CaptureSeconds = 8;

    // The GPU workload runs for most of the capture so the sample sees several
    // submits and can still resolve the process while it is alive.
    constexpr unsigned int WorkloadSeconds = 5;

    constexpr char OtherProcessTrackFragment[] = "other/unknown processes";

    void SubmitGpuWorkForAWhile()
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(WorkloadSeconds);
        do
        {
            SubmitGpuWorkAndWait();
            Sleep(16);
        } while (std::chrono::steady_clock::now() < deadline);
    }

    std::string ThisProcessLabel()
    {
        return SampleProcessLabel(GetCurrentProcessId());
    }

    // A machine without ETW rights, or without a working D3D12 device, cannot say
    // anything about attribution. Neither is a failure of the sample.
    void SkipUnlessGpuWorkWasCaptured(const SampleRun& run)
    {
        if (!run.SessionStarted)
        {
            GTEST_SKIP() << "The sample could not start an ETW session; run the tests elevated.\n" << run.Output;
        }

        if (!run.Trace.HasTrackNameContaining("GPU Queue"))
        {
            GTEST_SKIP() << "No GPU work was captured on this machine.\n" << run.Output;
        }
    }
}

TEST(PerfettoSampleProcess, TargetProcessGetsItsOwnGpuQueueTrack)
{
    const SampleRun run = RunSample(std::to_wstring(GetCurrentProcessId()), CaptureSeconds, SubmitGpuWorkForAWhile);
    SkipUnlessGpuWorkWasCaptured(run);

    EXPECT_EQ(0u, run.ExitCode);
    EXPECT_TRUE(run.Trace.HasTrackNameContaining(ThisProcessLabel()))
        << "Expected a GPU Queue track for " << ThisProcessLabel() << "\n" << run.Output;
}

TEST(PerfettoSampleProcess, ProcessesOtherThanTheTargetGetNoTrackOfTheirOwn)
{
    unsigned long childProcessId = 0;
    const SampleRun run = RunSample(std::to_wstring(GetCurrentProcessId()), CaptureSeconds, [&]
    {
        GpuWorkChildProcess child(WorkloadSeconds);
        childProcessId = child.ProcessId();
        SubmitGpuWorkForAWhile();
        child.WaitForSuccess();
    });
    SkipUnlessGpuWorkWasCaptured(run);

    EXPECT_TRUE(run.Trace.HasTrackNameContaining(ThisProcessLabel()))
        << "The target process should still be tracked in detail\n" << run.Output;
    EXPECT_FALSE(run.Trace.HasTrackNameContaining(SampleProcessLabel(childProcessId)))
        << "Only the target process should get a track of its own\n" << run.Output;
}

TEST(PerfettoSampleProcess, WorkFromAnotherProcessShowsUpOnTheSharedTrack)
{
    unsigned long childProcessId = 0;
    const SampleRun run = RunSample(std::to_wstring(GetCurrentProcessId()), CaptureSeconds, [&]
    {
        GpuWorkChildProcess child(WorkloadSeconds);
        childProcessId = child.ProcessId();
        SubmitGpuWorkForAWhile();
        child.WaitForSuccess();
    });
    SkipUnlessGpuWorkWasCaptured(run);

    if (!run.Trace.HasTrackNameContaining(OtherProcessTrackFragment))
    {
        GTEST_SKIP() << "No other-process GPU activity was captured on this machine.\n" << run.Output;
    }

    const auto slices = run.Trace.SlicesOnTracksContaining(OtherProcessTrackFragment);
    const std::string childLabel = SampleProcessLabel(childProcessId);
    const bool childIsOnTheSharedTrack = std::ranges::any_of(slices, [&](const TraceSlice& slice)
    {
        return slice.Name == childLabel;
    });

    EXPECT_TRUE(childIsOnTheSharedTrack)
        << "Expected " << childLabel << " among the " << slices.size() << " slices on the shared track\n" << run.Output;
}

TEST(PerfettoSampleProcess, EveryProcessGetsATrackWhenNoTargetIsGiven)
{
    unsigned long childProcessId = 0;
    const SampleRun run = RunSample(L"all", CaptureSeconds, [&]
    {
        GpuWorkChildProcess child(WorkloadSeconds);
        childProcessId = child.ProcessId();
        SubmitGpuWorkForAWhile();
        child.WaitForSuccess();
    });
    SkipUnlessGpuWorkWasCaptured(run);

    EXPECT_TRUE(run.Trace.HasTrackNameContaining(ThisProcessLabel()))
        << "Expected a GPU Queue track for " << ThisProcessLabel() << "\n" << run.Output;
    EXPECT_TRUE(run.Trace.HasTrackNameContaining(SampleProcessLabel(childProcessId)))
        << "Expected a GPU Queue track for " << SampleProcessLabel(childProcessId) << "\n" << run.Output;
}

TEST(PerfettoSampleProcess, SharedTracksAreDroppedWhenAskedTo)
{
    const SampleRun run = RunSample(
        std::to_wstring(GetCurrentProcessId()) + L" --no-other-processes",
        CaptureSeconds,
        SubmitGpuWorkForAWhile);
    SkipUnlessGpuWorkWasCaptured(run);

    EXPECT_TRUE(run.Trace.HasTrackNameContaining(ThisProcessLabel()))
        << "The target process should still be tracked in detail\n" << run.Output;
    EXPECT_FALSE(run.Trace.HasTrackNameContaining(OtherProcessTrackFragment))
        << "--no-other-processes should leave no shared tracks\n" << run.Output;
}
