// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "pch.h"

#include <vector>

// MarkerOp.h (pulled in transitively) expands the D3D12_MARKER_API_* constants
// from the D3D12 ETW manifest headers, so those must be included first.
#include <d3d12.h>
#include <D3D12Events.h>

#include <DxTimingCaptureLibrary/DxTimingCaptureEventHandler.h>
#include <DxTimingCaptureLibrary/EtwDispatcher.h>

using namespace DirectX::Etw;

#include "NoOpCallbacks.h"

namespace
{
    struct ReportedPacket
    {
        UINT32 ProcessId;
        UINT64 HardwareCommandQueueId;
        UINT32 EngineAffinity;
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
            UINT32 engineAffinity,
            INT64 submitTimestamp,
            INT64 completeTimestamp,
            bool hardwareScheduled) override
        {
            Packets.push_back({ processId, hardwareCommandQueueId, engineAffinity, submitTimestamp, completeTimestamp, hardwareScheduled });
            return S_OK;
        }
    };

    // The DmaSubmit / DmaIsrComplete payloads, of which the decoders read only
    // the leading fields. Laid out for 64-bit pointers.
    std::vector<uint8_t> BuildDmaSubmitPayload(uint64_t hContext, uint64_t submissionId)
    {
        std::vector<uint8_t> payload;
        auto append = [&payload](auto value)
        {
            auto const* raw = reinterpret_cast<uint8_t const*>(&value);
            payload.insert(payload.end(), raw, raw + sizeof(value));
        };

        append(hContext);
        append(0xAAAAAAAAAAAAAAAAull); // hQueuePacketContext
        append(uint32_t{ 3 });         // packetType
        append(submissionId);
        append(uint32_t{ 7 });         // queueSubmitSequence
        append(0xBBBBBBBBBBBBBBBBull); // pDmaBuffer
        append(uint32_t{ 0 });         // quantumStatus

        return payload;
    }

    std::vector<uint8_t> BuildDmaIsrCompletePayload(uint64_t hContext, uint64_t completionId)
    {
        std::vector<uint8_t> payload;
        auto append = [&payload](auto value)
        {
            auto const* raw = reinterpret_cast<uint8_t const*>(&value);
            payload.insert(payload.end(), raw, raw + sizeof(value));
        };

        append(hContext);
        append(uint32_t{ 3 });  // packetType
        append(completionId);
        append(uint32_t{ 7 });  // queueSubmitSequence
        append(uint32_t{ 2 });  // interruptType
        append(uint32_t{ 0 });  // quantumStatus
        append(uint64_t{ 0 });  // faultedVirtualAddress
        append(uint32_t{ 0 });  // pageFaultFlags
        append(uint64_t{ 0 });  // faultedProcessHandle

        return payload;
    }

    void Dispatch(DxTimingCaptureEventHandler& handler, uint16_t eventId, uint64_t timestamp, std::vector<uint8_t>& payload)
    {
        EVENT_RECORD record{};
        record.EventHeader.ProviderId = DxgkControlGuid;
        record.EventHeader.EventDescriptor.Id = eventId;
        record.EventHeader.Flags = EVENT_HEADER_FLAG_64_BIT_HEADER;
        record.EventHeader.TimeStamp.QuadPart = static_cast<LONGLONG>(timestamp);
        record.UserData = payload.data();
        record.UserDataLength = static_cast<USHORT>(payload.size());

        handler.HandleEventRecord(&record);
    }

    constexpr uint16_t DmaSubmitEventId = 175;
    constexpr uint16_t DmaIsrCompleteEventId = 177;

    std::unique_ptr<DxTimingCaptureEventHandler> CreateHandler(
        RecordingGpuEngineActivityCallbacks& gpuEngineActivity,
        NoOpTimestampConverter& converter,
        bool TrackGpuEngineActivity)
    {
        DxTimingCaptureEventCallbacks callbacks{};
        callbacks.GpuEngineActivityCallbacks = &gpuEngineActivity;

        DxTimingCaptureLibraryOptions options{};
        options.TrackGpuEngineActivity = TrackGpuEngineActivity;

        return DxTimingCaptureEventHandler::Create(1234u, options, callbacks, converter);
    }
}

// The packet events come from other processes, so the handler's process filter
// must not apply to them.
TEST(GpuEngineActivityCallbackTests, PairedPacketIsReportedRegardlessOfTheProcessFilter)
{
    RecordingGpuEngineActivityCallbacks gpuEngineActivity;
    NoOpTimestampConverter converter;
    auto handler = CreateHandler(gpuEngineActivity, converter, true);

    auto submit = BuildDmaSubmitPayload(0x1000, 42);
    auto complete = BuildDmaIsrCompletePayload(0x1000, 42);

    Dispatch(*handler, DmaSubmitEventId, 100, submit);
    Dispatch(*handler, DmaIsrCompleteEventId, 250, complete);

    ASSERT_EQ(1u, gpuEngineActivity.Packets.size());
    EXPECT_EQ(100, gpuEngineActivity.Packets[0].SubmitTimestamp);
    EXPECT_EQ(250, gpuEngineActivity.Packets[0].CompleteTimestamp);
    EXPECT_FALSE(gpuEngineActivity.Packets[0].HardwareScheduled);

    // No context events were fed in, so the packet cannot be attributed.
    EXPECT_EQ(0u, gpuEngineActivity.Packets[0].ProcessId);
    EXPECT_EQ(0u, gpuEngineActivity.Packets[0].HardwareCommandQueueId);
}

TEST(GpuEngineActivityCallbackTests, NothingIsReportedWithoutTrackGpuEngineActivity)
{
    RecordingGpuEngineActivityCallbacks gpuEngineActivity;
    NoOpTimestampConverter converter;
    auto handler = CreateHandler(gpuEngineActivity, converter, false);

    auto submit = BuildDmaSubmitPayload(0x1000, 42);
    auto complete = BuildDmaIsrCompletePayload(0x1000, 42);

    Dispatch(*handler, DmaSubmitEventId, 100, submit);
    Dispatch(*handler, DmaIsrCompleteEventId, 250, complete);
    handler->OnDataComplete();

    EXPECT_TRUE(gpuEngineActivity.Packets.empty());
}

TEST(GpuEngineActivityCallbackTests, UnfinishedPacketIsFlushedOnceOnDataComplete)
{
    RecordingGpuEngineActivityCallbacks gpuEngineActivity;
    NoOpTimestampConverter converter;
    auto handler = CreateHandler(gpuEngineActivity, converter, true);

    auto submit = BuildDmaSubmitPayload(0x2000, 7);
    auto unrelated = BuildDmaSubmitPayload(0x2001, 8);

    Dispatch(*handler, DmaSubmitEventId, 100, submit);
    Dispatch(*handler, DmaSubmitEventId, 400, unrelated);

    handler->OnDataComplete();
    handler->OnDataComplete();

    // Only the first packet has a span; the second started at the last event we saw.
    ASSERT_EQ(1u, gpuEngineActivity.Packets.size());
    EXPECT_EQ(100, gpuEngineActivity.Packets[0].SubmitTimestamp);
    EXPECT_EQ(400, gpuEngineActivity.Packets[0].CompleteTimestamp);
}
