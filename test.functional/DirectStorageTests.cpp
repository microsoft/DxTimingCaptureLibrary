// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "pch.h"

#include <dstorage.h>

#include <algorithm>
#include <array>
#include <cwctype>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include <DxTimingCaptureLibrary/DxTimingCaptureEventHandler.h>
#include <DxTimingCaptureLibrary/TicksToNanoseconds.h>

#include "EtwSession.h"
#include "RecordingDirectStorageCallbacks.h"

using namespace DirectX::Etw;

// End-to-end tests against the real DirectStorage runtime: start a live ETW session, drive
// actual DirectStorage work (open files, create queues, enqueue/submit reads, status, fences,
// set-events), then assert the library decoded and correlated it into matching callbacks.
//
// Each assertion finds its record by an identity the test controls (unique path, queue name,
// offset/size, fence value, handle) rather than by position, so unrelated in-process
// DirectStorage activity can't cause a false pass. Timestamps are QPC-derived nanoseconds, so
// tests bracket them against their own QueryPerformanceCounter samples to prove
// enqueue <= submit <= completion ordering.

namespace
{
    // Reads here are tiny so completion is near-immediate; this generous bound just avoids
    // hanging a broken run forever.
    constexpr DWORD c_completionTimeoutMs = 30000;

    // Real-time ETW delivery can lag the completion event slightly, so let the buffers flush
    // before we stop the trace and lose the completion records.
    constexpr DWORD c_etwFlushSettleMs = 750;

    // Slack for bracketing a timestamp inside the session window. Large enough to absorb the
    // gap between our "now" sample and provider-enable / trace-stop, small enough that a
    // zero / wrong-clock timestamp still can't slip through.
    constexpr INT64 c_clockSlackNs = 50'000'000; // 50 ms

    // ThrowIf takes an HRESULT, so convert GetLastError() for the Win32 calls below.
    void ThrowLastErrorIf(bool condition, const wchar_t* message)
    {
        if (condition)
        {
            ThrowToolException(HRESULT_FROM_WIN32(GetLastError()), message);
        }
    }

    // Raw QPC ticks converted to nanoseconds the same way the library stamps callbacks, so
    // the two are directly comparable.
    INT64 NowInLibraryNanoseconds()
    {
        LARGE_INTEGER counter;
        LARGE_INTEGER frequency;
        QueryPerformanceCounter(&counter);
        QueryPerformanceFrequency(&frequency);
        return static_cast<INT64>(TicksToNanoseconds(
            static_cast<uint64_t>(counter.QuadPart),
            static_cast<uint64_t>(frequency.QuadPart)));
    }

    // Nanosecond bracket around a workload: LowerNs sampled just after Begin(), UpperNs just
    // after the completion wait returns. Every callback the test correlates falls inside it.
    struct SessionWindow
    {
        INT64 LowerNs = 0;
        INT64 UpperNs = 0;
    };

    ::testing::AssertionResult WithinWindow(const SessionWindow& window, INT64 timestamp, const char* what)
    {
        if (timestamp >= window.LowerNs - c_clockSlackNs && timestamp <= window.UpperNs + c_clockSlackNs)
        {
            return ::testing::AssertionSuccess();
        }
        return ::testing::AssertionFailure()
            << what << " timestamp " << timestamp << " ns is outside the session window ["
            << window.LowerNs << ", " << window.UpperNs << "]";
    }

    // Asserts the timestamp invariants common to any correlated (enqueue-seen) operation.
    void ExpectCorrelatedTimestamps(const SessionWindow& window, INT64 enqueueNs, INT64 completionNs, const char* what)
    {
        EXPECT_GT(enqueueNs, 0) << what << " enqueue timestamp must be > 0";
        EXPECT_GT(completionNs, 0) << what << " completion timestamp must be > 0";
        EXPECT_GE(completionNs, enqueueNs) << what << " completion must be >= enqueue";
        EXPECT_TRUE(WithinWindow(window, enqueueNs, what));
        EXPECT_TRUE(WithinWindow(window, completionNs, what));
    }

    // Busy-waits until the library clock passes ns. Used between enqueues so their timestamp
    // windows are disjoint, making "enqueue timestamps strictly increase" provable rather than
    // dependent on QPC resolution. A QPC tick is sub-microsecond, so this spins only briefly.
    void SpinUntilAfter(INT64 ns)
    {
        while (NowInLibraryNanoseconds() <= ns)
        {
            YieldProcessor();
        }
    }

    std::wstring ToLower(std::wstring_view value)
    {
        std::wstring lowered(value);
        std::ranges::transform(lowered, lowered.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
        return lowered;
    }

    bool ContainsCaseInsensitive(std::wstring_view haystack, std::wstring_view needle)
    {
        return ToLower(haystack).find(ToLower(needle)) != std::wstring::npos;
    }

    class UniqueEvent
    {
        HANDLE m_handle;

    public:
        UniqueEvent()
            : m_handle(CreateEventW(nullptr, FALSE, FALSE, nullptr))
        {
            ThrowLastErrorIf(m_handle == nullptr, L"CreateEventW failed");
        }

        ~UniqueEvent()
        {
            if (m_handle != nullptr)
            {
                CloseHandle(m_handle);
            }
        }

        UniqueEvent(const UniqueEvent&) = delete;
        UniqueEvent& operator=(const UniqueEvent&) = delete;

        HANDLE Get() const { return m_handle; }
    };

    // A temp file seeded with deterministic bytes, deleted on destruction. Reads pull a
    // sub-range and compare it to the known bytes, so a successful read validates the data
    // path, not just the ETW decode.
    class TempFile
    {
        std::wstring m_path;

    public:
        std::vector<uint8_t> Contents;

        // seed distinguishes two temp files created in the same test.
        explicit TempFile(size_t sizeInBytes, uint8_t seed = 7u)
        {
            wchar_t directory[MAX_PATH]{};
            ThrowLastErrorIf(GetTempPathW(MAX_PATH, directory) == 0, L"GetTempPathW failed");

            wchar_t path[MAX_PATH]{};
            ThrowLastErrorIf(GetTempFileNameW(directory, L"dse", 0, path) == 0, L"GetTempFileNameW failed");
            m_path = path;

            Contents.resize(sizeInBytes);
            for (size_t i = 0; i < sizeInBytes; ++i)
            {
                Contents[i] = static_cast<uint8_t>((i * 131u + seed) & 0xFF);
            }

            WriteAllBytes(Contents);
        }

        // Overwrites the file (the compression test stores a compressed payload) and keeps
        // Contents in sync with disk.
        void WriteAllBytes(std::span<const uint8_t> bytes)
        {
            HANDLE file = CreateFileW(m_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            ThrowLastErrorIf(file == INVALID_HANDLE_VALUE, L"CreateFileW (temp file) failed");

            DWORD written = 0;
            BOOL ok = WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
            CloseHandle(file);
            ThrowIf(!ok || written != bytes.size(), E_FAIL, L"WriteFile (temp file) failed");
        }

        ~TempFile()
        {
            DeleteFileW(m_path.c_str());
        }

        TempFile(const TempFile&) = delete;
        TempFile& operator=(const TempFile&) = delete;

        const std::wstring& Path() const { return m_path; }

        std::wstring FileName() const
        {
            auto slash = m_path.find_last_of(L"\\/");
            return slash == std::wstring::npos ? m_path : m_path.substr(slash + 1);
        }
    };

    ComPtr<IDStorageFactory> CreateFactory(bool recordObjectNames = true)
    {
        ComPtr<IDStorageFactory> factory;
        ThrowFailure(DStorageGetFactory(IID_PPV_ARGS(factory.GetAddressOf())));

        // Queue names and file paths only reach ETW when object-name recording is on. Tests
        // that assert names enable it; the fallback test leaves it off to exercise the
        // synthesized-name path.
        factory->SetDebugFlags(recordObjectNames ? DSTORAGE_DEBUG_RECORD_OBJECT_NAMES : DSTORAGE_DEBUG_NONE);
        return factory;
    }

    ComPtr<IDStorageQueue1> CreateQueue(
        IDStorageFactory* factory,
        const char* name,
        ID3D12Device* device,
        DSTORAGE_PRIORITY priority = DSTORAGE_PRIORITY_NORMAL,
        DSTORAGE_REQUEST_SOURCE_TYPE sourceType = DSTORAGE_REQUEST_SOURCE_FILE,
        UINT16 capacity = DSTORAGE_MIN_QUEUE_CAPACITY)
    {
        DSTORAGE_QUEUE_DESC desc{};
        desc.Capacity = capacity;
        desc.Priority = priority;
        desc.SourceType = sourceType;
        desc.Name = name;
        desc.Device = device;

        ComPtr<IDStorageQueue1> queue;
        ThrowFailure(factory->CreateQueue(&desc, IID_PPV_ARGS(queue.GetAddressOf())));
        return queue;
    }

    ComPtr<IDStorageFile> OpenFile(IDStorageFactory* factory, const std::wstring& path)
    {
        ComPtr<IDStorageFile> file;
        ThrowFailure(factory->OpenFile(path.c_str(), IID_PPV_ARGS(file.GetAddressOf())));
        return file;
    }

    // Builds a file -> memory read request for [offset, offset + size) with no compression.
    DSTORAGE_REQUEST MakeFileToMemoryRequest(IDStorageFile* file, uint64_t offset, uint32_t size, void* destination)
    {
        DSTORAGE_REQUEST request{};
        request.Options.CompressionFormat = DSTORAGE_COMPRESSION_FORMAT_NONE;
        request.Options.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
        request.Options.DestinationType = DSTORAGE_REQUEST_DESTINATION_MEMORY;
        request.Source.File.Source = file;
        request.Source.File.Offset = offset;
        request.Source.File.Size = size;
        request.Destination.Memory.Buffer = destination;
        request.Destination.Memory.Size = size;
        request.UncompressedSize = size;
        return request;
    }

    // A memory source has no IDStorageFile, so the enqueue event carries a null file handle,
    // driving the library's synthesized "file# <id>" placeholder path.
    DSTORAGE_REQUEST MakeMemoryToMemoryRequest(const void* source, uint32_t size, void* destination)
    {
        DSTORAGE_REQUEST request{};
        request.Options.CompressionFormat = DSTORAGE_COMPRESSION_FORMAT_NONE;
        request.Options.SourceType = DSTORAGE_REQUEST_SOURCE_MEMORY;
        request.Options.DestinationType = DSTORAGE_REQUEST_DESTINATION_MEMORY;
        request.Source.Memory.Source = source;
        request.Source.Memory.Size = size;
        request.Destination.Memory.Buffer = destination;
        request.Destination.Memory.Size = size;
        request.UncompressedSize = size;
        return request;
    }

    ComPtr<ID3D12Device> TryCreateDevice()
    {
        ComPtr<ID3D12Device> device;
        if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(device.GetAddressOf()))))
        {
            return nullptr;
        }
        return device;
    }

    // Runs fn on a fresh thread and returns that thread's OS thread id (fn also receives it).
    // The thread is joined before returning.
    DWORD RunOnDedicatedThread(const std::function<void(DWORD)>& fn)
    {
        DWORD threadId = 0;
        std::thread worker([&]()
        {
            threadId = GetCurrentThreadId();
            fn(threadId);
        });
        worker.join();
        return threadId;
    }

    // Raw handle/pointer value as the library reports it (data.Handle / data.Fence /
    // data.StatusArray hold the raw values ETW logged).
    UINT64 AsHandleValue(void* pointer)
    {
        return reinterpret_cast<UINT64>(pointer);
    }
}

// ---------------------------------------------------------------------------------------
// Core correlation and timestamp rigor
// ---------------------------------------------------------------------------------------

// One ordinary read, with the full timestamp story: every callback timestamp is inside the
// session window, the enqueue <= submit <= completion lifecycle holds, and the set-event and
// status carry the exact handle and status-array identities the test used.
TEST(DirectStorageFunctional, SingleReadWorkloadTimestampsAreBoundedAndOrdered)
{
    constexpr uint64_t readOffset = 256;
    constexpr uint32_t readSize = 512;
    const char* queueName = "DxTimingCaptureLibrary Functional Single Read Queue";

    TempFile sourceFile(1024);

    auto callbacks = std::make_unique<RecordingDirectStorageCallbacks>();
    auto* recorder = callbacks.get();

    SimpleEtwSession session;
    session.SetDirectStorageCallbacks(std::move(callbacks));
    session.Begin();

    SessionWindow window;
    window.LowerNs = NowInLibraryNanoseconds();

    std::vector<uint8_t> destination(readSize);
    const DWORD mainThreadId = GetCurrentThreadId();

    {
        ComPtr<IDStorageFactory> factory = CreateFactory();
        ComPtr<IDStorageFile> file = OpenFile(factory.Get(), sourceFile.Path());
        ComPtr<IDStorageQueue1> queue = CreateQueue(factory.Get(), queueName, nullptr);

        ComPtr<IDStorageStatusArray> statusArray;
        ThrowFailure(factory->CreateStatusArray(DSTORAGE_MIN_QUEUE_CAPACITY, "Single Read Status", IID_PPV_ARGS(statusArray.GetAddressOf())));

        UniqueEvent completed;

        DSTORAGE_REQUEST request = MakeFileToMemoryRequest(file.Get(), readOffset, readSize, destination.data());
        queue->EnqueueRequest(&request);
        queue->EnqueueStatus(statusArray.Get(), 0);
        queue->EnqueueSetEvent(completed.Get());
        queue->Submit();

        ASSERT_EQ(WaitForSingleObject(completed.Get(), c_completionTimeoutMs), WAIT_OBJECT_0)
            << "DirectStorage read did not complete in time";
        ASSERT_HRESULT_SUCCEEDED(statusArray->GetHResult(0)) << "DirectStorage read reported a failure";

        EXPECT_TRUE(std::equal(destination.begin(), destination.end(), sourceFile.Contents.begin() + readOffset))
            << "DirectStorage read returned unexpected data";

        window.UpperNs = NowInLibraryNanoseconds();
    }

    Sleep(c_etwFlushSettleMs);
    session.End();
    session.RethrowConsumerException();

    EXPECT_EQ(session.CountDiagnostics(DiagnosticCode::EventsLost), 0u) << "ETW dropped events; the run is unreliable";
    EXPECT_EQ(session.CountDiagnostics(DiagnosticCode::BuffersLost), 0u) << "ETW dropped buffers; the run is unreliable";

    auto fileRecord = std::ranges::find_if(recorder->Files, [&](const auto& f)
    {
        return ContainsCaseInsensitive(f.Path, sourceFile.FileName());
    });
    ASSERT_NE(fileRecord, recorder->Files.end()) << "No DirectStorage file callback named the temp file";
    EXPECT_GE(fileRecord->FileId, 2);
    EXPECT_TRUE(WithinWindow(window, fileRecord->Timestamp, "file"));

    auto queueRecord = std::ranges::find_if(recorder->Queues, [&](const auto& q) { return q.Name == queueName; });
    ASSERT_NE(queueRecord, recorder->Queues.end()) << "No DirectStorage queue callback carried our queue name";
    EXPECT_GE(queueRecord->QueueId, 2);
    EXPECT_TRUE(WithinWindow(window, queueRecord->Timestamp, "queue"));

    auto readRecord = std::ranges::find_if(recorder->Reads, [&](const auto& r)
    {
        return r.FileId == fileRecord->FileId && r.QueueId == queueRecord->QueueId
            && r.Offset == readOffset && r.Size == readSize;
    });
    ASSERT_NE(readRecord, recorder->Reads.end()) << "No correlated DirectStorage read matched our request";
    EXPECT_EQ(readRecord->CompressionType, DSTORAGE_COMPRESSION_FORMAT_NONE);
    EXPECT_EQ(readRecord->ShuffleType, 0);
    EXPECT_EQ(readRecord->EnqueueThreadId, mainThreadId);
    ExpectCorrelatedTimestamps(window, readRecord->EnqueueTimestamp, readRecord->CompletionTimestamp, "read");

    auto submitRecord = std::ranges::find_if(recorder->Submits, [&](const auto& s) { return s.QueueId == queueRecord->QueueId; });
    ASSERT_NE(submitRecord, recorder->Submits.end()) << "No DirectStorage submit for our queue";
    EXPECT_EQ(submitRecord->EnqueueThreadId, mainThreadId);
    EXPECT_FALSE(submitRecord->AutoSubmitted);
    EXPECT_TRUE(WithinWindow(window, submitRecord->Timestamp, "submit"));

    // Lifecycle ordering: enqueue <= submit <= completion.
    EXPECT_LE(readRecord->EnqueueTimestamp, submitRecord->Timestamp) << "read enqueue must not be after submit";
    EXPECT_LE(submitRecord->Timestamp, readRecord->CompletionTimestamp) << "submit must not be after completion";

    auto setEventRecord = std::ranges::find_if(recorder->SetEvents, [&](const auto& e) { return e.QueueId == queueRecord->QueueId; });
    ASSERT_NE(setEventRecord, recorder->SetEvents.end()) << "No DirectStorage set-event for our queue";
    EXPECT_EQ(setEventRecord->EnqueueThreadId, mainThreadId);
    ExpectCorrelatedTimestamps(window, setEventRecord->EnqueueTimestamp, setEventRecord->CompletionTimestamp, "set-event");

    auto statusRecord = std::ranges::find_if(recorder->Statuses, [&](const auto& s)
    {
        return s.QueueId == queueRecord->QueueId && s.Index == 0;
    });
    ASSERT_NE(statusRecord, recorder->Statuses.end()) << "No DirectStorage status for our queue";
    EXPECT_EQ(statusRecord->EnqueueThreadId, mainThreadId);
    ExpectCorrelatedTimestamps(window, statusRecord->EnqueueTimestamp, statusRecord->CompletionTimestamp, "status");

    // The completion primitives are queue barriers: they complete no earlier than the read
    // they follow.
    EXPECT_GE(setEventRecord->CompletionTimestamp, readRecord->CompletionTimestamp);
    EXPECT_GE(statusRecord->CompletionTimestamp, readRecord->CompletionTimestamp);
}

// Multiple reads on one queue must each surface as a distinct ReadRequest with the right
// fields and exact count; enqueued in order with disjoint timestamp windows, their enqueue
// timestamps must strictly increase.
TEST(DirectStorageFunctional, MultipleReadsOnOneQueueCorrelateWithIncreasingEnqueueTimes)
{
    struct PlannedRead { uint64_t Offset; uint32_t Size; };
    constexpr std::array<PlannedRead, 4> reads{ { {0, 128}, {256, 128}, {512, 128}, {768, 128} } };
    const char* queueName = "DxTimingCaptureLibrary Functional Multi Read Queue";

    TempFile sourceFile(1024);

    auto callbacks = std::make_unique<RecordingDirectStorageCallbacks>();
    auto* recorder = callbacks.get();

    SimpleEtwSession session;
    session.SetDirectStorageCallbacks(std::move(callbacks));
    session.Begin();

    SessionWindow window;
    window.LowerNs = NowInLibraryNanoseconds();

    std::array<std::vector<uint8_t>, reads.size()> destinations;
    for (auto& d : destinations)
    {
        d.resize(128);
    }
    const DWORD mainThreadId = GetCurrentThreadId();

    {
        ComPtr<IDStorageFactory> factory = CreateFactory();
        ComPtr<IDStorageFile> file = OpenFile(factory.Get(), sourceFile.Path());
        ComPtr<IDStorageQueue1> queue = CreateQueue(factory.Get(), queueName, nullptr);
        UniqueEvent completed;

        std::array<DSTORAGE_REQUEST, reads.size()> requests{};
        INT64 previousUpper = 0;
        for (size_t i = 0; i < reads.size(); ++i)
        {
            // Disjoint enqueue windows -> provably increasing enqueue timestamps.
            SpinUntilAfter(previousUpper);
            requests[i] = MakeFileToMemoryRequest(file.Get(), reads[i].Offset, reads[i].Size, destinations[i].data());
            queue->EnqueueRequest(&requests[i]);
            previousUpper = NowInLibraryNanoseconds();
        }
        queue->EnqueueSetEvent(completed.Get());
        queue->Submit();

        ASSERT_EQ(WaitForSingleObject(completed.Get(), c_completionTimeoutMs), WAIT_OBJECT_0)
            << "DirectStorage reads did not complete in time";

        for (size_t i = 0; i < reads.size(); ++i)
        {
            EXPECT_TRUE(std::equal(destinations[i].begin(), destinations[i].end(), sourceFile.Contents.begin() + reads[i].Offset))
                << "read " << i << " returned unexpected data";
        }

        window.UpperNs = NowInLibraryNanoseconds();
    }

    Sleep(c_etwFlushSettleMs);
    session.End();
    session.RethrowConsumerException();
    EXPECT_EQ(session.CountDiagnostics(DiagnosticCode::EventsLost), 0u);

    auto queueRecord = std::ranges::find_if(recorder->Queues, [&](const auto& q) { return q.Name == queueName; });
    ASSERT_NE(queueRecord, recorder->Queues.end());

    // Recover each planned read (by offset) and assert it is present exactly once.
    std::vector<const DirectStorageCallbacks::ReadRequest*> matched(reads.size(), nullptr);
    for (size_t i = 0; i < reads.size(); ++i)
    {
        int count = 0;
        for (const auto& r : recorder->Reads)
        {
            if (r.QueueId == queueRecord->QueueId && r.Offset == reads[i].Offset && r.Size == reads[i].Size)
            {
                matched[i] = &r;
                ++count;
            }
        }
        ASSERT_NE(matched[i], nullptr) << "planned read " << i << " (offset " << reads[i].Offset << ") missing";
        EXPECT_EQ(count, 1) << "planned read " << i << " appeared more than once";
        EXPECT_EQ(matched[i]->CompressionType, DSTORAGE_COMPRESSION_FORMAT_NONE);
        EXPECT_EQ(matched[i]->EnqueueThreadId, mainThreadId);
        ExpectCorrelatedTimestamps(window, matched[i]->EnqueueTimestamp, matched[i]->CompletionTimestamp, "read");
    }

    // Strictly increasing enqueue timestamps in program order.
    for (size_t i = 1; i < reads.size(); ++i)
    {
        EXPECT_LT(matched[i - 1]->EnqueueTimestamp, matched[i]->EnqueueTimestamp)
            << "enqueue timestamps must strictly increase in program order (read " << (i - 1) << " -> " << i << ")";
    }

    // The single submit occurs after every read's enqueue and before every read's completion.
    auto submitRecord = std::ranges::find_if(recorder->Submits, [&](const auto& s) { return s.QueueId == queueRecord->QueueId; });
    ASSERT_NE(submitRecord, recorder->Submits.end());
    for (const auto* r : matched)
    {
        EXPECT_LE(r->EnqueueTimestamp, submitRecord->Timestamp);
        EXPECT_LE(submitRecord->Timestamp, r->CompletionTimestamp);
    }
}

// Reads against two different files must correlate to the right, distinct fileId and path.
TEST(DirectStorageFunctional, ReadsCorrelateToTheCorrectFileAcrossMultipleFiles)
{
    const char* queueName = "DxTimingCaptureLibrary Functional Multi File Queue";

    TempFile fileA(512, 0x11);
    TempFile fileB(512, 0x77);

    auto callbacks = std::make_unique<RecordingDirectStorageCallbacks>();
    auto* recorder = callbacks.get();

    SimpleEtwSession session;
    session.SetDirectStorageCallbacks(std::move(callbacks));
    session.Begin();

    SessionWindow window;
    window.LowerNs = NowInLibraryNanoseconds();

    std::vector<uint8_t> destinationA(128);
    std::vector<uint8_t> destinationB(128);

    {
        ComPtr<IDStorageFactory> factory = CreateFactory();
        ComPtr<IDStorageFile> openA = OpenFile(factory.Get(), fileA.Path());
        ComPtr<IDStorageFile> openB = OpenFile(factory.Get(), fileB.Path());
        ComPtr<IDStorageQueue1> queue = CreateQueue(factory.Get(), queueName, nullptr);
        UniqueEvent completed;

        DSTORAGE_REQUEST readA = MakeFileToMemoryRequest(openA.Get(), 0, 128, destinationA.data());
        DSTORAGE_REQUEST readB = MakeFileToMemoryRequest(openB.Get(), 64, 128, destinationB.data());
        queue->EnqueueRequest(&readA);
        queue->EnqueueRequest(&readB);
        queue->EnqueueSetEvent(completed.Get());
        queue->Submit();

        ASSERT_EQ(WaitForSingleObject(completed.Get(), c_completionTimeoutMs), WAIT_OBJECT_0);
        EXPECT_TRUE(std::equal(destinationA.begin(), destinationA.end(), fileA.Contents.begin()));
        EXPECT_TRUE(std::equal(destinationB.begin(), destinationB.end(), fileB.Contents.begin() + 64));

        window.UpperNs = NowInLibraryNanoseconds();
    }

    Sleep(c_etwFlushSettleMs);
    session.End();
    session.RethrowConsumerException();

    auto recordA = std::ranges::find_if(recorder->Files, [&](const auto& f) { return ContainsCaseInsensitive(f.Path, fileA.FileName()); });
    auto recordB = std::ranges::find_if(recorder->Files, [&](const auto& f) { return ContainsCaseInsensitive(f.Path, fileB.FileName()); });
    ASSERT_NE(recordA, recorder->Files.end()) << "file A not reported";
    ASSERT_NE(recordB, recorder->Files.end()) << "file B not reported";
    EXPECT_NE(recordA->FileId, recordB->FileId) << "distinct files must get distinct ids";
    EXPECT_GE(recordA->FileId, 2);
    EXPECT_GE(recordB->FileId, 2);
    // First-opened file gets the smaller id (monotonic first-seen order).
    EXPECT_LT(recordA->FileId, recordB->FileId);
    EXPECT_TRUE(WithinWindow(window, recordA->Timestamp, "file A"));
    EXPECT_TRUE(WithinWindow(window, recordB->Timestamp, "file B"));

    auto queueRecord = std::ranges::find_if(recorder->Queues, [&](const auto& q) { return q.Name == queueName; });
    ASSERT_NE(queueRecord, recorder->Queues.end());

    auto readA = std::ranges::find_if(recorder->Reads, [&](const auto& r) { return r.QueueId == queueRecord->QueueId && r.Offset == 0 && r.Size == 128; });
    auto readB = std::ranges::find_if(recorder->Reads, [&](const auto& r) { return r.QueueId == queueRecord->QueueId && r.Offset == 64 && r.Size == 128; });
    ASSERT_NE(readA, recorder->Reads.end());
    ASSERT_NE(readB, recorder->Reads.end());
    // The reads must not be swapped: offset-0 read is from file A, offset-64 read from file B.
    EXPECT_EQ(readA->FileId, recordA->FileId);
    EXPECT_EQ(readB->FileId, recordB->FileId);
    ExpectCorrelatedTimestamps(window, readA->EnqueueTimestamp, readA->CompletionTimestamp, "read A");
    ExpectCorrelatedTimestamps(window, readB->EnqueueTimestamp, readB->CompletionTimestamp, "read B");
}

// Reads and submits across two queues (different names and priorities) must correlate to
// the correct, distinct queueId.
TEST(DirectStorageFunctional, ReadsAndSubmitsCorrelateToTheCorrectQueueAcrossMultipleQueues)
{
    const char* queueNameNormal = "DxTimingCaptureLibrary Functional Queue Normal";
    const char* queueNameHigh = "DxTimingCaptureLibrary Functional Queue High";

    TempFile sourceFile(1024);

    auto callbacks = std::make_unique<RecordingDirectStorageCallbacks>();
    auto* recorder = callbacks.get();

    SimpleEtwSession session;
    session.SetDirectStorageCallbacks(std::move(callbacks));
    session.Begin();

    SessionWindow window;
    window.LowerNs = NowInLibraryNanoseconds();

    std::vector<uint8_t> destination1(128);
    std::vector<uint8_t> destination2(128);

    {
        ComPtr<IDStorageFactory> factory = CreateFactory();
        ComPtr<IDStorageFile> file = OpenFile(factory.Get(), sourceFile.Path());
        ComPtr<IDStorageQueue1> queueNormal = CreateQueue(factory.Get(), queueNameNormal, nullptr, DSTORAGE_PRIORITY_NORMAL);
        ComPtr<IDStorageQueue1> queueHigh = CreateQueue(factory.Get(), queueNameHigh, nullptr, DSTORAGE_PRIORITY_HIGH);
        UniqueEvent completed1;
        UniqueEvent completed2;

        DSTORAGE_REQUEST read1 = MakeFileToMemoryRequest(file.Get(), 0, 128, destination1.data());
        DSTORAGE_REQUEST read2 = MakeFileToMemoryRequest(file.Get(), 256, 128, destination2.data());
        queueNormal->EnqueueRequest(&read1);
        queueNormal->EnqueueSetEvent(completed1.Get());
        queueHigh->EnqueueRequest(&read2);
        queueHigh->EnqueueSetEvent(completed2.Get());
        queueNormal->Submit();
        queueHigh->Submit();

        const HANDLE events[] = { completed1.Get(), completed2.Get() };
        ASSERT_EQ(WaitForMultipleObjects(2, events, TRUE, c_completionTimeoutMs), WAIT_OBJECT_0);

        window.UpperNs = NowInLibraryNanoseconds();
    }

    Sleep(c_etwFlushSettleMs);
    session.End();
    session.RethrowConsumerException();

    auto queueNormalRecord = std::ranges::find_if(recorder->Queues, [&](const auto& q) { return q.Name == queueNameNormal; });
    auto queueHighRecord = std::ranges::find_if(recorder->Queues, [&](const auto& q) { return q.Name == queueNameHigh; });
    ASSERT_NE(queueNormalRecord, recorder->Queues.end());
    ASSERT_NE(queueHighRecord, recorder->Queues.end());
    EXPECT_NE(queueNormalRecord->QueueId, queueHighRecord->QueueId);

    auto read1 = std::ranges::find_if(recorder->Reads, [&](const auto& r) { return r.Offset == 0 && r.Size == 128; });
    auto read2 = std::ranges::find_if(recorder->Reads, [&](const auto& r) { return r.Offset == 256 && r.Size == 128; });
    ASSERT_NE(read1, recorder->Reads.end());
    ASSERT_NE(read2, recorder->Reads.end());
    EXPECT_EQ(read1->QueueId, queueNormalRecord->QueueId);
    EXPECT_EQ(read2->QueueId, queueHighRecord->QueueId);

    // Exactly one submit per queue, each not auto-submitted, each bounded by its queue's read.
    for (const auto* qr : { &*queueNormalRecord, &*queueHighRecord })
    {
        int count = 0;
        const DirectStorageCallbacks::Submit* submit = nullptr;
        for (const auto& s : recorder->Submits)
        {
            if (s.QueueId == qr->QueueId)
            {
                submit = &s;
                ++count;
            }
        }
        ASSERT_NE(submit, nullptr) << "no submit for queue id " << qr->QueueId;
        EXPECT_EQ(count, 1) << "expected exactly one submit for queue id " << qr->QueueId;
        EXPECT_FALSE(submit->AutoSubmitted);
        EXPECT_TRUE(WithinWindow(window, submit->Timestamp, "submit"));
    }
}

// Status notifications at multiple indices on one status array must each correlate, carry the
// right index and status-array identity, and complete no earlier than the read they follow.
TEST(DirectStorageFunctional, StatusNotificationsAtMultipleIndicesCorrelate)
{
    const char* queueName = "DxTimingCaptureLibrary Functional Multi Status Queue";

    TempFile sourceFile(1024);

    auto callbacks = std::make_unique<RecordingDirectStorageCallbacks>();
    auto* recorder = callbacks.get();

    SimpleEtwSession session;
    session.SetDirectStorageCallbacks(std::move(callbacks));
    session.Begin();

    SessionWindow window;
    window.LowerNs = NowInLibraryNanoseconds();

    std::vector<uint8_t> destination(128);
    const DWORD mainThreadId = GetCurrentThreadId();
    UINT64 statusArrayValue = 0;

    {
        ComPtr<IDStorageFactory> factory = CreateFactory();
        ComPtr<IDStorageFile> file = OpenFile(factory.Get(), sourceFile.Path());
        ComPtr<IDStorageQueue1> queue = CreateQueue(factory.Get(), queueName, nullptr);

        ComPtr<IDStorageStatusArray> statusArray;
        ThrowFailure(factory->CreateStatusArray(8, "Multi Status", IID_PPV_ARGS(statusArray.GetAddressOf())));
        statusArrayValue = AsHandleValue(statusArray.Get());

        UniqueEvent completed;

        DSTORAGE_REQUEST request = MakeFileToMemoryRequest(file.Get(), 0, 128, destination.data());
        queue->EnqueueRequest(&request);
        INT64 previousUpper = 0;
        for (UINT32 index = 0; index < 3; ++index)
        {
            SpinUntilAfter(previousUpper);
            queue->EnqueueStatus(statusArray.Get(), index);
            previousUpper = NowInLibraryNanoseconds();
        }
        queue->EnqueueSetEvent(completed.Get());
        queue->Submit();

        ASSERT_EQ(WaitForSingleObject(completed.Get(), c_completionTimeoutMs), WAIT_OBJECT_0);
        for (UINT32 index = 0; index < 3; ++index)
        {
            ASSERT_HRESULT_SUCCEEDED(statusArray->GetHResult(index));
        }

        window.UpperNs = NowInLibraryNanoseconds();
    }

    Sleep(c_etwFlushSettleMs);
    session.End();
    session.RethrowConsumerException();

    auto queueRecord = std::ranges::find_if(recorder->Queues, [&](const auto& q) { return q.Name == queueName; });
    ASSERT_NE(queueRecord, recorder->Queues.end());

    auto readRecord = std::ranges::find_if(recorder->Reads, [&](const auto& r) { return r.QueueId == queueRecord->QueueId; });
    ASSERT_NE(readRecord, recorder->Reads.end());

    std::vector<const DirectStorageCallbacks::StatusNotification*> byIndex(3, nullptr);
    for (UINT64 index = 0; index < 3; ++index)
    {
        int count = 0;
        for (const auto& s : recorder->Statuses)
        {
            if (s.QueueId == queueRecord->QueueId && s.Index == index)
            {
                byIndex[static_cast<size_t>(index)] = &s;
                ++count;
            }
        }
        ASSERT_NE(byIndex[static_cast<size_t>(index)], nullptr) << "status at index " << index << " missing";
        EXPECT_EQ(count, 1) << "status at index " << index << " appeared more than once";
        EXPECT_EQ(byIndex[static_cast<size_t>(index)]->StatusArray, statusArrayValue) << "status-array identity mismatch";
        EXPECT_EQ(byIndex[static_cast<size_t>(index)]->EnqueueThreadId, mainThreadId);
        ExpectCorrelatedTimestamps(window, byIndex[static_cast<size_t>(index)]->EnqueueTimestamp, byIndex[static_cast<size_t>(index)]->CompletionTimestamp, "status");
        // Barrier: the status completes no earlier than the read it follows.
        EXPECT_GE(byIndex[static_cast<size_t>(index)]->CompletionTimestamp, readRecord->CompletionTimestamp);
    }

    // Enqueued in index order with disjoint windows -> strictly increasing enqueue times.
    EXPECT_LT(byIndex[0]->EnqueueTimestamp, byIndex[1]->EnqueueTimestamp);
    EXPECT_LT(byIndex[1]->EnqueueTimestamp, byIndex[2]->EnqueueTimestamp);
}

// EnqueueThreadId must reflect the thread that actually called Enqueue*/Submit, not the one
// that created the factory/queue nor the ETW consumer thread.
TEST(DirectStorageFunctional, EnqueueThreadIdReflectsTheEnqueuingThread)
{
    const char* queueName = "DxTimingCaptureLibrary Functional Cross Thread Queue";

    TempFile sourceFile(1024);

    auto callbacks = std::make_unique<RecordingDirectStorageCallbacks>();
    auto* recorder = callbacks.get();

    SimpleEtwSession session;
    session.SetDirectStorageCallbacks(std::move(callbacks));
    session.Begin();

    SessionWindow window;
    window.LowerNs = NowInLibraryNanoseconds();

    std::vector<uint8_t> destination(128);
    const DWORD mainThreadId = GetCurrentThreadId();
    DWORD workerThreadId = 0;

    {
        ComPtr<IDStorageFactory> factory = CreateFactory();
        ComPtr<IDStorageFile> file = OpenFile(factory.Get(), sourceFile.Path());
        ComPtr<IDStorageQueue1> queue = CreateQueue(factory.Get(), queueName, nullptr);

        ComPtr<IDStorageStatusArray> statusArray;
        ThrowFailure(factory->CreateStatusArray(DSTORAGE_MIN_QUEUE_CAPACITY, "Cross Thread Status", IID_PPV_ARGS(statusArray.GetAddressOf())));
        UniqueEvent completed;

        DSTORAGE_REQUEST request = MakeFileToMemoryRequest(file.Get(), 0, 128, destination.data());

        // Enqueue the read, a status and a set-event from a dedicated worker thread.
        workerThreadId = RunOnDedicatedThread([&](DWORD)
        {
            queue->EnqueueRequest(&request);
            queue->EnqueueStatus(statusArray.Get(), 0);
            queue->EnqueueSetEvent(completed.Get());
        });
        ASSERT_NE(workerThreadId, mainThreadId);

        // Submit from the main thread, so submit and enqueue carry different thread ids.
        queue->Submit();

        ASSERT_EQ(WaitForSingleObject(completed.Get(), c_completionTimeoutMs), WAIT_OBJECT_0);
        window.UpperNs = NowInLibraryNanoseconds();
    }

    Sleep(c_etwFlushSettleMs);
    session.End();
    session.RethrowConsumerException();

    auto queueRecord = std::ranges::find_if(recorder->Queues, [&](const auto& q) { return q.Name == queueName; });
    ASSERT_NE(queueRecord, recorder->Queues.end());

    auto readRecord = std::ranges::find_if(recorder->Reads, [&](const auto& r) { return r.QueueId == queueRecord->QueueId; });
    ASSERT_NE(readRecord, recorder->Reads.end());
    EXPECT_EQ(readRecord->EnqueueThreadId, workerThreadId) << "read must be attributed to the enqueuing thread";

    auto statusRecord = std::ranges::find_if(recorder->Statuses, [&](const auto& s) { return s.QueueId == queueRecord->QueueId; });
    ASSERT_NE(statusRecord, recorder->Statuses.end());
    EXPECT_EQ(statusRecord->EnqueueThreadId, workerThreadId);

    auto setEventRecord = std::ranges::find_if(recorder->SetEvents, [&](const auto& e) { return e.QueueId == queueRecord->QueueId; });
    ASSERT_NE(setEventRecord, recorder->SetEvents.end());
    EXPECT_EQ(setEventRecord->EnqueueThreadId, workerThreadId);

    auto submitRecord = std::ranges::find_if(recorder->Submits, [&](const auto& s) { return s.QueueId == queueRecord->QueueId; });
    ASSERT_NE(submitRecord, recorder->Submits.end());
    EXPECT_EQ(submitRecord->EnqueueThreadId, mainThreadId) << "submit must be attributed to the submitting thread";
}

// Two set-events on distinct handles must each correlate to the queue and carry the exact
// handle value the test used.
TEST(DirectStorageFunctional, SetEventHandlesAreCorrelated)
{
    const char* queueName = "DxTimingCaptureLibrary Functional Set Event Queue";

    TempFile sourceFile(1024);

    auto callbacks = std::make_unique<RecordingDirectStorageCallbacks>();
    auto* recorder = callbacks.get();

    SimpleEtwSession session;
    session.SetDirectStorageCallbacks(std::move(callbacks));
    session.Begin();

    SessionWindow window;
    window.LowerNs = NowInLibraryNanoseconds();

    std::vector<uint8_t> destination1(128);
    std::vector<uint8_t> destination2(128);
    UINT64 handle1Value = 0;
    UINT64 handle2Value = 0;

    {
        ComPtr<IDStorageFactory> factory = CreateFactory();
        ComPtr<IDStorageFile> file = OpenFile(factory.Get(), sourceFile.Path());
        ComPtr<IDStorageQueue1> queue = CreateQueue(factory.Get(), queueName, nullptr);

        UniqueEvent event1;
        UniqueEvent event2;
        handle1Value = AsHandleValue(event1.Get());
        handle2Value = AsHandleValue(event2.Get());

        DSTORAGE_REQUEST read1 = MakeFileToMemoryRequest(file.Get(), 0, 128, destination1.data());
        DSTORAGE_REQUEST read2 = MakeFileToMemoryRequest(file.Get(), 256, 128, destination2.data());
        queue->EnqueueRequest(&read1);
        queue->EnqueueSetEvent(event1.Get());
        queue->EnqueueRequest(&read2);
        queue->EnqueueSetEvent(event2.Get());
        queue->Submit();

        const HANDLE events[] = { event1.Get(), event2.Get() };
        ASSERT_EQ(WaitForMultipleObjects(2, events, TRUE, c_completionTimeoutMs), WAIT_OBJECT_0);
        window.UpperNs = NowInLibraryNanoseconds();
    }

    Sleep(c_etwFlushSettleMs);
    session.End();
    session.RethrowConsumerException();

    auto queueRecord = std::ranges::find_if(recorder->Queues, [&](const auto& q) { return q.Name == queueName; });
    ASSERT_NE(queueRecord, recorder->Queues.end());

    auto setEvent1 = std::ranges::find_if(recorder->SetEvents, [&](const auto& e) { return e.QueueId == queueRecord->QueueId && e.Handle == handle1Value; });
    auto setEvent2 = std::ranges::find_if(recorder->SetEvents, [&](const auto& e) { return e.QueueId == queueRecord->QueueId && e.Handle == handle2Value; });
    ASSERT_NE(setEvent1, recorder->SetEvents.end()) << "set-event for handle 1 missing";
    ASSERT_NE(setEvent2, recorder->SetEvents.end()) << "set-event for handle 2 missing";
    EXPECT_NE(handle1Value, handle2Value);
    ExpectCorrelatedTimestamps(window, setEvent1->EnqueueTimestamp, setEvent1->CompletionTimestamp, "set-event 1");
    ExpectCorrelatedTimestamps(window, setEvent2->EnqueueTimestamp, setEvent2->CompletionTimestamp, "set-event 2");
}

// Multiple fence signals on one fence must each correlate by value, carry the fence identity,
// and complete in value order. Needs a D3D12 device to create the ID3D12Fence.
TEST(DirectStorageFunctional, MultipleFenceSignalsCorrelateByValue)
{
    ComPtr<ID3D12Device> device = TryCreateDevice();
    if (device == nullptr)
    {
        GTEST_SKIP() << "No D3D12 device available to create a fence";
    }

    const char* queueName = "DxTimingCaptureLibrary Functional Multi Fence Queue";
    constexpr std::array<uint64_t, 3> fenceValues{ 1, 2, 3 };

    TempFile sourceFile(1024);

    auto callbacks = std::make_unique<RecordingDirectStorageCallbacks>();
    auto* recorder = callbacks.get();

    SimpleEtwSession session;
    session.SetDirectStorageCallbacks(std::move(callbacks));
    session.Begin();

    SessionWindow window;
    window.LowerNs = NowInLibraryNanoseconds();

    std::vector<uint8_t> destination(128);
    const DWORD mainThreadId = GetCurrentThreadId();
    UINT64 fenceValue = 0;

    {
        ComPtr<IDStorageFactory> factory = CreateFactory();
        ComPtr<IDStorageFile> file = OpenFile(factory.Get(), sourceFile.Path());
        ComPtr<IDStorageQueue1> queue = CreateQueue(factory.Get(), queueName, device.Get());

        ComPtr<ID3D12Fence> fence;
        ThrowFailure(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence.GetAddressOf())));
        fenceValue = AsHandleValue(fence.Get());

        UniqueEvent reachedHighest;
        ThrowFailure(fence->SetEventOnCompletion(fenceValues.back(), reachedHighest.Get()));

        DSTORAGE_REQUEST request = MakeFileToMemoryRequest(file.Get(), 0, 128, destination.data());
        queue->EnqueueRequest(&request);
        INT64 previousUpper = 0;
        for (uint64_t value : fenceValues)
        {
            SpinUntilAfter(previousUpper);
            queue->EnqueueSignal(fence.Get(), value);
            previousUpper = NowInLibraryNanoseconds();
        }
        queue->Submit();

        // Waiting on the highest value implies the lower ones already signalled.
        ASSERT_EQ(WaitForSingleObject(reachedHighest.Get(), c_completionTimeoutMs), WAIT_OBJECT_0);
        window.UpperNs = NowInLibraryNanoseconds();
    }

    Sleep(c_etwFlushSettleMs);
    session.End();
    session.RethrowConsumerException();

    auto queueRecord = std::ranges::find_if(recorder->Queues, [&](const auto& q) { return q.Name == queueName; });
    ASSERT_NE(queueRecord, recorder->Queues.end());

    std::vector<const DirectStorageCallbacks::FenceSignal*> byValue(fenceValues.size(), nullptr);
    for (size_t i = 0; i < fenceValues.size(); ++i)
    {
        int count = 0;
        for (const auto& f : recorder->Fences)
        {
            if (f.QueueId == queueRecord->QueueId && f.Value == fenceValues[i])
            {
                byValue[i] = &f;
                ++count;
            }
        }
        ASSERT_NE(byValue[i], nullptr) << "fence signal for value " << fenceValues[i] << " missing";
        EXPECT_EQ(count, 1) << "fence signal for value " << fenceValues[i] << " appeared more than once";
        EXPECT_EQ(byValue[i]->Fence, fenceValue) << "fence identity mismatch";
        EXPECT_EQ(byValue[i]->EnqueueThreadId, mainThreadId);
        ExpectCorrelatedTimestamps(window, byValue[i]->EnqueueTimestamp, byValue[i]->CompletionTimestamp, "fence");
    }

    // Enqueued in value order with disjoint windows -> strictly increasing enqueue times.
    for (size_t i = 1; i < fenceValues.size(); ++i)
    {
        EXPECT_LT(byValue[i - 1]->EnqueueTimestamp, byValue[i]->EnqueueTimestamp);
    }
}

// ---------------------------------------------------------------------------------------
// Broader functional surface
// ---------------------------------------------------------------------------------------

// A memory-source read has no IDStorageFile, so the enqueue event's file handle is null and
// the library synthesizes a "file# <id>" placeholder. Exercises the memory source and
// synthesized-name paths, and verifies the copied bytes.
TEST(DirectStorageFunctional, MemorySourceReadCorrelatesWithSynthesizedFileName)
{
    const char* queueName = "DxTimingCaptureLibrary Functional Memory Source Queue";
    constexpr uint32_t size = 512;

    std::vector<uint8_t> source(size);
    for (uint32_t i = 0; i < size; ++i)
    {
        source[i] = static_cast<uint8_t>((i * 197u + 3u) & 0xFF);
    }
    std::vector<uint8_t> destination(size);

    auto callbacks = std::make_unique<RecordingDirectStorageCallbacks>();
    auto* recorder = callbacks.get();

    SimpleEtwSession session;
    session.SetDirectStorageCallbacks(std::move(callbacks));
    session.Begin();

    SessionWindow window;
    window.LowerNs = NowInLibraryNanoseconds();

    {
        ComPtr<IDStorageFactory> factory = CreateFactory();
        ComPtr<IDStorageQueue1> queue = CreateQueue(factory.Get(), queueName, nullptr, DSTORAGE_PRIORITY_NORMAL, DSTORAGE_REQUEST_SOURCE_MEMORY);
        UniqueEvent completed;

        DSTORAGE_REQUEST request = MakeMemoryToMemoryRequest(source.data(), size, destination.data());
        queue->EnqueueRequest(&request);
        queue->EnqueueSetEvent(completed.Get());
        queue->Submit();

        ASSERT_EQ(WaitForSingleObject(completed.Get(), c_completionTimeoutMs), WAIT_OBJECT_0);
        EXPECT_EQ(destination, source) << "memory-to-memory copy returned unexpected data";
        window.UpperNs = NowInLibraryNanoseconds();
    }

    Sleep(c_etwFlushSettleMs);
    session.End();
    session.RethrowConsumerException();

    auto queueRecord = std::ranges::find_if(recorder->Queues, [&](const auto& q) { return q.Name == queueName; });
    ASSERT_NE(queueRecord, recorder->Queues.end());

    auto readRecord = std::ranges::find_if(recorder->Reads, [&](const auto& r) { return r.QueueId == queueRecord->QueueId && r.Size == size; });
    ASSERT_NE(readRecord, recorder->Reads.end());
    EXPECT_EQ(readRecord->Offset, 0u);
    EXPECT_EQ(readRecord->CompressionType, DSTORAGE_COMPRESSION_FORMAT_NONE);
    ExpectCorrelatedTimestamps(window, readRecord->EnqueueTimestamp, readRecord->CompletionTimestamp, "memory read");

    // The read's file id resolves to a synthesized "file# <id>" placeholder.
    auto fileRecord = std::ranges::find_if(recorder->Files, [&](const auto& f) { return f.FileId == readRecord->FileId; });
    ASSERT_NE(fileRecord, recorder->Files.end());
    EXPECT_EQ(fileRecord->Path, L"file# " + std::to_wstring(fileRecord->FileId))
        << "memory-source read should resolve to a synthesized file name";
}

// With object-name recording off, the CreateQueue event carries no name, so the library
// reports an empty queue name. (OpenFile still carries the real path regardless; synthesized
// file names are covered by MemorySourceReadCorrelatesWithSynthesizedFileName.)
TEST(DirectStorageFunctional, NamesDisabledYieldsEmptyQueueName)
{
    constexpr uint64_t readOffset = 128;
    constexpr uint32_t readSize = 256;

    TempFile sourceFile(1024);

    auto callbacks = std::make_unique<RecordingDirectStorageCallbacks>();
    auto* recorder = callbacks.get();

    SimpleEtwSession session;
    session.SetDirectStorageCallbacks(std::move(callbacks));
    session.Begin();

    SessionWindow window;
    window.LowerNs = NowInLibraryNanoseconds();

    std::vector<uint8_t> destination(readSize);

    {
        // Names OFF: the factory does not record object names.
        ComPtr<IDStorageFactory> factory = CreateFactory(/*recordObjectNames*/ false);
        ComPtr<IDStorageFile> file = OpenFile(factory.Get(), sourceFile.Path());
        // A non-empty name is requested, but with recording off it should not reach ETW.
        ComPtr<IDStorageQueue1> queue = CreateQueue(factory.Get(), "This Name Should Not Appear", nullptr);
        UniqueEvent completed;

        DSTORAGE_REQUEST request = MakeFileToMemoryRequest(file.Get(), readOffset, readSize, destination.data());
        queue->EnqueueRequest(&request);
        queue->EnqueueSetEvent(completed.Get());
        queue->Submit();

        ASSERT_EQ(WaitForSingleObject(completed.Get(), c_completionTimeoutMs), WAIT_OBJECT_0);
        EXPECT_TRUE(std::equal(destination.begin(), destination.end(), sourceFile.Contents.begin() + readOffset));
        window.UpperNs = NowInLibraryNanoseconds();
    }

    Sleep(c_etwFlushSettleMs);
    session.End();
    session.RethrowConsumerException();

    // Find our read by its distinctive offset/size, then reach its file and queue by id.
    auto readRecord = std::ranges::find_if(recorder->Reads, [&](const auto& r) { return r.Offset == readOffset && r.Size == readSize; });
    ASSERT_NE(readRecord, recorder->Reads.end());

    auto queueRecord = std::ranges::find_if(recorder->Queues, [&](const auto& q) { return q.QueueId == readRecord->QueueId; });
    ASSERT_NE(queueRecord, recorder->Queues.end());
    EXPECT_TRUE(queueRecord->Name.empty()) << "queue name should be empty when name recording is disabled";
    EXPECT_TRUE(WithinWindow(window, queueRecord->Timestamp, "queue"));

    auto fileRecord = std::ranges::find_if(recorder->Files, [&](const auto& f) { return f.FileId == readRecord->FileId; });
    ASSERT_NE(fileRecord, recorder->Files.end());
    // OpenFile always carries the real path regardless of name recording, so it is not
    // synthesized here; name recording only governs the queue name, asserted empty above.
    EXPECT_FALSE(fileRecord->Path.empty()) << "a real file path should still be recorded";
    EXPECT_TRUE(WithinWindow(window, fileRecord->Timestamp, "file"));
}

// A GDeflate-compressed read must report CompressionType == GDEFLATE and a Size equal to the
// compressed byte count, while still delivering the correct decompressed bytes. Uses
// DirectStorage's own codec; the CPU decompression path needs no device.
TEST(DirectStorageFunctional, GDeflateCompressedReadReportsCompressionType)
{
    const char* queueName = "DxTimingCaptureLibrary Functional GDeflate Queue";
    constexpr uint32_t uncompressedSize = 64 * 1024;

    // A compressible, deterministic buffer.
    std::vector<uint8_t> original(uncompressedSize);
    for (uint32_t i = 0; i < uncompressedSize; ++i)
    {
        original[i] = static_cast<uint8_t>((i / 64) & 0xFF);
    }

    ComPtr<IDStorageCompressionCodec> codec;
    if (FAILED(DStorageCreateCompressionCodec(DSTORAGE_COMPRESSION_FORMAT_GDEFLATE, 0, IID_PPV_ARGS(codec.GetAddressOf()))))
    {
        GTEST_SKIP() << "GDeflate compression codec unavailable";
    }

    std::vector<uint8_t> compressed(codec->CompressBufferBound(uncompressedSize));
    size_t compressedSize = 0;
    ThrowFailure(codec->CompressBuffer(
        original.data(), uncompressedSize, DSTORAGE_COMPRESSION_DEFAULT,
        compressed.data(), compressed.size(), &compressedSize));
    ASSERT_GT(compressedSize, 0u);
    ASSERT_LT(compressedSize, static_cast<size_t>(uncompressedSize));

    TempFile compressedFile(0);
    compressedFile.WriteAllBytes(std::span<const uint8_t>(compressed.data(), compressedSize));

    std::vector<uint8_t> destination(uncompressedSize);

    auto callbacks = std::make_unique<RecordingDirectStorageCallbacks>();
    auto* recorder = callbacks.get();

    SimpleEtwSession session;
    session.SetDirectStorageCallbacks(std::move(callbacks));
    session.Begin();

    SessionWindow window;
    window.LowerNs = NowInLibraryNanoseconds();

    {
        ComPtr<IDStorageFactory> factory = CreateFactory();
        ComPtr<IDStorageFile> file = OpenFile(factory.Get(), compressedFile.Path());
        ComPtr<IDStorageQueue1> queue = CreateQueue(factory.Get(), queueName, nullptr);
        UniqueEvent completed;

        DSTORAGE_REQUEST request{};
        request.Options.CompressionFormat = DSTORAGE_COMPRESSION_FORMAT_GDEFLATE;
        request.Options.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
        request.Options.DestinationType = DSTORAGE_REQUEST_DESTINATION_MEMORY;
        request.Source.File.Source = file.Get();
        request.Source.File.Offset = 0;
        request.Source.File.Size = static_cast<uint32_t>(compressedSize);
        request.Destination.Memory.Buffer = destination.data();
        request.Destination.Memory.Size = uncompressedSize;
        request.UncompressedSize = uncompressedSize;

        queue->EnqueueRequest(&request);
        queue->EnqueueSetEvent(completed.Get());
        queue->Submit();

        ASSERT_EQ(WaitForSingleObject(completed.Get(), c_completionTimeoutMs), WAIT_OBJECT_0);
        EXPECT_EQ(destination, original) << "decompressed data did not match the original";
        window.UpperNs = NowInLibraryNanoseconds();
    }

    Sleep(c_etwFlushSettleMs);
    session.End();
    session.RethrowConsumerException();

    auto queueRecord = std::ranges::find_if(recorder->Queues, [&](const auto& q) { return q.Name == queueName; });
    ASSERT_NE(queueRecord, recorder->Queues.end());

    auto readRecord = std::ranges::find_if(recorder->Reads, [&](const auto& r)
    {
        return r.QueueId == queueRecord->QueueId && r.CompressionType == DSTORAGE_COMPRESSION_FORMAT_GDEFLATE;
    });
    ASSERT_NE(readRecord, recorder->Reads.end()) << "no GDeflate-compressed read was decoded";
    EXPECT_EQ(readRecord->Size, static_cast<uint32_t>(compressedSize)) << "Size must be the compressed (source) size";
    EXPECT_EQ(readRecord->Offset, 0u);
    EXPECT_EQ(readRecord->ShuffleType, 0);
    ExpectCorrelatedTimestamps(window, readRecord->EnqueueTimestamp, readRecord->CompletionTimestamp, "gdeflate read");
}

// When a queue fills to capacity without an explicit Submit, DirectStorage auto-submits, and
// the decoded Submit must carry AutoSubmitted == true. The threshold is read from the runtime
// rather than assumed; a trailing set-event is submitted only as a drain handle to wait on.
TEST(DirectStorageFunctional, AutoSubmittedFlagIsTrueWhenQueueFillsToCapacity)
{
    const char* queueName = "DxTimingCaptureLibrary Functional Auto Submit Queue";
    constexpr uint32_t size = 64;

    std::vector<uint8_t> source(size);
    for (uint32_t i = 0; i < size; ++i)
    {
        source[i] = static_cast<uint8_t>(i);
    }

    auto callbacks = std::make_unique<RecordingDirectStorageCallbacks>();
    auto* recorder = callbacks.get();

    SimpleEtwSession session;
    session.SetDirectStorageCallbacks(std::move(callbacks));
    session.Begin();

    SessionWindow window;
    window.LowerNs = NowInLibraryNanoseconds();

    // Destinations must outlive the wait; reserve enough for the whole fill.
    std::vector<std::vector<uint8_t>> destinations;

    {
        ComPtr<IDStorageFactory> factory = CreateFactory();
        ComPtr<IDStorageQueue1> queue = CreateQueue(
            factory.Get(), queueName, nullptr, DSTORAGE_PRIORITY_NORMAL,
            DSTORAGE_REQUEST_SOURCE_MEMORY, DSTORAGE_MIN_QUEUE_CAPACITY);
        UniqueEvent completed;

        DSTORAGE_QUEUE_INFO info{};
        queue->Query(&info);
        ASSERT_GT(info.RequestCountUntilAutoSubmit, 1) << "queue reports it would auto-submit immediately";

        std::vector<DSTORAGE_REQUEST> requests;
        requests.reserve(static_cast<size_t>(DSTORAGE_MIN_QUEUE_CAPACITY) + 2);

        // Fill to one entry below the auto-submit threshold without ever calling Submit.
        // Moving the outer vector preserves the inner buffers' heap pointers, and
        // DSTORAGE_REQUEST is copied at enqueue time, so reallocation is safe.
        for (int guard = 0; guard < 4096; ++guard)
        {
            queue->Query(&info);
            if (info.RequestCountUntilAutoSubmit <= 1)
            {
                break;
            }
            destinations.emplace_back(size);
            requests.push_back(MakeMemoryToMemoryRequest(source.data(), size, destinations.back().data()));
            queue->EnqueueRequest(&requests.back());
        }
        ASSERT_LE(info.RequestCountUntilAutoSubmit, 1) << "queue never approached its auto-submit threshold";

        // One more read reaches the threshold and triggers the auto-submit (a set-event does
        // not count toward the threshold, so it can't be the trigger).
        destinations.emplace_back(size);
        requests.push_back(MakeMemoryToMemoryRequest(source.data(), size, destinations.back().data()));
        queue->EnqueueRequest(&requests.back());

        // Reads have now auto-submitted. Submit a set-event in a fresh batch as a wait handle;
        // same-queue ordering means it drains after the auto-submitted reads.
        queue->EnqueueSetEvent(completed.Get());
        queue->Submit();

        ASSERT_EQ(WaitForSingleObject(completed.Get(), c_completionTimeoutMs), WAIT_OBJECT_0)
            << "auto-submit + drain did not complete";
        window.UpperNs = NowInLibraryNanoseconds();
    }

    Sleep(c_etwFlushSettleMs);
    session.End();
    session.RethrowConsumerException();

    auto queueRecord = std::ranges::find_if(recorder->Queues, [&](const auto& q) { return q.Name == queueName; });
    ASSERT_NE(queueRecord, recorder->Queues.end());

    // We never manually submitted the reads, so the batch carrying them must be an auto-submit.
    // (The trailing set-event is a separate manual submit — expected, not asserted against.)
    bool sawAutoSubmit = false;
    for (const auto& s : recorder->Submits)
    {
        if (s.QueueId != queueRecord->QueueId)
        {
            continue;
        }
        if (s.AutoSubmitted)
        {
            sawAutoSubmit = true;
            EXPECT_TRUE(WithinWindow(window, s.Timestamp, "auto submit"));
        }
    }
    EXPECT_TRUE(sawAutoSubmit) << "expected an auto-submitted Submit for the filled queue";
}
