// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "pch.h"

#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <DxTimingCaptureLibrary/DxTimingCaptureEventHandler.h>

// The vendored DirectStorage event descriptors and payload layouts are used here to
// synthesize the raw ETW records the handler decodes. This is exactly what a live
// DirectStorage trace would deliver, so feeding these through a real
// DxTimingCaptureEventHandler exercises the full decode + correlation path end to end.
#include <dstorage_etw.h>

#include "NoOpCallbacks.h"

using namespace DirectX::Etw;

namespace
{
    constexpr uint32_t AnyProcessId = 4321;
    constexpr uint32_t EnqueueThreadId = 77;

    // The library assigns file and queue ids by pre-incrementing a counter that starts
    // at 1, so the first file and the first queue each get id 2.
    constexpr UINT16 FirstFileId = 2;
    constexpr UINT16 FirstQueueId = 2;

    // Records every DirectStorage callback the handler makes so the tests can assert on
    // the correlated results.
    class RecordingDirectStorageCallbacks : public DirectStorageCallbacks
    {
    public:
        struct FileRecord { UINT16 FileId; INT64 Timestamp; std::wstring Path; };
        struct QueueRecord { UINT16 QueueId; INT64 Timestamp; std::string Name; };

        std::vector<FileRecord> Files;
        std::vector<QueueRecord> Queues;
        std::vector<ReadRequest> Reads;
        std::vector<StatusNotification> Statuses;
        std::vector<FenceSignal> Fences;
        std::vector<SetEventNotification> SetEvents;
        std::vector<Submit> Submits;

        // Set any of these to a failing HRESULT before Run() to make that callback fail;
        // the handler wraps every callback in ThrowFailure, so the failure surfaces as a
        // throw out of HandleEventRecord.
        HRESULT FileResult = S_OK;
        HRESULT QueueResult = S_OK;
        HRESULT ReadResult = S_OK;
        HRESULT StatusResult = S_OK;
        HRESULT FenceResult = S_OK;
        HRESULT SetEventResult = S_OK;
        HRESULT SubmitResult = S_OK;

        HRESULT OnDirectStorageFile(UINT16 fileId, INT64 timestamp, std::wstring_view path) override
        {
            Files.push_back({ fileId, timestamp, std::wstring(path) });
            return FileResult;
        }

        HRESULT OnDirectStorageQueue(UINT16 queueId, INT64 timestamp, std::string_view name) override
        {
            Queues.push_back({ queueId, timestamp, std::string(name) });
            return QueueResult;
        }

        HRESULT OnDirectStorageReadRequest(const ReadRequest& request) override
        {
            Reads.push_back(request);
            return ReadResult;
        }

        HRESULT OnDirectStorageStatus(const StatusNotification& status) override
        {
            Statuses.push_back(status);
            return StatusResult;
        }

        HRESULT OnDirectStorageFenceSignal(const FenceSignal& signal) override
        {
            Fences.push_back(signal);
            return FenceResult;
        }

        HRESULT OnDirectStorageSetEvent(const SetEventNotification& setEvent) override
        {
            SetEvents.push_back(setEvent);
            return SetEventResult;
        }

        HRESULT OnDirectStorageSubmit(const Submit& submit) override
        {
            Submits.push_back(submit);
            return SubmitResult;
        }
    };

    // Assembles an ETW event payload one field at a time, matching the packed layout the
    // decoder reads back with EventData::Read<T>().
    class BlobBuilder
    {
        std::vector<uint8_t> m_data;

    public:
        template<typename T>
        BlobBuilder& Append(T value)
        {
            auto* bytes = reinterpret_cast<const uint8_t*>(&value);
            m_data.insert(m_data.end(), bytes, bytes + sizeof(T));
            return *this;
        }

        // A narrow, null-terminated string, matching the runtime's EndingString fields
        // that the decoder reads with ReadNullTerminatedString().
        BlobBuilder& AppendNullTerminated(std::string_view value)
        {
            m_data.insert(m_data.end(), value.begin(), value.end());
            m_data.push_back(0);
            return *this;
        }

        // A narrow string with no terminator, for the trailing EndingString the decoder
        // reads with ReadRemainingAsString() (it consumes the rest of the payload).
        BlobBuilder& AppendRemainingNarrow(std::string_view value)
        {
            m_data.insert(m_data.end(), value.begin(), value.end());
            return *this;
        }

        // A wide string with no terminator, for the trailing EndingWideString the decoder
        // reads with ReadRemainingAsUnicodeString().
        BlobBuilder& AppendRemainingWide(std::wstring_view value)
        {
            auto* bytes = reinterpret_cast<const uint8_t*>(value.data());
            m_data.insert(m_data.end(), bytes, bytes + value.size() * sizeof(wchar_t));
            return *this;
        }

        std::vector<uint8_t> Take() { return std::move(m_data); }
    };

    // An EVENT_RECORD that owns its payload, so a vector of these stays valid while the
    // handler decodes them.
    struct OwningEventRecord : EVENT_RECORD
    {
        OwningEventRecord(UINT16 eventId, UINT8 version, INT64 timestamp, std::vector<uint8_t> payload)
            : EVENT_RECORD{}
            , m_payload(std::move(payload))
        {
            EventHeader.ProviderId = DirectStorage::DStorageEtwProvider;
            EventHeader.EventDescriptor.Id = eventId;
            EventHeader.EventDescriptor.Version = version;
            EventHeader.Flags = EVENT_HEADER_FLAG_64_BIT_HEADER;
            EventHeader.ProcessId = AnyProcessId;
            EventHeader.ThreadId = EnqueueThreadId;
            EventHeader.TimeStamp.QuadPart = timestamp;
            UserData = m_payload.data();
            UserDataLength = static_cast<USHORT>(m_payload.size());
        }

        OwningEventRecord(OwningEventRecord&&) = default;
        OwningEventRecord& operator=(OwningEventRecord&&) = default;
        OwningEventRecord(const OwningEventRecord&) = delete;
        OwningEventRecord& operator=(const OwningEventRecord&) = delete;

    private:
        std::vector<uint8_t> m_payload;
    };

    // Builds a DirectStorage workload as a list of raw ETW records, then replays it
    // through a real DxTimingCaptureEventHandler and exposes the recorded callbacks. Timestamps
    // are assigned by the caller so the tests can assert exact enqueue/completion values;
    // the handler is given an identity timestamp converter (NoOpTimestampConverter).
    class DirectStorageFixture
    {
        std::vector<OwningEventRecord> m_records;

        NoOpTimestampConverter m_converter;
        RecordingDirectStorageCallbacks m_callbacks;

        std::unique_ptr<DxTimingCaptureEventHandler> m_handler;

    public:
        // Valid before and after Run(); set the callbacks' failure-injection fields on it
        // before Run() to exercise the error path.
        RecordingDirectStorageCallbacks* Callbacks = &m_callbacks;

        void OpenFile(INT64 timestamp, uint64_t file, std::wstring_view path)
        {
            BlobBuilder builder;
            builder.Append<uint64_t>(file);
            builder.AppendRemainingWide(path);
            Add(DirectStorage::OpenFileEvent::Desc.Id, 1, timestamp, builder.Take());
        }

        void CloseFile(INT64 timestamp, uint64_t file)
        {
            BlobBuilder builder;
            builder.Append<uint64_t>(file);
            Add(DirectStorage::CloseFileEvent::Desc.Id, 1, timestamp, builder.Take());
        }

        void CreateQueue(INT64 timestamp, uint64_t queue, std::string_view name)
        {
            BlobBuilder builder;
            builder.Append<uint64_t>(queue);
            builder.Append<uint8_t>(0);  // SourceType
            builder.Append<uint16_t>(0); // Capacity
            builder.Append<int8_t>(0);   // Priority
            builder.Append<uint64_t>(0); // Device
            builder.AppendRemainingNarrow(name);
            Add(DirectStorage::CreateQueueEvent::Desc.Id, 1, timestamp, builder.Take());
        }

        void CloseQueue(INT64 timestamp, uint64_t queue)
        {
            BlobBuilder builder;
            builder.Append<uint64_t>(queue);
            Add(DirectStorage::CloseQueueEvent::Desc.Id, 1, timestamp, builder.Take());
        }

        void EnqueueRequestV1(
            INT64 timestamp,
            uint64_t queue,
            uint64_t request,
            uint64_t file,
            uint32_t sourceSize,
            uint64_t offset,
            uint8_t compressionFormat)
        {
            BlobBuilder builder;
            AppendEnqueueRequestData(builder, request, queue, file, sourceSize, offset, compressionFormat);
            builder.AppendNullTerminated(""); // EndingString name
            Add(DirectStorage::EnqueueRequestEvent::Desc.Id, 1, timestamp, builder.Take());
        }

        void EnqueueRequestV2(
            INT64 timestamp,
            uint64_t queue,
            uint64_t request,
            uint64_t file,
            uint32_t sourceSize,
            uint64_t offset,
            uint8_t compressionFormat,
            uint8_t transformType)
        {
            BlobBuilder builder;
            AppendEnqueueRequestData(builder, request, queue, file, sourceSize, offset, compressionFormat);
            builder.AppendNullTerminated(""); // EndingString name
            builder.Append<uint8_t>(transformType);
            Add(DirectStorage::EnqueueRequestEvent::Desc.Id, 2, timestamp, builder.Take());
        }

        void RequestCompleted(INT64 timestamp, uint64_t queue, uint64_t request)
        {
            BlobBuilder builder;
            builder.Append<uint64_t>(request);
            builder.Append<int32_t>(0); // Result
            builder.Append<uint64_t>(queue);
            Add(DirectStorage::RequestCompletedEvent::Desc.Id, 1, timestamp, builder.Take());
        }

        void EnqueueStatus(INT64 timestamp, uint64_t queue, uint64_t statusArray, uint32_t index)
        {
            BlobBuilder builder;
            builder.Append<uint64_t>(statusArray);
            builder.Append<uint32_t>(index);
            builder.Append<uint64_t>(queue);
            Add(DirectStorage::EnqueueStatusEvent::Desc.Id, 1, timestamp, builder.Take());
        }

        void StatusCompleted(INT64 timestamp, uint64_t queue, uint64_t statusArray, uint32_t index)
        {
            BlobBuilder builder;
            builder.Append<uint64_t>(statusArray);
            builder.Append<uint32_t>(index);
            builder.Append<int32_t>(0); // Result
            builder.Append<uint64_t>(queue);
            Add(DirectStorage::StatusCompletedEvent::Desc.Id, 1, timestamp, builder.Take());
        }

        void EnqueueSignal(INT64 timestamp, uint64_t queue, uint64_t fence, uint64_t value)
        {
            BlobBuilder builder;
            builder.Append<uint64_t>(fence);
            builder.Append<uint64_t>(value);
            builder.Append<uint64_t>(queue);
            Add(DirectStorage::EnqueueSignalEvent::Desc.Id, 1, timestamp, builder.Take());
        }

        void SignalCompleted(INT64 timestamp, uint64_t queue, uint64_t fence, uint64_t value)
        {
            BlobBuilder builder;
            builder.Append<uint64_t>(fence);
            builder.Append<uint64_t>(value);
            builder.Append<uint64_t>(queue);
            Add(DirectStorage::SignalCompletedEvent::Desc.Id, 1, timestamp, builder.Take());
        }

        void EnqueueSetEvent(INT64 timestamp, uint64_t queue, uint64_t handle)
        {
            BlobBuilder builder;
            builder.Append<uint64_t>(handle);
            builder.Append<uint64_t>(queue);
            Add(DirectStorage::EnqueueSetEvent::Desc.Id, 1, timestamp, builder.Take());
        }

        void SetEventCompleted(INT64 timestamp, uint64_t queue, uint64_t handle)
        {
            BlobBuilder builder;
            builder.Append<uint64_t>(handle);
            builder.Append<uint64_t>(queue);
            Add(DirectStorage::SetEventCompletedEvent::Desc.Id, 1, timestamp, builder.Take());
        }

        void Submit(INT64 timestamp, uint64_t queue, bool autoSubmit)
        {
            BlobBuilder builder;
            builder.Append<uint64_t>(queue);
            builder.Append<uint8_t>(autoSubmit ? 1 : 0);
            Add(DirectStorage::SubmitEvent::Desc.Id, 1, timestamp, builder.Take());
        }

        // Replays the recorded workload through a fresh handler and leaves the recorded
        // callbacks available through Callbacks. The handler is retained for the
        // fixture's lifetime so it (and the callbacks it owns) outlive the assertions.
        void Run()
        {
            DxTimingCaptureEventCallbacks callbacks;
            callbacks.DirectStorageCallbacks = &m_callbacks;

            PixEventConfig pixEventConfig;
            pixEventConfig.MirrorGpuContextEventsToCpu = true;

            m_handler = DxTimingCaptureEventHandler::Create(AnyProcessId, {}, callbacks, m_converter, pixEventConfig);

            for (auto& record : m_records)
            {
                m_handler->HandleEventRecord(&record);
            }
            m_handler->OnDataComplete();
        }

    private:
        static void AppendEnqueueRequestData(
            BlobBuilder& builder,
            uint64_t request,
            uint64_t queue,
            uint64_t file,
            uint32_t sourceSize,
            uint64_t offset,
            uint8_t compressionFormat)
        {
            builder.Append<uint64_t>(request);
            builder.Append<uint64_t>(queue);
            builder.Append<uint32_t>(sourceSize);
            builder.Append<uint32_t>(sourceSize); // UncompressedSize (unused by the decoder)
            builder.Append<uint8_t>(compressionFormat);
            builder.Append<uint8_t>(0); // SourceType
            builder.Append<uint8_t>(0); // DestinationType
            builder.Append<uint64_t>(file);
            builder.Append<uint64_t>(offset);
        }

        void Add(UINT16 eventId, UINT8 version, INT64 timestamp, std::vector<uint8_t> payload)
        {
            m_records.emplace_back(eventId, version, timestamp, std::move(payload));
        }
    };
}

// A read request whose enqueue and completion are both captured is reported once, with
// its file/queue ids correlated and its offset/size/compression carried through.
TEST(DirectStorage, ReadRequestV1Correlation)
{
    DirectStorageFixture fixture;
    fixture.CreateQueue(100, 0xA000, "GraphicsQueue");
    fixture.OpenFile(110, 0xF000, L"C:\\game\\textures.pak");
    fixture.EnqueueRequestV1(120, 0xA000, 0x1000, 0xF000, /*sourceSize*/ 4096, /*offset*/ 8192, /*compression*/ 1);
    fixture.Submit(125, 0xA000, false);
    fixture.RequestCompleted(200, 0xA000, 0x1000);
    fixture.Run();

    auto& callbacks = *fixture.Callbacks;

    ASSERT_EQ(callbacks.Queues.size(), 1u);
    EXPECT_EQ(callbacks.Queues[0].QueueId, FirstQueueId);
    EXPECT_EQ(callbacks.Queues[0].Timestamp, 100);
    EXPECT_EQ(callbacks.Queues[0].Name, "GraphicsQueue");

    ASSERT_EQ(callbacks.Files.size(), 1u);
    EXPECT_EQ(callbacks.Files[0].FileId, FirstFileId);
    EXPECT_EQ(callbacks.Files[0].Timestamp, 110);
    EXPECT_EQ(callbacks.Files[0].Path, L"C:\\game\\textures.pak");

    ASSERT_EQ(callbacks.Reads.size(), 1u);
    const auto& read = callbacks.Reads[0];
    EXPECT_EQ(read.QueueId, FirstQueueId);
    EXPECT_EQ(read.FileId, FirstFileId);
    EXPECT_EQ(read.EnqueueThreadId, EnqueueThreadId);
    EXPECT_EQ(read.EnqueueTimestamp, 120);
    EXPECT_EQ(read.CompletionTimestamp, 200);
    EXPECT_EQ(read.Offset, 8192u);
    EXPECT_EQ(read.Size, 4096u);
    EXPECT_EQ(read.CompressionType, 1u);
    EXPECT_EQ(read.ShuffleType, 0u); // v1 has no transform, so shuffle stays 0

    ASSERT_EQ(callbacks.Submits.size(), 1u);
    EXPECT_EQ(callbacks.Submits[0].QueueId, FirstQueueId);
    EXPECT_EQ(callbacks.Submits[0].Timestamp, 125);
    EXPECT_FALSE(callbacks.Submits[0].AutoSubmitted);
}

// The DirectStorage 1.4 (version 2) enqueue carries an extra transform/shuffle byte,
// which must surface as ShuffleType on the correlated read.
TEST(DirectStorage, ReadRequestV2CarriesShuffleType)
{
    DirectStorageFixture fixture;
    fixture.CreateQueue(100, 0xA000, "Queue");
    fixture.OpenFile(110, 0xF000, L"data.bin");
    fixture.EnqueueRequestV2(120, 0xA000, 0x1000, 0xF000, /*sourceSize*/ 2048, /*offset*/ 512, /*compression*/ 2, /*transform*/ 3);
    fixture.RequestCompleted(200, 0xA000, 0x1000);
    fixture.Run();

    auto& callbacks = *fixture.Callbacks;
    ASSERT_EQ(callbacks.Reads.size(), 1u);
    const auto& read = callbacks.Reads[0];
    EXPECT_EQ(read.Size, 2048u);
    EXPECT_EQ(read.Offset, 512u);
    EXPECT_EQ(read.CompressionType, 2u);
    EXPECT_EQ(read.ShuffleType, 3u);
}

// A file referenced by a request but never opened in the trace is still registered, with
// a synthesized "file# <id>" placeholder name.
TEST(DirectStorage, RequestForUnopenedFileSynthesizesName)
{
    DirectStorageFixture fixture;
    fixture.CreateQueue(100, 0xA000, "Queue");
    fixture.EnqueueRequestV1(120, 0xA000, 0x1000, 0xF000, 1024, 0, 0);
    fixture.RequestCompleted(200, 0xA000, 0x1000);
    fixture.Run();

    auto& callbacks = *fixture.Callbacks;
    ASSERT_EQ(callbacks.Files.size(), 1u);
    EXPECT_EQ(callbacks.Files[0].FileId, FirstFileId);
    EXPECT_EQ(callbacks.Files[0].Path, std::wstring(L"file# ") + std::to_wstring(FirstFileId));

    ASSERT_EQ(callbacks.Reads.size(), 1u);
    EXPECT_EQ(callbacks.Reads[0].FileId, FirstFileId);
}

// Distinct files and queues each get their own monotonically increasing id, assigned in
// first-seen order.
TEST(DirectStorage, DistinctFilesAndQueuesGetSequentialIds)
{
    DirectStorageFixture fixture;
    fixture.CreateQueue(100, 0xA000, "QueueA");
    fixture.CreateQueue(101, 0xB000, "QueueB");
    fixture.OpenFile(110, 0xF000, L"a.pak");
    fixture.OpenFile(111, 0xF100, L"b.pak");
    fixture.Run();

    auto& callbacks = *fixture.Callbacks;
    ASSERT_EQ(callbacks.Queues.size(), 2u);
    EXPECT_EQ(callbacks.Queues[0].QueueId, 2);
    EXPECT_EQ(callbacks.Queues[0].Name, "QueueA");
    EXPECT_EQ(callbacks.Queues[1].QueueId, 3);
    EXPECT_EQ(callbacks.Queues[1].Name, "QueueB");

    ASSERT_EQ(callbacks.Files.size(), 2u);
    EXPECT_EQ(callbacks.Files[0].FileId, 2);
    EXPECT_EQ(callbacks.Files[0].Path, L"a.pak");
    EXPECT_EQ(callbacks.Files[1].FileId, 3);
    EXPECT_EQ(callbacks.Files[1].Path, L"b.pak");
}

// A status-array notification is reported once its enqueue and completion are matched,
// keyed on (status array, index).
TEST(DirectStorage, StatusNotificationCorrelation)
{
    DirectStorageFixture fixture;
    fixture.CreateQueue(100, 0xA000, "Queue");
    fixture.EnqueueStatus(120, 0xA000, /*statusArray*/ 0x5000, /*index*/ 7);
    fixture.StatusCompleted(200, 0xA000, 0x5000, 7);
    fixture.Run();

    auto& callbacks = *fixture.Callbacks;
    ASSERT_EQ(callbacks.Statuses.size(), 1u);
    const auto& status = callbacks.Statuses[0];
    EXPECT_EQ(status.QueueId, FirstQueueId);
    EXPECT_EQ(status.EnqueueThreadId, EnqueueThreadId);
    EXPECT_EQ(status.EnqueueTimestamp, 120);
    EXPECT_EQ(status.CompletionTimestamp, 200);
    EXPECT_EQ(status.StatusArray, 0x5000u);
    EXPECT_EQ(status.Index, 7u);
}

// A completion with no matching enqueue (its enqueue predates the trace) is still
// reported, with the enqueue timestamp defaulted to the completion timestamp.
TEST(DirectStorage, StatusCompletedWithoutEnqueue)
{
    DirectStorageFixture fixture;
    fixture.StatusCompleted(200, 0xA000, 0x5000, 7);
    fixture.Run();

    auto& callbacks = *fixture.Callbacks;
    // The queue was never created, so it is registered on first reference here.
    ASSERT_EQ(callbacks.Queues.size(), 1u);
    EXPECT_EQ(callbacks.Queues[0].QueueId, FirstQueueId);
    EXPECT_TRUE(callbacks.Queues[0].Name.empty());

    ASSERT_EQ(callbacks.Statuses.size(), 1u);
    const auto& status = callbacks.Statuses[0];
    EXPECT_EQ(status.QueueId, FirstQueueId);
    EXPECT_EQ(status.EnqueueTimestamp, 200);
    EXPECT_EQ(status.CompletionTimestamp, 200);
    EXPECT_EQ(status.StatusArray, 0x5000u);
    EXPECT_EQ(status.Index, 7u);
}

// Unlike Status/Fence/SetEvent, a RequestCompleted whose enqueue was never captured
// (its enqueue predates the trace) is dropped: there is no request data to report, and
// emitting an all-zero read would surface a phantom IO on queue/file 0. This matches the
// original decoder, whose default-constructed XdsData carried an out-of-range DsType that
// the downstream consumer ignored.
TEST(DirectStorage, RequestCompletedWithoutEnqueueIsIgnored)
{
    DirectStorageFixture fixture;
    fixture.RequestCompleted(200, 0xA000, 0x1000);
    fixture.Run();

    auto& callbacks = *fixture.Callbacks;
    EXPECT_TRUE(callbacks.Reads.empty());
    // Nothing is registered off the back of the dropped completion either.
    EXPECT_TRUE(callbacks.Queues.empty());
    EXPECT_TRUE(callbacks.Files.empty());
}

// A fence signal is reported once its enqueue and completion are matched on (fence, value).
TEST(DirectStorage, FenceSignalCorrelation)
{
    DirectStorageFixture fixture;
    fixture.CreateQueue(100, 0xA000, "Queue");
    fixture.EnqueueSignal(120, 0xA000, /*fence*/ 0x6000, /*value*/ 9);
    fixture.SignalCompleted(200, 0xA000, 0x6000, 9);
    fixture.Run();

    auto& callbacks = *fixture.Callbacks;
    ASSERT_EQ(callbacks.Fences.size(), 1u);
    const auto& fence = callbacks.Fences[0];
    EXPECT_EQ(fence.QueueId, FirstQueueId);
    EXPECT_EQ(fence.EnqueueThreadId, EnqueueThreadId);
    EXPECT_EQ(fence.EnqueueTimestamp, 120);
    EXPECT_EQ(fence.CompletionTimestamp, 200);
    EXPECT_EQ(fence.Fence, 0x6000u);
    EXPECT_EQ(fence.Value, 9u);
}

// A SignalCompleted with no matching enqueue is still reported, with its enqueue timestamp
// synthesized from the completion and the queue registered on first reference.
TEST(DirectStorage, FenceSignalCompletedWithoutEnqueue)
{
    DirectStorageFixture fixture;
    fixture.SignalCompleted(200, 0xA000, /*fence*/ 0x6000, /*value*/ 9);
    fixture.Run();

    auto& callbacks = *fixture.Callbacks;
    // The queue was never created, so it is registered on first reference here.
    ASSERT_EQ(callbacks.Queues.size(), 1u);
    EXPECT_EQ(callbacks.Queues[0].QueueId, FirstQueueId);
    EXPECT_TRUE(callbacks.Queues[0].Name.empty());

    ASSERT_EQ(callbacks.Fences.size(), 1u);
    const auto& fence = callbacks.Fences[0];
    EXPECT_EQ(fence.QueueId, FirstQueueId);
    EXPECT_EQ(fence.EnqueueThreadId, EnqueueThreadId);
    EXPECT_EQ(fence.EnqueueTimestamp, 200);
    EXPECT_EQ(fence.CompletionTimestamp, 200);
    EXPECT_EQ(fence.Fence, 0x6000u);
    EXPECT_EQ(fence.Value, 9u);
}

// A Win32 event-set request is reported once its enqueue and completion are matched on
// the event handle.
TEST(DirectStorage, SetEventCorrelation)
{
    DirectStorageFixture fixture;
    fixture.CreateQueue(100, 0xA000, "Queue");
    fixture.EnqueueSetEvent(120, 0xA000, /*handle*/ 0x7000);
    fixture.SetEventCompleted(200, 0xA000, 0x7000);
    fixture.Run();

    auto& callbacks = *fixture.Callbacks;
    ASSERT_EQ(callbacks.SetEvents.size(), 1u);
    const auto& setEvent = callbacks.SetEvents[0];
    EXPECT_EQ(setEvent.QueueId, FirstQueueId);
    EXPECT_EQ(setEvent.EnqueueThreadId, EnqueueThreadId);
    EXPECT_EQ(setEvent.EnqueueTimestamp, 120);
    EXPECT_EQ(setEvent.CompletionTimestamp, 200);
    EXPECT_EQ(setEvent.Handle, 0x7000u);
}

// As above, for the set-event handle path: a SetEventCompleted with no matching enqueue is
// still reported.
TEST(DirectStorage, SetEventCompletedWithoutEnqueue)
{
    DirectStorageFixture fixture;
    fixture.SetEventCompleted(200, 0xA000, /*handle*/ 0x7000);
    fixture.Run();

    auto& callbacks = *fixture.Callbacks;
    ASSERT_EQ(callbacks.Queues.size(), 1u);
    EXPECT_EQ(callbacks.Queues[0].QueueId, FirstQueueId);
    EXPECT_TRUE(callbacks.Queues[0].Name.empty());

    ASSERT_EQ(callbacks.SetEvents.size(), 1u);
    const auto& setEvent = callbacks.SetEvents[0];
    EXPECT_EQ(setEvent.QueueId, FirstQueueId);
    EXPECT_EQ(setEvent.EnqueueThreadId, EnqueueThreadId);
    EXPECT_EQ(setEvent.EnqueueTimestamp, 200);
    EXPECT_EQ(setEvent.CompletionTimestamp, 200);
    EXPECT_EQ(setEvent.Handle, 0x7000u);
}

// An auto-submit surfaces the AutoSubmitted flag.
TEST(DirectStorage, AutoSubmitFlag)
{
    DirectStorageFixture fixture;
    fixture.CreateQueue(100, 0xA000, "Queue");
    fixture.Submit(130, 0xA000, /*autoSubmit*/ true);
    fixture.Run();

    auto& callbacks = *fixture.Callbacks;
    ASSERT_EQ(callbacks.Submits.size(), 1u);
    EXPECT_EQ(callbacks.Submits[0].QueueId, FirstQueueId);
    EXPECT_TRUE(callbacks.Submits[0].AutoSubmitted);
}

// A queue referenced only by an enqueue (its creation predates the trace) is registered
// on first reference, with an empty name.
TEST(DirectStorage, EnqueueOnUncreatedQueueRegistersQueue)
{
    DirectStorageFixture fixture;
    fixture.EnqueueRequestV1(120, 0xA000, 0x1000, 0xF000, 1024, 0, 0);
    fixture.RequestCompleted(200, 0xA000, 0x1000);
    fixture.Run();

    auto& callbacks = *fixture.Callbacks;
    ASSERT_EQ(callbacks.Queues.size(), 1u);
    EXPECT_EQ(callbacks.Queues[0].QueueId, FirstQueueId);
    EXPECT_TRUE(callbacks.Queues[0].Name.empty());
    EXPECT_EQ(callbacks.Queues[0].Timestamp, 120);
}

// A full multi-queue, multi-file workload with interleaved operations produces one
// correlated result per completed operation, with ids and timestamps intact.
TEST(DirectStorage, FullWorkload)
{
    DirectStorageFixture fixture;
    fixture.CreateQueue(100, 0xA000, "Direct");
    fixture.CreateQueue(101, 0xB000, "Copy");
    fixture.OpenFile(110, 0xF000, L"level0.pak");
    fixture.OpenFile(111, 0xF100, L"level1.pak");

    // Two reads on different queues/files, interleaved.
    fixture.EnqueueRequestV1(120, 0xA000, 0x1000, 0xF000, 1024, 0, 0);
    fixture.EnqueueRequestV2(121, 0xB000, 0x1001, 0xF100, 2048, 4096, 1, 2);
    fixture.Submit(122, 0xA000, false);
    fixture.Submit(123, 0xB000, true);
    fixture.EnqueueStatus(124, 0xA000, 0x5000, 1);
    fixture.EnqueueSignal(125, 0xB000, 0x6000, 42);
    fixture.EnqueueSetEvent(126, 0xA000, 0x7000);

    fixture.RequestCompleted(200, 0xA000, 0x1000);
    fixture.RequestCompleted(201, 0xB000, 0x1001);
    fixture.StatusCompleted(202, 0xA000, 0x5000, 1);
    fixture.SignalCompleted(203, 0xB000, 0x6000, 42);
    fixture.SetEventCompleted(204, 0xA000, 0x7000);

    fixture.CloseFile(210, 0xF000);
    fixture.CloseQueue(211, 0xA000);
    fixture.Run();

    auto& callbacks = *fixture.Callbacks;
    EXPECT_EQ(callbacks.Queues.size(), 2u);
    EXPECT_EQ(callbacks.Files.size(), 2u);
    EXPECT_EQ(callbacks.Reads.size(), 2u);
    EXPECT_EQ(callbacks.Statuses.size(), 1u);
    EXPECT_EQ(callbacks.Fences.size(), 1u);
    EXPECT_EQ(callbacks.SetEvents.size(), 1u);
    EXPECT_EQ(callbacks.Submits.size(), 2u);

    // The two reads carry their own queue/file ids.
    ASSERT_EQ(callbacks.Reads.size(), 2u);
    EXPECT_EQ(callbacks.Reads[0].QueueId, 2);
    EXPECT_EQ(callbacks.Reads[0].FileId, 2);
    EXPECT_EQ(callbacks.Reads[0].CompletionTimestamp, 200);
    EXPECT_EQ(callbacks.Reads[1].QueueId, 3);
    EXPECT_EQ(callbacks.Reads[1].FileId, 3);
    EXPECT_EQ(callbacks.Reads[1].ShuffleType, 2u);
    EXPECT_EQ(callbacks.Reads[1].CompletionTimestamp, 201);
}

// Correlation must be keyed on the request id, not the arrival order of completions.
// Two reads on different queues/files are enqueued, then completed in the opposite
// order; each reported read must carry the fields of its own enqueue (queue, file,
// offset, size) paired with its own completion timestamp. A positional match would
// swap the completion timestamps.
TEST(DirectStorage, ReadCompletionsCorrelateByRequestIdNotArrivalOrder)
{
    DirectStorageFixture fixture;
    fixture.CreateQueue(100, 0xA000, "QueueA");
    fixture.CreateQueue(101, 0xB000, "QueueB");
    fixture.OpenFile(110, 0xF000, L"a.pak");
    fixture.OpenFile(111, 0xF100, L"b.pak");
    fixture.EnqueueRequestV1(120, 0xA000, /*request*/ 0x1000, 0xF000, /*size*/ 1024, /*offset*/ 10, 0);
    fixture.EnqueueRequestV1(121, 0xB000, /*request*/ 0x1001, 0xF100, /*size*/ 2048, /*offset*/ 20, 0);

    // Complete in reverse enqueue order.
    fixture.RequestCompleted(200, 0xB000, 0x1001);
    fixture.RequestCompleted(201, 0xA000, 0x1000);
    fixture.Run();

    auto& callbacks = *fixture.Callbacks;
    ASSERT_EQ(callbacks.Reads.size(), 2u);

    // First completion is request 0x1001 (queue B / file b.pak).
    EXPECT_EQ(callbacks.Reads[0].QueueId, 3);
    EXPECT_EQ(callbacks.Reads[0].FileId, 3);
    EXPECT_EQ(callbacks.Reads[0].Offset, 20u);
    EXPECT_EQ(callbacks.Reads[0].Size, 2048u);
    EXPECT_EQ(callbacks.Reads[0].EnqueueTimestamp, 121);
    EXPECT_EQ(callbacks.Reads[0].CompletionTimestamp, 200);

    // Second completion is request 0x1000 (queue A / file a.pak).
    EXPECT_EQ(callbacks.Reads[1].QueueId, 2);
    EXPECT_EQ(callbacks.Reads[1].FileId, 2);
    EXPECT_EQ(callbacks.Reads[1].Offset, 10u);
    EXPECT_EQ(callbacks.Reads[1].Size, 1024u);
    EXPECT_EQ(callbacks.Reads[1].EnqueueTimestamp, 120);
    EXPECT_EQ(callbacks.Reads[1].CompletionTimestamp, 201);
}

// The same keyed-correlation guard for the status/fence/set-event paths: two of each
// with distinct keys, completed in reverse order, must each pair with their own
// enqueue rather than matching positionally.
TEST(DirectStorage, StatusFenceAndSetEventCorrelateByKeyNotArrivalOrder)
{
    DirectStorageFixture fixture;
    fixture.CreateQueue(100, 0xA000, "Queue");

    // Status array, keyed on (array, index).
    fixture.EnqueueStatus(120, 0xA000, 0x5000, /*index*/ 1);
    fixture.EnqueueStatus(121, 0xA000, 0x5000, /*index*/ 2);
    // Fence, keyed on (fence, value).
    fixture.EnqueueSignal(130, 0xA000, 0x6000, /*value*/ 10);
    fixture.EnqueueSignal(131, 0xA000, 0x6000, /*value*/ 20);
    // Set-event, keyed on the handle.
    fixture.EnqueueSetEvent(140, 0xA000, /*handle*/ 0x7000);
    fixture.EnqueueSetEvent(141, 0xA000, /*handle*/ 0x7001);

    // Complete each pair in reverse enqueue order.
    fixture.StatusCompleted(200, 0xA000, 0x5000, 2);
    fixture.StatusCompleted(201, 0xA000, 0x5000, 1);
    fixture.SignalCompleted(210, 0xA000, 0x6000, 20);
    fixture.SignalCompleted(211, 0xA000, 0x6000, 10);
    fixture.SetEventCompleted(220, 0xA000, 0x7001);
    fixture.SetEventCompleted(221, 0xA000, 0x7000);
    fixture.Run();

    auto& callbacks = *fixture.Callbacks;

    ASSERT_EQ(callbacks.Statuses.size(), 2u);
    EXPECT_EQ(callbacks.Statuses[0].Index, 2u);
    EXPECT_EQ(callbacks.Statuses[0].EnqueueTimestamp, 121);
    EXPECT_EQ(callbacks.Statuses[0].CompletionTimestamp, 200);
    EXPECT_EQ(callbacks.Statuses[1].Index, 1u);
    EXPECT_EQ(callbacks.Statuses[1].EnqueueTimestamp, 120);
    EXPECT_EQ(callbacks.Statuses[1].CompletionTimestamp, 201);

    ASSERT_EQ(callbacks.Fences.size(), 2u);
    EXPECT_EQ(callbacks.Fences[0].Value, 20u);
    EXPECT_EQ(callbacks.Fences[0].EnqueueTimestamp, 131);
    EXPECT_EQ(callbacks.Fences[0].CompletionTimestamp, 210);
    EXPECT_EQ(callbacks.Fences[1].Value, 10u);
    EXPECT_EQ(callbacks.Fences[1].EnqueueTimestamp, 130);
    EXPECT_EQ(callbacks.Fences[1].CompletionTimestamp, 211);

    ASSERT_EQ(callbacks.SetEvents.size(), 2u);
    EXPECT_EQ(callbacks.SetEvents[0].Handle, 0x7001u);
    EXPECT_EQ(callbacks.SetEvents[0].EnqueueTimestamp, 141);
    EXPECT_EQ(callbacks.SetEvents[0].CompletionTimestamp, 220);
    EXPECT_EQ(callbacks.SetEvents[1].Handle, 0x7000u);
    EXPECT_EQ(callbacks.SetEvents[1].EnqueueTimestamp, 140);
    EXPECT_EQ(callbacks.SetEvents[1].CompletionTimestamp, 221);
}

// Closing a file untracks its handle, so if the runtime reuses that raw handle for a
// different file it must be registered as a new id/name rather than aliasing the old
// one. Without untracking, the reopen would resolve to the stale id and emit no new
// file, and the later read would be misattributed to the closed file.
TEST(DirectStorage, ClosedFileHandleReuseGetsNewId)
{
    DirectStorageFixture fixture;
    fixture.OpenFile(110, 0xF000, L"first.pak");
    fixture.CloseFile(120, 0xF000);
    fixture.OpenFile(130, 0xF000, L"second.pak"); // same raw handle, different file
    fixture.EnqueueRequestV1(140, 0xA000, 0x1000, 0xF000, 1024, 0, 0);
    fixture.RequestCompleted(200, 0xA000, 0x1000);
    fixture.Run();

    auto& callbacks = *fixture.Callbacks;
    ASSERT_EQ(callbacks.Files.size(), 2u);
    EXPECT_EQ(callbacks.Files[0].FileId, 2);
    EXPECT_EQ(callbacks.Files[0].Path, L"first.pak");
    EXPECT_EQ(callbacks.Files[1].FileId, 3);
    EXPECT_EQ(callbacks.Files[1].Path, L"second.pak");

    ASSERT_EQ(callbacks.Reads.size(), 1u);
    EXPECT_EQ(callbacks.Reads[0].FileId, 3); // the read maps to the reopened file
}

// As above for queues: closing a queue untracks its handle, so reusing that raw handle
// for a new queue produces a new id/name and later work correlates to the new queue.
TEST(DirectStorage, ClosedQueueHandleReuseGetsNewId)
{
    DirectStorageFixture fixture;
    fixture.CreateQueue(100, 0xA000, "First");
    fixture.CloseQueue(110, 0xA000);
    fixture.CreateQueue(120, 0xA000, "Second"); // same raw handle, different queue
    fixture.Submit(130, 0xA000, false);
    fixture.Run();

    auto& callbacks = *fixture.Callbacks;
    ASSERT_EQ(callbacks.Queues.size(), 2u);
    EXPECT_EQ(callbacks.Queues[0].QueueId, 2);
    EXPECT_EQ(callbacks.Queues[0].Name, "First");
    EXPECT_EQ(callbacks.Queues[1].QueueId, 3);
    EXPECT_EQ(callbacks.Queues[1].Name, "Second");

    ASSERT_EQ(callbacks.Submits.size(), 1u);
    EXPECT_EQ(callbacks.Submits[0].QueueId, 3); // submit correlates to the recreated queue
}

// The public callback contract promises a failing HRESULT from any DirectStorage
// callback is fatal and propagates out of HandleEventRecord. This covers the deferred
// read-completion path.
TEST(DirectStorage, ReadCompletionCallbackFailurePropagates)
{
    DirectStorageFixture fixture;
    fixture.CreateQueue(100, 0xA000, "Queue");
    fixture.OpenFile(110, 0xF000, L"data.bin");
    fixture.EnqueueRequestV1(120, 0xA000, 0x1000, 0xF000, 1024, 0, 0);
    fixture.RequestCompleted(200, 0xA000, 0x1000);
    fixture.Callbacks->ReadResult = E_FAIL;

    EXPECT_ANY_THROW(fixture.Run());
}

// As above for an immediate callback fired during decode: a failing queue-creation
// callback must propagate.
TEST(DirectStorage, QueueCallbackFailurePropagates)
{
    DirectStorageFixture fixture;
    fixture.CreateQueue(100, 0xA000, "Queue");
    fixture.Callbacks->QueueResult = E_FAIL;

    EXPECT_ANY_THROW(fixture.Run());
}

// As above for the file-open callback.
TEST(DirectStorage, FileCallbackFailurePropagates)
{
    DirectStorageFixture fixture;
    fixture.OpenFile(110, 0xF000, L"data.bin");
    fixture.Callbacks->FileResult = E_FAIL;

    EXPECT_ANY_THROW(fixture.Run());
}

// As above for the submit callback.
TEST(DirectStorage, SubmitCallbackFailurePropagates)
{
    DirectStorageFixture fixture;
    fixture.CreateQueue(100, 0xA000, "Queue");
    fixture.Submit(130, 0xA000, false);
    fixture.Callbacks->SubmitResult = E_FAIL;

    EXPECT_ANY_THROW(fixture.Run());
}
