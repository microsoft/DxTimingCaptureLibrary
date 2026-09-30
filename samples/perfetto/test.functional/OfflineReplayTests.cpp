// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "pch.h"

#include "EtlRecording.h"
#include "SampleProcess.h"

using namespace DxTimingCaptureLibraryTest;

// Replays one recorded ETW trace through the sample's --etl mode. Every test sees
// exactly the same events, so modes can be compared against each other rather than
// against two different live captures.

namespace
{
    constexpr unsigned int RecordingSeconds = 5;

    // The trace's own clock has nanosecond resolution, and the writer re-bases it
    // onto perfetto's clock, so only distances within the trace mean anything.
    constexpr uint64_t NanosecondsPerSecond = 1'000'000'000ull;

    // Generous: the recording is bracketed by session start-up and provider rundown,
    // and rundown events carry timestamps from before the session began.
    constexpr uint64_t AllowedSlackSeconds = 60;
}

class PerfettoSampleOffline : public ::testing::Test
{
protected:
    static inline std::filesystem::path s_directory;
    static inline EtlRecording s_recording;

    static void SetUpTestSuite()
    {
        s_directory = std::filesystem::temp_directory_path() / "DxTimingCaptureLibrary.sample.perfetto.offline";
        std::error_code errorCode;
        std::filesystem::create_directories(s_directory, errorCode);

        s_recording = RecordGpuWorkToEtl(s_directory / "capture.etl", RecordingSeconds);
    }

    static void TearDownTestSuite()
    {
        std::error_code errorCode;
        std::filesystem::remove_all(s_directory, errorCode);
    }

    void SetUp() override
    {
        if (!s_recording.Succeeded)
        {
            GTEST_SKIP() << "Could not record an ETW trace; run the tests elevated.";
        }

        if (s_recording.EventsLost != 0)
        {
            GTEST_SKIP() << "The recording lost " << s_recording.EventsLost
                         << " events, so what is missing from it means nothing.";
        }
    }

    SampleInvocation Replay(const std::wstring& sampleArguments) const
    {
        const SampleInvocation invocation =
            RunSampleToCompletion(sampleArguments + L" --etl \"" + s_recording.Path.wstring() + L"\"");
        EXPECT_EQ(0u, invocation.ExitCode) << invocation.Output;
        return invocation;
    }

    // A dead process cannot be named, and replay always happens after the children
    // have exited, so the sample falls back to their process ids.
    static std::string FirstChildLabel()
    {
        return "PID " + std::to_string(s_recording.FirstChildProcessId);
    }

    static std::string SecondChildLabel()
    {
        return "PID " + std::to_string(s_recording.SecondChildProcessId);
    }
};

TEST_F(PerfettoSampleOffline, AnEtlFileCanBeReplayedIntoATrace)
{
    const SampleInvocation replay = Replay(L"all");

    EXPECT_TRUE(replay.Trace.HasTrackNameContaining("GPU Queue")) << replay.Trace.TrackNames() << replay.Output;
    EXPECT_FALSE(replay.Trace.Slices.empty()) << replay.Trace.TrackNames() << replay.Output;
}

TEST_F(PerfettoSampleOffline, BothRecordedProcessesAreTrackedInDetail)
{
    const SampleInvocation replay = Replay(L"all");

    EXPECT_TRUE(replay.Trace.HasTrackNameContaining(FirstChildLabel())) << replay.Trace.TrackNames() << replay.Output;
    EXPECT_TRUE(replay.Trace.HasTrackNameContaining(SecondChildLabel())) << replay.Trace.TrackNames() << replay.Output;
}

TEST_F(PerfettoSampleOffline, TheTargetIsTheOnlyProcessTrackedInDetail)
{
    const SampleInvocation replay = Replay(std::to_wstring(s_recording.FirstChildProcessId));

    EXPECT_TRUE(replay.Trace.HasTrackNameContaining(FirstChildLabel())) << replay.Trace.TrackNames() << replay.Output;
    EXPECT_FALSE(replay.Trace.HasTrackNameContaining(SecondChildLabel()))
        << "Only the target should get a track of its own\n" << replay.Trace.TrackNames() << replay.Output;
}

// The same events, read twice: naming a target has to narrow what is tracked in
// detail, and nothing else.
TEST_F(PerfettoSampleOffline, TargetingAProcessNarrowsWhatIsTrackedInDetail)
{
    const SampleInvocation targeted = Replay(std::to_wstring(s_recording.FirstChildProcessId));
    const SampleInvocation everything = Replay(L"all");

    EXPECT_GT(everything.Trace.CountTrackNamesContaining("GPU Queue"),
              targeted.Trace.CountTrackNamesContaining("GPU Queue"))
        << "Naming a target should leave fewer detailed tracks than 'all' on the same events";

    EXPECT_TRUE(targeted.Trace.HasTrackNameContaining("other/unknown processes"))
        << "The work of the processes that were not named still belongs in the trace\n" << targeted.Output;
}

TEST_F(PerfettoSampleOffline, NoSpanOutlastsTheRecording)
{
    const SampleInvocation replay = Replay(L"all");

    const uint64_t longestAllowedNs = (RecordingSeconds + AllowedSlackSeconds) * NanosecondsPerSecond;
    const TraceSlice* longest = nullptr;
    for (const TraceSlice& slice : replay.Trace.Slices)
    {
        if (longest == nullptr || slice.DurationNs() > longest->DurationNs())
        {
            longest = &slice;
        }
    }
    ASSERT_NE(longest, nullptr) << replay.Trace.TrackNames() << replay.Output;

    EXPECT_LE(longest->DurationNs(), longestAllowedNs)
        << "'" << longest->Name << "' on '" << longest->TrackName << "' lasts "
        << longest->DurationNs() / NanosecondsPerSecond << "s, longer than the " << RecordingSeconds
        << "s recording. A span that never ends wedges a lane and makes the writer drop later work.";
}

TEST_F(PerfettoSampleOffline, TheTraceCoversOnlyTheRecordingWindow)
{
    const SampleInvocation replay = Replay(L"all");

    const uint64_t widestAllowedNs = (RecordingSeconds + AllowedSlackSeconds) * NanosecondsPerSecond;
    EXPECT_LE(replay.Trace.TimeExtentNs(), widestAllowedNs)
        << "The trace spans " << replay.Trace.TimeExtentNs() / NanosecondsPerSecond
        << "s for a " << RecordingSeconds << "s recording, so some event carries a bad timestamp.";
}
