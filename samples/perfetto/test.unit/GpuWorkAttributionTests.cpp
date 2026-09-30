// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "pch.h"

#include <filesystem>
#include <string>

#include "PerfettoCallbacks.h"
#include "PerfettoTraceReader.h"
#include "PerfettoTraceWriter.h"

using namespace DxTimingCaptureLibraryTest;

// Covers how the perfetto sample attributes GPU work to processes. The library
// decodes GPU timing machine-wide, so everything here - which track a span lands
// on, whose name is on it, and which mode the target process id selects - is the
// sample's own policy, and none of it is exercised by the library's tests.
//
// The callbacks are driven directly rather than through an ETW session: the
// scenario that matters is two D3D12 apps sharing a hardware engine, which a live
// session cannot be made to produce on demand.

namespace
{
    constexpr UINT32 FirstProcessId = 4001;
    constexpr UINT32 SecondProcessId = 4002;

    // The two processes submit on their own API command queues but share the engine,
    // which is what put them on a single track before the sample keyed tracks per queue.
    constexpr UINT64 SharedHardwareQueueId = 77;
    constexpr UINT64 FirstApiQueueId = 100;
    constexpr UINT64 SecondApiQueueId = 200;

    // The sample resolves a process to its image name; ids that belong to no live
    // process - as these deliberately do - fall back to this.
    std::string ProcessTag(UINT32 processId)
    {
        return "PID " + std::to_string(processId);
    }

    std::string CurrentProcessImageName()
    {
        wchar_t path[MAX_PATH]{};
        GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));
        return std::filesystem::path(path).filename().string();
    }

    // Writes to a file named after the running test so a failure leaves the trace
    // behind to look at, and two tests never collide.
    class TraceFile
    {
        std::filesystem::path m_path;

    public:
        TraceFile()
        {
            const auto* test = ::testing::UnitTest::GetInstance()->current_test_info();
            m_path = std::filesystem::temp_directory_path() /
                (std::string("DxTimingCaptureLibrary.test.") + test->test_suite_name() + "." + test->name() + ".perfetto-trace");
            std::filesystem::remove(m_path);
        }

        std::string Path() const { return m_path.string(); }
    };

    // Drives the sample's callbacks the way the library would, so the tests describe
    // a scenario ("this process submitted work on that queue") rather than the
    // writer calls it turns into.
    class SampleUnderTest
    {
        TraceFile m_file;
        PerfettoTraceWriter m_writer;
        PerfettoGpuTimingsCallbacks m_gpuTimings;
        PerfettoGpuEngineActivityCallbacks m_engineActivity;
        INT64 m_nextTimestamp = 1'000'000;

    public:
        // targetProcessId empty means the machine-wide mode.
        explicit SampleUnderTest(std::optional<UINT32> targetProcessId)
            : m_writer(m_file.Path(), /*captureAllCategories*/ false)
            , m_gpuTimings(m_writer, targetProcessId)
            , m_engineActivity(m_writer)
        {
            if (targetProcessId)
            {
                m_writer.MarkProcessDetailed(*targetProcessId);
            }
            m_writer.NameHwQueue(SharedHardwareQueueId, L"3D");
        }

        // One command list execution, start to finish, as the library reports it.
        void SubmitGpuWork(UINT32 processId, UINT64 apiQueueId, UINT64 hardwareQueueId = SharedHardwareQueueId)
        {
            UINT64 executionId = 0;
            m_gpuTimings.OnGpuExecutionBegin(processId, 0, apiQueueId, m_nextTimestamp, 0, &executionId);
            SubmitGpuWorkForExecution(executionId, hardwareQueueId);
            m_gpuTimings.OnGpuExecutionComplete(executionId);
        }

        void SubmitGpuWorkForExecution(UINT64 executionId, UINT64 hardwareQueueId = SharedHardwareQueueId)
        {
            const INT64 begin = m_nextTimestamp;
            const INT64 end = begin + 500'000;
            m_nextTimestamp = end + 500'000;
            m_gpuTimings.OnGpuWork(hardwareQueueId, executionId, 0, 0, begin, end);
        }

        // A DMA packet the scheduler ran, which the library reports for every process.
        void RunGpuPacket(UINT32 processId, UINT64 hardwareQueueId = SharedHardwareQueueId)
        {
            const INT64 begin = m_nextTimestamp;
            const INT64 end = begin + 500'000;
            m_nextTimestamp = end + 500'000;
            m_engineActivity.OnGpuEngineActivity(processId, hardwareQueueId, 0, begin, end, true);
        }

        ParsedTrace Finish()
        {
            m_writer.Finish();
            return ReadPerfettoTrace(m_file.Path());
        }
    };
}

// The bug this suite was written for: both processes ran on the same engine, so
// keying tracks off the engine drew their work as one interleaved timeline.
TEST(PerfettoSampleTests, ProcessesSharingAnEngineGetTheirOwnTracks)
{
    SampleUnderTest sample(std::nullopt);
    sample.SubmitGpuWork(FirstProcessId, FirstApiQueueId);
    sample.SubmitGpuWork(SecondProcessId, SecondApiQueueId);

    const ParsedTrace trace = sample.Finish();

    EXPECT_TRUE(trace.HasTrackNameContaining("GPU Queue: 3D - " + ProcessTag(FirstProcessId)))
        << "Expected a GPU Queue track labelled with the first process";
    EXPECT_TRUE(trace.HasTrackNameContaining("GPU Queue: 3D - " + ProcessTag(SecondProcessId)))
        << "Expected a GPU Queue track labelled with the second process";
    EXPECT_EQ(trace.CountTracksContaining("GPU Queue: 3D"), 2u)
        << "Two processes on one engine should produce exactly two GPU Queue tracks";
}

// Without a target every D3D12 process is drawn in detail.
TEST(PerfettoSampleTests, EveryProcessIsDetailedWithoutATargetProcess)
{
    SampleUnderTest sample(std::nullopt);
    sample.SubmitGpuWork(FirstProcessId, FirstApiQueueId);
    sample.SubmitGpuWork(SecondProcessId, SecondApiQueueId);

    const ParsedTrace trace = sample.Finish();

    EXPECT_EQ(trace.SlicesOnTracksContaining(ProcessTag(FirstProcessId)).size(), 1u);
    EXPECT_EQ(trace.SlicesOnTracksContaining(ProcessTag(SecondProcessId)).size(), 1u);
}

// With a target, everyone else is left to the engine activity tracks.
TEST(PerfettoSampleTests, OnlyTheTargetProcessIsDetailed)
{
    SampleUnderTest sample(FirstProcessId);
    sample.SubmitGpuWork(FirstProcessId, FirstApiQueueId);
    sample.SubmitGpuWork(SecondProcessId, SecondApiQueueId);

    const ParsedTrace trace = sample.Finish();

    EXPECT_EQ(trace.CountTracksContaining("GPU Queue: 3D"), 1u)
        << "Only the target process should get a detailed GPU Queue track";
    EXPECT_TRUE(trace.HasTrackNameContaining("GPU Queue: 3D - " + ProcessTag(FirstProcessId)));
    EXPECT_TRUE(trace.SlicesOnTracksContaining(ProcessTag(SecondProcessId)).empty())
        << "A non-target process should not be drawn in detail";
}

// The same work measured twice - once as command list spans, once as submit-to-retire
// packets - reads as if the process ran everything twice.
TEST(PerfettoSampleTests, ADetailedProcessIsNotAlsoDrawnAsEngineActivity)
{
    SampleUnderTest sample(FirstProcessId);
    sample.SubmitGpuWork(FirstProcessId, FirstApiQueueId);
    sample.RunGpuPacket(FirstProcessId);
    sample.RunGpuPacket(SecondProcessId);

    const ParsedTrace trace = sample.Finish();

    const auto engineActivity = trace.SlicesOnTracksContaining("other/unknown processes");
    ASSERT_EQ(engineActivity.size(), 1u) << "Only the process without a detailed track belongs here";
    EXPECT_NE(engineActivity[0].Name.find(ProcessTag(SecondProcessId)), std::string::npos);
}

// Engine activity can arrive before a process has submitted anything, which would
// otherwise draw the target on the other-process track until its first execution.
TEST(PerfettoSampleTests, TheTargetProcessIsClaimedBeforeItsFirstGpuWork)
{
    SampleUnderTest sample(FirstProcessId);
    sample.RunGpuPacket(FirstProcessId);

    const ParsedTrace trace = sample.Finish();

    EXPECT_TRUE(trace.SlicesOnTracksContaining("other/unknown processes").empty())
        << "The target process should be claimed for detailed tracks up front";
}

// Two queues in one process are two timelines; merging them would make independent
// work look serialized.
TEST(PerfettoSampleTests, QueuesWithinAProcessGetTheirOwnTracks)
{
    SampleUnderTest sample(FirstProcessId);
    sample.SubmitGpuWork(FirstProcessId, FirstApiQueueId);
    sample.SubmitGpuWork(FirstProcessId, SecondApiQueueId);

    const ParsedTrace trace = sample.Finish();

    EXPECT_EQ(trace.CountTracksContaining("GPU Queue: 3D - " + ProcessTag(FirstProcessId)), 2u)
        << "Each API command queue should get a track of its own";
    EXPECT_EQ(trace.CountTrackNamesContaining("GPU Queue: 3D - " + ProcessTag(FirstProcessId)), 1u)
        << "Both queues run on the same engine, so both tracks carry the same label";
    EXPECT_EQ(trace.SlicesOnTracksContaining(ProcessTag(FirstProcessId)).size(), 2u);
}

// OnGpuWork carries no process id, so work whose execution was never seen cannot be
// attributed and must not be guessed onto a track.
TEST(PerfettoSampleTests, GpuWorkWithoutItsExecutionIsDropped)
{
    SampleUnderTest sample(std::nullopt);
    sample.SubmitGpuWorkForExecution(/*executionId*/ 999999);

    const ParsedTrace trace = sample.Finish();

    EXPECT_FALSE(trace.HasTrackNameContaining("GPU Queue"))
        << "Unattributable GPU work should not create a track";
}

// The engine activity tracks name processes the tracing process never opened, which
// is what makes them readable at all.
TEST(PerfettoSampleTests, EngineActivityNamesTheProcessThatRanIt)
{
    SampleUnderTest sample(FirstProcessId);
    sample.RunGpuPacket(GetCurrentProcessId());

    const ParsedTrace trace = sample.Finish();

    const auto engineActivity = trace.SlicesOnTracksContaining("other/unknown processes");
    ASSERT_EQ(engineActivity.size(), 1u);
    EXPECT_NE(engineActivity[0].Name.find(CurrentProcessImageName()), std::string::npos)
        << "Expected the image name, got: " << engineActivity[0].Name;
}

// A packet the library could not attribute is still worth drawing, but must not be
// labelled as if it belonged to a process.
TEST(PerfettoSampleTests, UnattributedEngineActivityIsLabelledAsSuch)
{
    SampleUnderTest sample(FirstProcessId);
    sample.RunGpuPacket(/*processId*/ 0);

    const ParsedTrace trace = sample.Finish();

    const auto engineActivity = trace.SlicesOnTracksContaining("other/unknown processes");
    ASSERT_EQ(engineActivity.size(), 1u);
    EXPECT_EQ(engineActivity[0].Name, "unattributed");
}
