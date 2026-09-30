// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "pch.h"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

// MarkerOp.h (pulled in transitively) expands the D3D12_MARKER_API_* constants
// from the D3D12 ETW manifest headers, so those must be included first. These
// headers also provide Direct3D12EtwProviderGuid and EventD3D12JournalEntry_value.
#include <d3d12.h>
#include <D3D12Events.h>

#include <DxTimingCaptureLibrary/DxTimingCaptureEventHandler.h>

using namespace DirectX::Etw;

#include "NoOpCallbacks.h"

// These tests drive the runtime-failure path end to end: a synthetic
// EventD3D12JournalEntry record is pushed through DxTimingCaptureEventHandler (dispatch +
// decode + processor) and the decoded RuntimeFailureCallbacks call is asserted.
// The journal event is the D3D12 runtime's failure-breadcrumb stream - e.g. a
// command list removed because a recording-time call such as CopyBufferRegion was
// rejected lands here as "Removing Command List: <message>". See ETW-JournalEntry.md.

namespace
{
    constexpr DWORD TargetProcessId = 4321u;

    // Records every journal entry surfaced to the consumer, with the fields the
    // tests assert on.
    class RecordingRuntimeFailureCallbacks : public RuntimeFailureCallbacks
    {
    public:
        struct Entry
        {
            INT64 Timestamp;
            UINT32 Index;
            UINT32 Code;
            UINT32 ThreadId;
            std::string Message;
        };

        std::vector<Entry> Entries;

        HRESULT OnD3D12JournalEntry(INT64 timestamp, UINT32 index, UINT32 code, UINT32 threadId, std::string_view message) override
        {
            Entries.push_back({ timestamp, index, code, threadId, std::string(message) });
            return S_OK;
        }
    };

    // Builds the on-wire payload for an EventD3D12JournalEntry: Index, Code,
    // ThreadId (each UInt32, little-endian) followed by the ANSI Message. The
    // runtime copies the message into a fixed char[64], so it is null-terminated
    // on the wire; nullTerminate mirrors that (set false to test a buffer that is
    // exactly filled with no terminator).
    std::vector<uint8_t> MakeJournalPayload(uint32_t index, uint32_t code, uint32_t threadId, std::string_view message, bool nullTerminate = true)
    {
        std::vector<uint8_t> data;

        auto append32 = [&](uint32_t value)
        {
            auto* bytes = reinterpret_cast<uint8_t*>(&value);
            data.insert(data.end(), bytes, bytes + sizeof(uint32_t));
        };

        append32(index);
        append32(code);
        append32(threadId);
        data.insert(data.end(), message.begin(), message.end());
        if (nullTerminate)
        {
            data.push_back(0);
        }
        return data;
    }

    // A synthetic EventD3D12JournalEntry EVENT_RECORD. Owns its payload so the
    // record's UserData pointer stays valid for the lifetime of the object.
    struct JournalEventRecord
    {
        EVENT_RECORD record = {};
        std::vector<uint8_t> payload;

        JournalEventRecord(std::vector<uint8_t> data, DWORD processId, LONGLONG timestamp)
            : payload(std::move(data))
        {
            record.EventHeader.ProviderId = Direct3D12EtwProviderGuid;
            record.EventHeader.EventDescriptor.Id = EventD3D12JournalEntry_value;
            // Real ETW records always carry a pointer-size header flag; EventData
            // requires one even though this payload contains no pointers.
            record.EventHeader.Flags = EVENT_HEADER_FLAG_64_BIT_HEADER;
            record.EventHeader.ProcessId = processId;
            record.EventHeader.TimeStamp.QuadPart = timestamp;
            record.UserData = payload.data();
            record.UserDataLength = static_cast<USHORT>(payload.size());
        }

        EVENT_RECORD* Get() { return &record; }
    };

    // Wires a DxTimingCaptureEventHandler with no-op callbacks for every required category and
    // an optional recording RuntimeFailureCallbacks. When withFailureCallback is
    // false the optional slot is null, exercising the "journal entries ignored" path.
    struct HandlerFixture
    {
        NoOpTimestampConverter Converter;
        RecordingRuntimeFailureCallbacks RecordingStorage;

        RecordingRuntimeFailureCallbacks* Recording = nullptr;
        std::unique_ptr<DxTimingCaptureEventHandler> Handler;

        explicit HandlerFixture(bool withFailureCallback = true)
        {
            DxTimingCaptureEventCallbacks callbacks;

            if (withFailureCallback)
            {
                Recording = &RecordingStorage;
                callbacks.RuntimeFailureCallbacks = &RecordingStorage;
            }

            Handler = DxTimingCaptureEventHandler::Create(TargetProcessId, {}, callbacks, Converter);
        }
    };
}

// The motivating case: a CopyBufferRegion failure removes the command list, which
// the runtime journals as "Removing Command List: <message>" with E_INVALIDARG.
TEST(RuntimeFailureTests, CopyBufferRegionFailureSurfacesJournalEntry)
{
    HandlerFixture fixture;

    constexpr uint32_t expectedCode = 0x80070057; // E_INVALIDARG
    const std::string message = "Removing Command List: CopyBufferRegion invalid region";
    JournalEventRecord event(MakeJournalPayload(7, expectedCode, 9999, message), TargetProcessId, 123456);

    fixture.Handler->HandleEventRecord(event.Get());

    ASSERT_EQ(fixture.Recording->Entries.size(), 1u);
    const auto& entry = fixture.Recording->Entries.front();
    EXPECT_EQ(entry.Index, 7u);
    EXPECT_EQ(entry.Code, expectedCode);
    EXPECT_EQ(entry.ThreadId, 9999u);
    EXPECT_EQ(entry.Message, message);
    EXPECT_EQ(entry.Timestamp, 123456); // NoOpTimestampConverter passes ticks through unchanged
}

// Field-order regression guard: a fixed byte buffer with known values must map to
// exactly index/code/threadId/message in that order.
TEST(RuntimeFailureTests, DecodesFieldsInWireOrder)
{
    HandlerFixture fixture;

    // index=0x11223344, code=0x8007000E (E_OUTOFMEMORY), threadId=0x55667788, "oom"
    std::vector<uint8_t> payload = {
        0x44, 0x33, 0x22, 0x11, // index
        0x0E, 0x00, 0x07, 0x80, // code
        0x88, 0x77, 0x66, 0x55, // threadId
        'o', 'o', 'm', 0x00,    // message + NUL
    };
    JournalEventRecord event(std::move(payload), TargetProcessId, 1);

    fixture.Handler->HandleEventRecord(event.Get());

    ASSERT_EQ(fixture.Recording->Entries.size(), 1u);
    const auto& entry = fixture.Recording->Entries.front();
    EXPECT_EQ(entry.Index, 0x11223344u);
    EXPECT_EQ(entry.Code, 0x8007000Eu);
    EXPECT_EQ(entry.ThreadId, 0x55667788u);
    EXPECT_EQ(entry.Message, "oom");
}

// An empty message ("") is a valid, expected case and must not over-read.
TEST(RuntimeFailureTests, EmptyMessageIsValid)
{
    HandlerFixture fixture;

    JournalEventRecord event(MakeJournalPayload(0, 0x80004005 /*E_FAIL*/, 1, ""), TargetProcessId, 1);
    fixture.Handler->HandleEventRecord(event.Get());

    ASSERT_EQ(fixture.Recording->Entries.size(), 1u);
    EXPECT_TRUE(fixture.Recording->Entries.front().Message.empty());
    EXPECT_EQ(fixture.Recording->Entries.front().Code, 0x80004005u);
}

// The runtime copies the message into a char[64], so the longest possible message
// is 63 characters plus the terminator. The full string must be preserved.
TEST(RuntimeFailureTests, MaxLengthMessageIsPreserved)
{
    HandlerFixture fixture;

    const std::string message(63, 'x');
    JournalEventRecord event(MakeJournalPayload(1, 0x80070057, 2, message), TargetProcessId, 1);
    fixture.Handler->HandleEventRecord(event.Get());

    ASSERT_EQ(fixture.Recording->Entries.size(), 1u);
    EXPECT_EQ(fixture.Recording->Entries.front().Message, message);
    EXPECT_EQ(fixture.Recording->Entries.front().Message.size(), 63u);
}

// A message buffer that is exactly filled with no trailing NUL must still be read
// safely (bounded by the payload length, not a terminator).
TEST(RuntimeFailureTests, UnterminatedMessageIsBoundedByPayload)
{
    HandlerFixture fixture;

    const std::string message = "no terminator here";
    JournalEventRecord event(MakeJournalPayload(2, 0x80070057, 3, message, /*nullTerminate*/ false), TargetProcessId, 1);
    fixture.Handler->HandleEventRecord(event.Get());

    ASSERT_EQ(fixture.Recording->Entries.size(), 1u);
    EXPECT_EQ(fixture.Recording->Entries.front().Message, message);
}

// A truncated payload (fewer bytes than the three fixed UInt32 fields) is rejected
// by throwing - a clean bounds failure, not an out-of-bounds read or crash - and
// surfaces no partial entry. (Per the HandleEventRecord contract the consumer
// catches this; here we assert the throw directly.)
TEST(RuntimeFailureTests, TruncatedPayloadThrowsAndSurfacesNothing)
{
    HandlerFixture fixture;

    // Only 6 bytes - not even the first two UInt32s fit.
    std::vector<uint8_t> payload = { 0x01, 0x02, 0x03, 0x04, 0x05, 0x06 };
    JournalEventRecord event(std::move(payload), TargetProcessId, 1);

    EXPECT_ANY_THROW(fixture.Handler->HandleEventRecord(event.Get()));
    EXPECT_TRUE(fixture.Recording->Entries.empty());
}

// Journal entries from a process other than the decode target are ignored (the
// library correlates one target process at a time).
TEST(RuntimeFailureTests, EntryFromNonTargetProcessIsIgnored)
{
    HandlerFixture fixture;

    JournalEventRecord event(MakeJournalPayload(0, 0x80070057, 1, "other process"), TargetProcessId + 1, 1);
    fixture.Handler->HandleEventRecord(event.Get());

    EXPECT_TRUE(fixture.Recording->Entries.empty());
}

// With no RuntimeFailureCallbacks supplied (the optional slot is null), a journal
// entry is safely ignored - no null dereference, no crash.
TEST(RuntimeFailureTests, NullCallbackIgnoresEntry)
{
    HandlerFixture fixture(/*withFailureCallback*/ false);

    JournalEventRecord event(MakeJournalPayload(0, 0x80070057, 1, "ignored"), TargetProcessId, 1);
    EXPECT_NO_THROW(fixture.Handler->HandleEventRecord(event.Get()));
}

// The Index is a ring-buffer slot that wraps and is NOT a unique counter: the same
// Index on two different failures must surface both (de-duplication, if wanted, is
// the consumer's job).
TEST(RuntimeFailureTests, RepeatedIndexSurfacesEveryEntry)
{
    HandlerFixture fixture;

    JournalEventRecord first(MakeJournalPayload(5, 0x80070057, 1, "first"), TargetProcessId, 10);
    JournalEventRecord second(MakeJournalPayload(5, 0x8007000E, 2, "second"), TargetProcessId, 20);
    fixture.Handler->HandleEventRecord(first.Get());
    fixture.Handler->HandleEventRecord(second.Get());

    ASSERT_EQ(fixture.Recording->Entries.size(), 2u);
    EXPECT_EQ(fixture.Recording->Entries[0].Message, "first");
    EXPECT_EQ(fixture.Recording->Entries[1].Message, "second");
    EXPECT_EQ(fixture.Recording->Entries[0].Index, fixture.Recording->Entries[1].Index);
}

// A different Direct3D12 event id must not be routed to the journal callback (guards
// the dispatch wiring for event id 0x64).
TEST(RuntimeFailureTests, OtherEventIdDoesNotReachCallback)
{
    HandlerFixture fixture;

    JournalEventRecord event(MakeJournalPayload(0, 0x80070057, 1, "not a journal entry"), TargetProcessId, 1);
    event.record.EventHeader.EventDescriptor.Id = EventD3D12JournalEntry_value + 1;

    fixture.Handler->HandleEventRecord(event.Get());
    EXPECT_TRUE(fixture.Recording->Entries.empty());
}
