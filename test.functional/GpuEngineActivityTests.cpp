// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "pch.h"

#include <algorithm>
#include <chrono>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include <DxTimingCaptureLibrary/DxTimingCaptureEventHandler.h>
#include "EtwSession.h"
#include "GpuWorkload.h"

using namespace DirectX::Etw;

// Live-device tests for the GPU engine activity path. The unit tests pair packets
// from hand-built payloads, which says nothing about whether the events DxgKrnl
// actually emits line up.
//
// There are four cases because two things change how a packet resolves: whether
// the work came from this process or another one, and which scheduler ran it -
// the two event pairs are keyed off different handles.

namespace
{
    struct ReportedPacket
    {
        UINT32 ProcessId;
        UINT64 HardwareCommandQueueId;
        INT64 SubmitTimestamp;
        INT64 CompleteTimestamp;
        bool HardwareScheduled;
    };

    class RecordingGpuEngineActivityCallbacks : public GpuEngineActivityCallbacks
    {
    public:
        std::vector<ReportedPacket> Packets;

        HRESULT OnGpuEngineActivity(
            UINT32 processId,
            UINT64 hardwareCommandQueueId,
            UINT32,
            INT64 submitTimestamp,
            INT64 completeTimestamp,
            bool hardwareScheduled) override
        {
            Packets.push_back({ processId, hardwareCommandQueueId, submitTimestamp, completeTimestamp, hardwareScheduled });
            return S_OK;
        }
    };

    struct UniqueHandle
    {
        HANDLE Value = nullptr;

        ~UniqueHandle()
        {
            if (Value != nullptr)
            {
                CloseHandle(Value);
            }
        }
    };

    struct ChildProcessResult
    {
        DWORD ProcessId;
        DWORD ExitCode;
    };

    std::wstring GetTestExecutablePath()
    {
        wchar_t path[MAX_PATH]{};
        const DWORD lengthInCharacters = GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));
        ThrowIf(lengthInCharacters == 0 || lengthInCharacters == ARRAYSIZE(path), E_FAIL, L"GetModuleFileNameW failed");
        return std::wstring(path, lengthInCharacters);
    }

    // Runs this executable again in its GPU work mode. Returns the exit code alongside
    // the process id so the caller can tell "nothing was attributed to the child" apart
    // from "the child never managed to submit anything".
    ChildProcessResult RunGpuWorkInChildProcess(DxTimingCaptureLibraryTest::GpuAdapterKind adapter)
    {
        constexpr DWORD ChildTimeoutInMilliseconds = 60'000;

        std::wstring commandLine = L"\"" + GetTestExecutablePath() + L"\" " + DxTimingCaptureLibraryTest::SubmitGpuWorkSwitchW;
        if (adapter == DxTimingCaptureLibraryTest::GpuAdapterKind::Warp)
        {
            commandLine += L" ";
            commandLine += DxTimingCaptureLibraryTest::WarpAdapterSwitchW;
        }

        STARTUPINFOW startupInfo{};
        startupInfo.cb = sizeof(startupInfo);
        PROCESS_INFORMATION processInformation{};
        const BOOL created = CreateProcessW(
            nullptr,
            commandLine.data(),
            nullptr,
            nullptr,
            FALSE,
            0,
            nullptr,
            nullptr,
            &startupInfo,
            &processInformation);
        ThrowIf(!created, HRESULT_FROM_WIN32(GetLastError()), L"CreateProcessW failed");

        UniqueHandle process{ processInformation.hProcess };
        UniqueHandle thread{ processInformation.hThread };

        ThrowIf(
            WaitForSingleObject(process.Value, ChildTimeoutInMilliseconds) != WAIT_OBJECT_0,
            E_FAIL,
            L"Timed out waiting for the GPU work child process");

        DWORD exitCode = 0;
        ThrowIf(!GetExitCodeProcess(process.Value, &exitCode), E_FAIL, L"GetExitCodeProcess failed");

        return { processInformation.dwProcessId, exitCode };
    }

    // Traces the given work and returns every packet reported while it ran.
    std::vector<ReportedPacket> CapturePacketsAround(const std::function<void()>& submitWork)
    {
        auto callbacks = std::make_unique<RecordingGpuEngineActivityCallbacks>();
        auto* recorder = callbacks.get();

        SimpleEtwSession session;
        // Deliberately the only callback set, so TrackGpuTiming stays off. Attribution
        // rides on the DXGK context and hardware queue handlers, which run regardless -
        // if that ever stops being true, these tests are how we find out.
        session.SetGpuEngineActivityCallbacks(std::move(callbacks));
        session.Begin();

        submitWork();

        // ETW delivery is asynchronous; give the completion events time to reach the
        // consumer thread before the session stops the trace.
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        session.End();
        session.RethrowConsumerException();

        return std::move(recorder->Packets);
    }

    std::vector<ReportedPacket> PacketsFrom(const std::vector<ReportedPacket>& packets, DWORD processId)
    {
        std::vector<ReportedPacket> matching;
        std::ranges::copy_if(
            packets,
            std::back_inserter(matching),
            [processId](const ReportedPacket& packet) { return packet.ProcessId == processId; });
        return matching;
    }

    void ExpectSpansAreOrdered(const std::vector<ReportedPacket>& packets)
    {
        const bool everySpanIsOrdered = std::ranges::all_of(
            packets,
            [](const ReportedPacket& packet) { return packet.CompleteTimestamp > packet.SubmitTimestamp; });
        EXPECT_TRUE(everySpanIsOrdered) << "A packet completed at or before it was submitted";
    }

    void ExpectScheduledInHardware(const std::vector<ReportedPacket>& packets, bool expected)
    {
        const bool sawExpectedScheduler = std::ranges::any_of(
            packets,
            [expected](const ReportedPacket& packet) { return packet.HardwareScheduled == expected; });
        EXPECT_TRUE(sawExpectedScheduler)
            << "Expected at least one packet from the " << (expected ? "hardware-scheduled" : "legacy") << " path";
    }
}

TEST(GpuEngineActivityTests, GpuWorkFromThisProcessIsAttributedToIt)
{
    const auto packets = CapturePacketsAround([] { DxTimingCaptureLibraryTest::SubmitGpuWorkAndWait(); });

    if (packets.empty())
    {
        GTEST_SKIP() << "No DMA packets were captured in this environment.";
    }

    const auto ourPackets = PacketsFrom(packets, GetCurrentProcessId());
    EXPECT_FALSE(ourPackets.empty()) << "Expected the copy submitted above to be attributed to this process";
    ExpectSpansAreOrdered(ourPackets);
}

// The case that actually matters for interference detection: GPU work the tracing
// process did not submit, tied back to the process that did.
TEST(GpuEngineActivityTests, GpuWorkFromAnotherProcessIsAttributedToIt)
{
    ChildProcessResult child{};
    const auto packets = CapturePacketsAround(
        // Launched after the session starts, so its contexts are created inside the
        // trace and its packets can be resolved back to it.
        [&child] { child = RunGpuWorkInChildProcess(DxTimingCaptureLibraryTest::GpuAdapterKind::Default); });

    if (child.ExitCode == DxTimingCaptureLibraryTest::SubmitGpuWorkFailedExitCode)
    {
        GTEST_SKIP() << "The child process could not submit GPU work in this environment.";
    }
    ASSERT_EQ(child.ExitCode, 0u) << "The GPU work child process failed unexpectedly";

    const auto childPackets = PacketsFrom(packets, child.ProcessId);
    if (packets.empty())
    {
        GTEST_SKIP() << "No DMA packets were captured in this environment.";
    }

    EXPECT_FALSE(childPackets.empty())
        << "Expected GPU work submitted by process " << child.ProcessId << " to be attributed to it";
    ExpectSpansAreOrdered(childPackets);
}

// The scheduler picks the event pair, and a machine with a hardware-scheduling GPU
// never raises the legacy one. WARP is a software adapter, so it is how this suite
// reaches that path at all.
TEST(GpuEngineActivityTests, WarpWorkFromThisProcessArrivesOnTheLegacyPath)
{
    const auto packets = CapturePacketsAround(
        [] { DxTimingCaptureLibraryTest::SubmitGpuWorkAndWait(DxTimingCaptureLibraryTest::GpuAdapterKind::Warp); });

    if (packets.empty())
    {
        GTEST_SKIP() << "No DMA packets were captured in this environment.";
    }

    const auto ourPackets = PacketsFrom(packets, GetCurrentProcessId());
    ASSERT_FALSE(ourPackets.empty()) << "Expected the WARP work submitted above to be attributed to this process";

    ExpectScheduledInHardware(ourPackets, false);
    ExpectSpansAreOrdered(ourPackets);
}

// The legacy path keys packets off a different handle than the hardware-scheduled
// one, so proving attribution there needs its own out-of-process run.
TEST(GpuEngineActivityTests, WarpWorkFromAnotherProcessIsAttributedToIt)
{
    ChildProcessResult child{};
    const auto packets = CapturePacketsAround(
        [&child] { child = RunGpuWorkInChildProcess(DxTimingCaptureLibraryTest::GpuAdapterKind::Warp); });

    if (child.ExitCode == DxTimingCaptureLibraryTest::SubmitGpuWorkFailedExitCode)
    {
        GTEST_SKIP() << "The child process could not submit WARP work in this environment.";
    }
    ASSERT_EQ(child.ExitCode, 0u) << "The WARP work child process failed unexpectedly";

    const auto childPackets = PacketsFrom(packets, child.ProcessId);
    if (packets.empty())
    {
        GTEST_SKIP() << "No DMA packets were captured in this environment.";
    }

    ASSERT_FALSE(childPackets.empty())
        << "Expected WARP work submitted by process " << child.ProcessId << " to be attributed to it";

    ExpectScheduledInHardware(childPackets, false);
    ExpectSpansAreOrdered(childPackets);
}
