// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "pch.h"

#include <cstring>
#include <optional>
#include <vector>

// MarkerOp.h (pulled in transitively) expands the D3D12_MARKER_API_* constants
// from the D3D12 ETW manifest headers, so those must be included first.
#include <d3d12.h>
#include <D3D12Events.h>

#include <DxTimingCaptureLibrary/EtwDispatcher.h>

using namespace DirectX::Etw;

// The layouts these decoders assume come from the DxgKrnl manifest, which is
// not in this repo, so nothing else here can catch getting them wrong.
namespace
{
    struct PayloadBuilder
    {
        std::vector<uint8_t> bytes;
        bool is32Bit;

        explicit PayloadBuilder(bool use32BitPointers)
            : is32Bit(use32BitPointers)
        {
        }

        template <typename T>
        PayloadBuilder& Append(T value)
        {
            auto const* raw = reinterpret_cast<uint8_t const*>(&value);
            bytes.insert(bytes.end(), raw, raw + sizeof(T));
            return *this;
        }

        PayloadBuilder& AppendPointer(uint64_t value)
        {
            if (is32Bit)
            {
                return Append(static_cast<uint32_t>(value));
            }

            return Append(value);
        }
    };

    // Records the DXGK DMA packet events and ignores everything else.
    class RecordingProcessor
    {
    public:
        std::optional<DxgkDmaSubmitArgs> Submitted;
        std::optional<DxgkDmaIsrCompleteArgs> Completed;

    private:
        friend class EtwDispatcher<RecordingProcessor>;

        void SetTraceInfo(EVENT_TRACE_LOGFILE const&) {}

        template <typename TEventArgs>
        void OnD3D12Event_Start(EVENT_RECORD*, TEventArgs) {}

        template <typename TEventArgs>
        void OnD3D12Event_Stop(EVENT_RECORD*, TEventArgs) {}

        template <typename TEventArgs>
        void OnD3D12Event_DCStart(EVENT_RECORD*, TEventArgs) {}

        template <typename TEventArgs>
        void OnD3D12Event_Info(EVENT_RECORD*, TEventArgs) {}

        template <typename TEventArgs>
        void OnDxgkEvent_Start(EVENT_RECORD*, TEventArgs) {}

        template <typename TEventArgs>
        void OnDxgkEvent_Stop(EVENT_RECORD*, TEventArgs) {}

        template <typename TEventArgs>
        void OnDxgkEvent_DCStart(EVENT_RECORD*, TEventArgs) {}

        template <typename TEventArgs>
        void OnDxgkEvent_Info(EVENT_RECORD*, TEventArgs) {}

        template <>
        void OnDxgkEvent_Start(EVENT_RECORD*, DxgkDmaSubmitArgs args)
        {
            Submitted = args;
        }

        template <>
        void OnDxgkEvent_Info(EVENT_RECORD*, DxgkDmaIsrCompleteArgs args)
        {
            Completed = args;
        }

        void OnEventWritePIXRecordTimingBlock_v1(EVENT_RECORD*, uint32_t, uint32_t, uint8_t*) {}
        void OnEventWritePIXRecordTimingBlock_v2(EVENT_RECORD*, uint32_t, uint32_t, uint8_t*) {}
        void OnEventWritePIXReportCounterData(EVENT_RECORD*, float, std::wstring_view) {}
        void OnEventWritePIXRecordMemoryAllocationEvent(EVENT_RECORD*, UINT16, UINT64, UINT64, UINT64) {}
        void OnEventWritePIXRecordMemoryFreeEvent(EVENT_RECORD*, UINT16, UINT64, UINT64, UINT64) {}
        void OnPnpDeviceDescription(EVENT_RECORD*, std::wstring_view, std::wstring_view) {}
    };

    void Dispatch(RecordingProcessor& processor, uint16_t eventId, PayloadBuilder& payload)
    {
        EVENT_RECORD record{};
        record.EventHeader.ProviderId = DxgkControlGuid;
        record.EventHeader.EventDescriptor.Id = eventId;
        record.EventHeader.Flags = payload.is32Bit
            ? EVENT_HEADER_FLAG_32_BIT_HEADER
            : EVENT_HEADER_FLAG_64_BIT_HEADER;
        record.UserData = payload.bytes.data();
        record.UserDataLength = static_cast<USHORT>(payload.bytes.size());

        EtwDispatcher<RecordingProcessor> dispatcher(&processor);
        dispatcher.Dispatch(&record);
    }

    // The full manifest payload, of which the decoder only reads the leading
    // fields it needs.
    PayloadBuilder BuildDmaSubmitPayload(bool is32Bit, uint64_t hContext, uint64_t submissionId)
    {
        PayloadBuilder payload(is32Bit);
        payload.AppendPointer(hContext);
        payload.AppendPointer(0xAAAAAAAAAAAAAAAAull); // hQueuePacketContext
        payload.Append<uint32_t>(3);                  // packetType
        payload.Append<uint64_t>(submissionId);
        payload.Append<uint32_t>(7);                  // queueSubmitSequence
        payload.AppendPointer(0xBBBBBBBBBBBBBBBBull); // pDmaBuffer
        payload.Append<uint32_t>(0);                  // quantumStatus
        return payload;
    }

    PayloadBuilder BuildDmaIsrCompletePayload(bool is32Bit, uint64_t hContext, uint64_t completionId)
    {
        PayloadBuilder payload(is32Bit);
        payload.AppendPointer(hContext);
        payload.Append<uint32_t>(3); // packetType
        payload.Append<uint64_t>(completionId);
        payload.Append<uint32_t>(7); // queueSubmitSequence
        payload.Append<uint32_t>(2); // interruptType
        payload.Append<uint32_t>(0); // quantumStatus
        payload.Append<uint64_t>(0); // faultedVirtualAddress
        payload.Append<uint32_t>(0); // pageFaultFlags
        payload.AppendPointer(0);    // faultedProcessHandle
        return payload;
    }
}

TEST(DxgkDmaPacketDecoderTests, DmaSubmit_64BitPointers)
{
    RecordingProcessor processor;
    auto payload = BuildDmaSubmitPayload(false, 0x1122334455667788ull, 0x00000000DEADBEEFull);
    Dispatch(processor, EventDmaSubmit_value, payload);

    ASSERT_TRUE(processor.Submitted.has_value());
    EXPECT_EQ(0x1122334455667788ull, processor.Submitted->hContext);
    EXPECT_EQ(0x00000000DEADBEEFull, processor.Submitted->submissionId);
}

TEST(DxgkDmaPacketDecoderTests, DmaSubmit_32BitPointers)
{
    RecordingProcessor processor;
    auto payload = BuildDmaSubmitPayload(true, 0x55667788ull, 0x00000000DEADBEEFull);
    Dispatch(processor, EventDmaSubmit_value, payload);

    ASSERT_TRUE(processor.Submitted.has_value());
    EXPECT_EQ(0x55667788ull, processor.Submitted->hContext);
    EXPECT_EQ(0x00000000DEADBEEFull, processor.Submitted->submissionId);
}

TEST(DxgkDmaPacketDecoderTests, DmaIsrComplete_64BitPointers)
{
    RecordingProcessor processor;
    auto payload = BuildDmaIsrCompletePayload(false, 0x1122334455667788ull, 0x00000000DEADBEEFull);
    Dispatch(processor, EventDmaIsrComplete_value, payload);

    ASSERT_TRUE(processor.Completed.has_value());
    EXPECT_EQ(0x1122334455667788ull, processor.Completed->hContext);
    EXPECT_EQ(0x00000000DEADBEEFull, processor.Completed->completionId);
}

TEST(DxgkDmaPacketDecoderTests, DmaIsrComplete_32BitPointers)
{
    RecordingProcessor processor;
    auto payload = BuildDmaIsrCompletePayload(true, 0x55667788ull, 0x00000000DEADBEEFull);
    Dispatch(processor, EventDmaIsrComplete_value, payload);

    ASSERT_TRUE(processor.Completed.has_value());
    EXPECT_EQ(0x55667788ull, processor.Completed->hContext);
    EXPECT_EQ(0x00000000DEADBEEFull, processor.Completed->completionId);
}

// A submission and its completion have to agree on the packet's identity, or
// the interference check pairs nothing up.
TEST(DxgkDmaPacketDecoderTests, SubmitAndCompletePairOnContextAndFence)
{
    RecordingProcessor processor;

    auto submitPayload = BuildDmaSubmitPayload(false, 0xFEEDFACEull, 0x4242ull);
    Dispatch(processor, EventDmaSubmit_value, submitPayload);

    auto completePayload = BuildDmaIsrCompletePayload(false, 0xFEEDFACEull, 0x4242ull);
    Dispatch(processor, EventDmaIsrComplete_value, completePayload);

    ASSERT_TRUE(processor.Submitted.has_value());
    ASSERT_TRUE(processor.Completed.has_value());
    EXPECT_EQ(processor.Submitted->hContext, processor.Completed->hContext);
    EXPECT_EQ(processor.Submitted->submissionId, processor.Completed->completionId);
}

// The decoder stops at the last field it needs, so a shorter payload from an
// older build must not throw and take the whole capture down with it.
TEST(DxgkDmaPacketDecoderTests, TrailingFieldsAreNotRequired)
{
    RecordingProcessor processor;

    PayloadBuilder payload(false);
    payload.AppendPointer(0xFEEDFACEull);
    payload.Append<uint32_t>(3);
    payload.Append<uint64_t>(0x4242ull);

    Dispatch(processor, EventDmaIsrComplete_value, payload);

    ASSERT_TRUE(processor.Completed.has_value());
    EXPECT_EQ(0xFEEDFACEull, processor.Completed->hContext);
    EXPECT_EQ(0x4242ull, processor.Completed->completionId);
}
