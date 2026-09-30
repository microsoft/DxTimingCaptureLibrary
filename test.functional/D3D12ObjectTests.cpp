// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "pch.h"

#include <algorithm>
#include <vector>

#include <d3dcompiler.h>

#include <DxTimingCaptureLibrary/DxTimingCaptureEventHandler.h>
#include <DxTimingCaptureLibrary/TicksToNanoseconds.h>

using namespace DirectX::Etw;

// Internal library headers: the heap-classification tests at the bottom of this file drive a
// D3D12ObjectProcessor directly rather than through a live D3D12 device. These headers are part
// of the library's private implementation and expect the DirectX::Etw namespace to be in scope.
#include <lib/D3D12ObjectProcessor.h>
#include <lib/AllocationTracker.h>

#include "EtwSession.h"
#include "RecordingApiObjectCallbacks.h"

// ---------------------------------------------------------------------------
// Shared helpers
// ---------------------------------------------------------------------------

namespace
{
    // Converts the current QPC reading to nanoseconds on the same timebase the library
    // uses for callback timestamps (QPC ticks -> ns, see QpcTimestampConverter).
    INT64 GetCurrentTimestampNs()
    {
        LARGE_INTEGER frequency;
        QueryPerformanceFrequency(&frequency);

        LARGE_INTEGER counter;
        QueryPerformanceCounter(&counter);

        return static_cast<INT64>(TicksToNanoseconds(counter.QuadPart, frequency.QuadPart));
    }

    // A [lower, upper] nanosecond window captured around a D3D12 call. The ETW event the
    // call emits is timestamped inside the window, so the timestamp the library reports
    // for that object must fall within it.
    class TimestampWindow
    {
        INT64 m_lowerNs = 0;
        INT64 m_upperNs = 0;

    public:
        void Open() { m_lowerNs = GetCurrentTimestampNs(); }
        void Close() { m_upperNs = GetCurrentTimestampNs(); }

        void ExpectContains(INT64 timestampNs) const
        {
            EXPECT_GE(timestampNs, m_lowerNs);
            EXPECT_LE(timestampNs, m_upperNs);
        }
    };

    void ExpectCurrentProcessAndThread(UINT32 processId, UINT32 threadId)
    {
        EXPECT_EQ(GetCurrentProcessId(), processId);
        EXPECT_EQ(GetCurrentThreadId(), threadId);
    }

    template <typename Record, typename Predicate>
    const Record* FindRecord(const std::vector<Record>& records, Predicate predicate)
    {
        auto it = std::ranges::find_if(records, predicate);
        return it == records.end() ? nullptr : &*it;
    }

    ComPtr<ID3D12Device> CreateDevice()
    {
        ComPtr<ID3D12Device> device;
        ThrowFailure(D3D12CreateDevice(
            nullptr,
            D3D_FEATURE_LEVEL_11_0,
            IID_PPV_ARGS(&device)));
        return device;
    }

    // A simple buffer resource description. Buffers place no constraints on Width, so
    // tests use a distinctive Width to tell their resource apart from any the runtime
    // or driver creates internally.
    D3D12_RESOURCE_DESC MakeBufferDesc(UINT64 width)
    {
        D3D12_RESOURCE_DESC resourceDesc{};
        resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        resourceDesc.Alignment = 0;
        resourceDesc.Width = width;
        resourceDesc.Height = 1;
        resourceDesc.DepthOrArraySize = 1;
        resourceDesc.MipLevels = 1;
        resourceDesc.Format = DXGI_FORMAT_UNKNOWN;
        resourceDesc.SampleDesc.Count = 1;
        resourceDesc.SampleDesc.Quality = 0;
        resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        resourceDesc.Flags = D3D12_RESOURCE_FLAG_NONE;
        return resourceDesc;
    }

    void ExpectBufferDescMatches(const D3D12_RESOURCE_DESC& actual, const D3D12_RESOURCE_DESC& expected)
    {
        EXPECT_EQ(actual.Dimension, expected.Dimension);
        EXPECT_EQ(actual.Width, expected.Width);
        EXPECT_EQ(actual.Height, expected.Height);
        EXPECT_EQ(actual.DepthOrArraySize, expected.DepthOrArraySize);
        EXPECT_EQ(actual.MipLevels, expected.MipLevels);
        EXPECT_EQ(actual.Format, expected.Format);
        EXPECT_EQ(actual.Layout, expected.Layout);
        EXPECT_EQ(actual.Flags, expected.Flags);
    }
}

// ---------------------------------------------------------------------------
// Device
// ---------------------------------------------------------------------------

TEST(D3D12ObjectTests, Device)
{
    auto callbacks = std::make_unique<RecordingApiObjectCallbacks>();
    auto* recorder = callbacks.get();

    SimpleEtwSession session;
    session.SetApiObjectCallbacks(std::move(callbacks));
    session.Begin();

    ComPtr<ID3D12Device> device = CreateDevice();

    session.End();
    session.RethrowConsumerException();

    ASSERT_FALSE(recorder->Devices.empty());

    // The version numbers reported via ETW are DDI versions we can't meaningfully
    // validate here, but the reported feature level should be one the device agrees it
    // supports.
    const D3D_FEATURE_LEVEL reportedFeatureLevel = recorder->Devices.front().Info.FeatureLevel;
    EXPECT_NE(reportedFeatureLevel, 0);

    D3D_FEATURE_LEVEL requested = reportedFeatureLevel;
    D3D12_FEATURE_DATA_FEATURE_LEVELS featureLevelData{};
    featureLevelData.NumFeatureLevels = 1;
    featureLevelData.pFeatureLevelsRequested = &requested;

    ThrowFailure(device->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, &featureLevelData, static_cast<UINT>(sizeof(featureLevelData))));
    EXPECT_EQ(reportedFeatureLevel, featureLevelData.MaxSupportedFeatureLevel);

    EXPECT_NE(recorder->Devices.front().ObjectId, 0u);
}

// ---------------------------------------------------------------------------
// Descriptor heap
// ---------------------------------------------------------------------------

TEST(D3D12ObjectTests, DescriptorHeap)
{
    auto callbacks = std::make_unique<RecordingApiObjectCallbacks>();
    auto* recorder = callbacks.get();

    SimpleEtwSession session;
    session.SetApiObjectCallbacks(std::move(callbacks));
    session.Begin();

    ComPtr<ID3D12Device> device = CreateDevice();

    D3D12_DESCRIPTOR_HEAP_DESC expected{};
    expected.NumDescriptors = 1337u;
    expected.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    expected.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    expected.NodeMask = 0;

    TimestampWindow window;
    ComPtr<ID3D12DescriptorHeap> heap;
    window.Open();
    ThrowFailure(device->CreateDescriptorHeap(&expected, IID_PPV_ARGS(&heap)));
    window.Close();

    session.End();
    session.RethrowConsumerException();

    const auto* record = FindRecord(recorder->DescriptorHeaps, [&](const auto& r)
    {
        return r.Desc.NumDescriptors == expected.NumDescriptors && r.Desc.Type == expected.Type;
    });
    ASSERT_NE(record, nullptr) << "Expected a descriptor-heap callback for the created heap";

    EXPECT_EQ(record->Desc.NumDescriptors, expected.NumDescriptors);
    EXPECT_EQ(record->Desc.Type, expected.Type);
    EXPECT_EQ(record->Desc.Flags, expected.Flags);
    // D3D12 normalizes a NodeMask of 0 to the single-node mask 1, and the runtime
    // reports that normalized value through ETW, so the library hands back 1 here.
    EXPECT_EQ(record->Desc.NodeMask, 1u);

    ExpectCurrentProcessAndThread(record->ProcessId, record->ThreadId);
    window.ExpectContains(record->Timestamp);
    EXPECT_NE(record->ObjectId, 0u);
}

// ---------------------------------------------------------------------------
// Command allocator (type round-trips for each queue type)
// ---------------------------------------------------------------------------

namespace
{
    void RunCommandAllocatorTest(D3D12_COMMAND_LIST_TYPE commandListType)
    {
        auto callbacks = std::make_unique<RecordingApiObjectCallbacks>();
        auto* recorder = callbacks.get();

        SimpleEtwSession session;
        session.SetApiObjectCallbacks(std::move(callbacks));
        session.Begin();

        ComPtr<ID3D12Device> device = CreateDevice();

        TimestampWindow window;
        ComPtr<ID3D12CommandAllocator> allocator;
        window.Open();
        ThrowFailure(device->CreateCommandAllocator(commandListType, IID_PPV_ARGS(&allocator)));
        window.Close();

        session.End();
        session.RethrowConsumerException();

        const auto* record = FindRecord(recorder->CommandAllocators, [&](const auto& r)
        {
            return r.CommandListType == commandListType;
        });
        ASSERT_NE(record, nullptr) << "Expected a command-allocator callback with the requested type";

        EXPECT_EQ(record->CommandListType, commandListType);
        ExpectCurrentProcessAndThread(record->ProcessId, record->ThreadId);
        window.ExpectContains(record->Timestamp);
        EXPECT_NE(record->ObjectId, 0u);
    }
}

TEST(D3D12ObjectTests, CommandAllocator_Copy)
{
    RunCommandAllocatorTest(D3D12_COMMAND_LIST_TYPE_COPY);
}

TEST(D3D12ObjectTests, CommandAllocator_Direct)
{
    RunCommandAllocatorTest(D3D12_COMMAND_LIST_TYPE_DIRECT);
}

TEST(D3D12ObjectTests, CommandAllocator_Compute)
{
    RunCommandAllocatorTest(D3D12_COMMAND_LIST_TYPE_COMPUTE);
}

// ---------------------------------------------------------------------------
// Committed resource
// ---------------------------------------------------------------------------

TEST(D3D12ObjectTests, Resource_Committed)
{
    constexpr UINT64 DistinctiveWidth = 0x1A2B3C; // distinctive so we don't match an internal buffer

    auto callbacks = std::make_unique<RecordingApiObjectCallbacks>();
    auto* recorder = callbacks.get();

    SimpleEtwSession session;
    session.SetApiObjectCallbacks(std::move(callbacks));
    session.Begin();

    ComPtr<ID3D12Device> device = CreateDevice();

    D3D12_HEAP_PROPERTIES heapProperties{};
    heapProperties.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC resourceDesc = MakeBufferDesc(DistinctiveWidth);

    TimestampWindow window;
    ComPtr<ID3D12Resource> resource;
    window.Open();
    ThrowFailure(device->CreateCommittedResource(
        &heapProperties,
        D3D12_HEAP_FLAG_NONE,
        &resourceDesc,
        D3D12_RESOURCE_STATE_COMMON,
        nullptr,
        IID_PPV_ARGS(&resource)));
    window.Close();

    session.End();
    session.RethrowConsumerException();

    const auto* record = FindRecord(recorder->CommittedResources, [&](const auto& r)
    {
        return r.Desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER && r.Desc.Width == DistinctiveWidth;
    });
    ASSERT_NE(record, nullptr) << "Expected a committed-resource callback for the created buffer";

    ExpectBufferDescMatches(record->Desc, resourceDesc);

    // A committed resource owns an implicit heap, so the heap properties should round-trip.
    EXPECT_EQ(record->HeapProperties.Type, D3D12_HEAP_TYPE_DEFAULT);

    // It is backed by real memory, so it should carry a usable GPU virtual address range.
    EXPECT_NE(record->Placement.GpuVirtualAddress, 0u);
    EXPECT_NE(record->Placement.GpuVirtualAddress, UINT64_MAX);
    EXPECT_GE(record->Placement.GpuVirtualSize, DistinctiveWidth);

    ExpectCurrentProcessAndThread(record->ProcessId, record->ThreadId);
    window.ExpectContains(record->Timestamp);
    EXPECT_NE(record->ObjectId, 0u);
}

TEST(D3D12ObjectTests, MultipleResources)
{
    // Eight buffers with consecutive distinctive widths. All eight must be reported —
    // this is the collect-all guarantee (no resource silently dropped).
    constexpr UINT64 BaseWidth = 0x2C0000;
    constexpr UINT32 ResourceCount = 8;

    auto callbacks = std::make_unique<RecordingApiObjectCallbacks>();
    auto* recorder = callbacks.get();

    SimpleEtwSession session;
    session.SetApiObjectCallbacks(std::move(callbacks));
    session.Begin();

    ComPtr<ID3D12Device> device = CreateDevice();

    D3D12_HEAP_PROPERTIES heapProperties{};
    heapProperties.Type = D3D12_HEAP_TYPE_DEFAULT;

    std::vector<ComPtr<ID3D12Resource>> resources;
    for (UINT32 i = 0; i < ResourceCount; ++i)
    {
        D3D12_RESOURCE_DESC resourceDesc = MakeBufferDesc(BaseWidth + i);
        ComPtr<ID3D12Resource> resource;
        ThrowFailure(device->CreateCommittedResource(
            &heapProperties,
            D3D12_HEAP_FLAG_NONE,
            &resourceDesc,
            D3D12_RESOURCE_STATE_COMMON,
            nullptr,
            IID_PPV_ARGS(&resource)));
        resources.push_back(std::move(resource));
    }

    session.End();
    session.RethrowConsumerException();

    for (UINT32 i = 0; i < ResourceCount; ++i)
    {
        const UINT64 expectedWidth = BaseWidth + i;
        const auto* record = FindRecord(recorder->CommittedResources, [&](const auto& r)
        {
            return r.Desc.Width == expectedWidth;
        });
        EXPECT_NE(record, nullptr) << "Missing committed-resource callback for width " << expectedWidth;
    }
}

// ---------------------------------------------------------------------------
// Heap
// ---------------------------------------------------------------------------

TEST(D3D12ObjectTests, Heap)
{
    constexpr UINT64 DistinctiveSize = 2ull * 1024 * 1024; // 2 MB, distinctive vs typical internal heaps

    auto callbacks = std::make_unique<RecordingApiObjectCallbacks>();
    auto* recorder = callbacks.get();

    SimpleEtwSession session;
    session.SetApiObjectCallbacks(std::move(callbacks));
    session.Begin();

    ComPtr<ID3D12Device> device = CreateDevice();

    D3D12_HEAP_DESC heapDesc{};
    heapDesc.SizeInBytes = DistinctiveSize;
    heapDesc.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    heapDesc.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    heapDesc.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;

    TimestampWindow window;
    ComPtr<ID3D12Heap> heap;
    window.Open();
    ThrowFailure(device->CreateHeap(&heapDesc, IID_PPV_ARGS(&heap)));
    window.Close();

    session.End();
    session.RethrowConsumerException();

    const auto* record = FindRecord(recorder->Heaps, [&](const auto& r)
    {
        return r.Desc.SizeInBytes == DistinctiveSize;
    });
    ASSERT_NE(record, nullptr) << "Expected a heap callback for the created heap";

    EXPECT_EQ(record->Desc.SizeInBytes, heapDesc.SizeInBytes);
    EXPECT_EQ(record->Desc.Properties.Type, D3D12_HEAP_TYPE_DEFAULT);
    EXPECT_EQ(record->Desc.Flags, heapDesc.Flags);

    ExpectCurrentProcessAndThread(record->ProcessId, record->ThreadId);
    window.ExpectContains(record->Timestamp);
    EXPECT_NE(record->ObjectId, 0u);
}

// ---------------------------------------------------------------------------
// Placed resource (exercises the heap <-> resource correlation)
// ---------------------------------------------------------------------------

TEST(D3D12ObjectTests, Resource_Placed)
{
    constexpr UINT64 HeapSize = 4ull * 1024 * 1024; // 4 MB, distinctive
    constexpr UINT64 DistinctiveWidth = 0x3D4E5F;

    auto callbacks = std::make_unique<RecordingApiObjectCallbacks>();
    auto* recorder = callbacks.get();

    SimpleEtwSession session;
    session.SetApiObjectCallbacks(std::move(callbacks));
    session.Begin();

    ComPtr<ID3D12Device> device = CreateDevice();

    D3D12_HEAP_DESC heapDesc{};
    heapDesc.SizeInBytes = HeapSize;
    heapDesc.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    heapDesc.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    heapDesc.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;

    ComPtr<ID3D12Heap> heap;
    ThrowFailure(device->CreateHeap(&heapDesc, IID_PPV_ARGS(&heap)));

    D3D12_RESOURCE_DESC resourceDesc = MakeBufferDesc(DistinctiveWidth);

    TimestampWindow window;
    ComPtr<ID3D12Resource> resource;
    window.Open();
    ThrowFailure(device->CreatePlacedResource(
        heap.Get(),
        0,
        &resourceDesc,
        D3D12_RESOURCE_STATE_COMMON,
        nullptr,
        IID_PPV_ARGS(&resource)));
    window.Close();

    session.End();
    session.RethrowConsumerException();

    // Validate the heap surfaced.
    const auto* heapRecord = FindRecord(recorder->Heaps, [&](const auto& r)
    {
        return r.Desc.SizeInBytes == HeapSize;
    });
    ASSERT_NE(heapRecord, nullptr) << "Expected a heap callback for the placed-resource heap";

    // Validate the placed resource surfaced and is correlated to the heap's address range.
    const auto* record = FindRecord(recorder->PlacedResources, [&](const auto& r)
    {
        return r.Desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER && r.Desc.Width == DistinctiveWidth;
    });
    ASSERT_NE(record, nullptr) << "Expected a placed-resource callback for the created buffer";

    ExpectBufferDescMatches(record->Desc, resourceDesc);

    // The resource was placed at offset 0, so its address should be the heap's base and
    // lie within the heap's reported range.
    EXPECT_NE(record->Placement.GpuVirtualAddress, UINT64_MAX);
    EXPECT_GE(record->Placement.GpuVirtualAddress, heapRecord->Placement.GpuVirtualAddress);
    EXPECT_LT(record->Placement.GpuVirtualAddress, heapRecord->Placement.GpuVirtualAddress + HeapSize);

    ExpectCurrentProcessAndThread(record->ProcessId, record->ThreadId);
    window.ExpectContains(record->Timestamp);
    EXPECT_NE(record->ObjectId, 0u);
}

// ---------------------------------------------------------------------------
// Reserved (tiled) resource — feature-gated
// ---------------------------------------------------------------------------

TEST(D3D12ObjectTests, Resource_Reserved)
{
    auto callbacks = std::make_unique<RecordingApiObjectCallbacks>();
    auto* recorder = callbacks.get();

    ComPtr<ID3D12Device> probe = CreateDevice();
    D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
    ThrowFailure(probe->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options)));
    if (options.TiledResourcesTier == D3D12_TILED_RESOURCES_TIER_NOT_SUPPORTED)
    {
        GTEST_SKIP() << "Adapter does not support tiled (reserved) resources";
    }
    probe.Reset();

    SimpleEtwSession session;
    session.SetApiObjectCallbacks(std::move(callbacks));
    session.Begin();

    ComPtr<ID3D12Device> device = CreateDevice();

    // A reserved buffer, which has tiles but no backing memory at creation. A buffer is
    // the most broadly supported reserved-resource shape; its size must be a multiple of
    // the 64 KB tile size. 4 MB is exactly 64 tiles.
    constexpr UINT64 TileSize = 64ull * 1024;
    constexpr UINT64 ExpectedTileCount = 64;
    constexpr UINT64 ReservedBufferWidth = TileSize * ExpectedTileCount;

    D3D12_RESOURCE_DESC resourceDesc = MakeBufferDesc(ReservedBufferWidth);

    TimestampWindow window;
    ComPtr<ID3D12Resource> resource;
    window.Open();
    ThrowFailure(device->CreateReservedResource(
        &resourceDesc,
        D3D12_RESOURCE_STATE_COMMON,
        nullptr,
        IID_PPV_ARGS(&resource)));
    window.Close();

    session.End();
    session.RethrowConsumerException();

    const auto* record = FindRecord(recorder->ReservedResources, [&](const auto& r)
    {
        return r.Desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER
            && r.Desc.Width == ReservedBufferWidth;
    });
    ASSERT_NE(record, nullptr) << "Expected a reserved-resource callback for the created tiled buffer";

    EXPECT_EQ(record->Reserved.NumTilesForResource, ExpectedTileCount);

    ExpectCurrentProcessAndThread(record->ProcessId, record->ThreadId);
    window.ExpectContains(record->Timestamp);
    EXPECT_NE(record->ObjectId, 0u);
}

// ---------------------------------------------------------------------------
// Pipeline state
// ---------------------------------------------------------------------------

TEST(D3D12ObjectTests, PipelineState_Compute)
{
    auto callbacks = std::make_unique<RecordingApiObjectCallbacks>();
    auto* recorder = callbacks.get();

    // Minimal compute shader and an empty root signature — enough to create a PSO
    // without any resource bindings.
    static const char computeShaderSource[] = "[numthreads(1,1,1)] void main() {}";

    ComPtr<ID3DBlob> compiledShader;
    ComPtr<ID3DBlob> compileErrors;
    HRESULT compileResult = D3DCompile(
        computeShaderSource,
        sizeof(computeShaderSource) - 1,
        nullptr,
        nullptr,
        nullptr,
        "main",
        "cs_5_0",
        0,
        0,
        &compiledShader,
        &compileErrors);
    ASSERT_TRUE(SUCCEEDED(compileResult)) << "Failed to compile the test compute shader";

    SimpleEtwSession session;
    session.SetApiObjectCallbacks(std::move(callbacks));
    session.Begin();

    ComPtr<ID3D12Device> device = CreateDevice();

    D3D12_ROOT_SIGNATURE_DESC rootSignatureDesc{};
    rootSignatureDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    ComPtr<ID3DBlob> serializedRootSignature;
    ThrowFailure(D3D12SerializeRootSignature(
        &rootSignatureDesc,
        D3D_ROOT_SIGNATURE_VERSION_1,
        &serializedRootSignature,
        nullptr));

    ComPtr<ID3D12RootSignature> rootSignature;
    ThrowFailure(device->CreateRootSignature(
        0,
        serializedRootSignature->GetBufferPointer(),
        serializedRootSignature->GetBufferSize(),
        IID_PPV_ARGS(&rootSignature)));

    D3D12_COMPUTE_PIPELINE_STATE_DESC pipelineStateDesc{};
    pipelineStateDesc.pRootSignature = rootSignature.Get();
    pipelineStateDesc.CS.pShaderBytecode = compiledShader->GetBufferPointer();
    pipelineStateDesc.CS.BytecodeLength = compiledShader->GetBufferSize();

    ComPtr<ID3D12PipelineState> pipelineState;
    ThrowFailure(device->CreateComputePipelineState(&pipelineStateDesc, IID_PPV_ARGS(&pipelineState)));

    session.End();
    session.RethrowConsumerException();

    // The pipeline-state callback carries no identifying payload (no desc, no
    // shader hash), and the runtime/driver create their own internal PSOs, so we
    // can't attribute a specific record to our CreateComputePipelineState call. We
    // only confirm the OnPipelineStateCreation path fires on our thread with sane
    // common details. Placement (real GPU virtual address vs. sentinel) is
    // driver-dependent, so that edge belongs in a unit test.
    const auto* record = FindRecord(recorder->PipelineStates, [&](const auto& r)
    {
        return r.ProcessId == GetCurrentProcessId() && r.ThreadId == GetCurrentThreadId();
    });
    ASSERT_NE(record, nullptr) << "Expected a pipeline-state creation callback on this thread";

    EXPECT_NE(record->ObjectId, 0u);
    EXPECT_GT(record->Timestamp, 0) << "Pipeline-state timestamp should be a sane positive value";
}

// ---------------------------------------------------------------------------
// Object name update and destruction
// ---------------------------------------------------------------------------

TEST(D3D12ObjectTests, ObjectName)
{
    constexpr UINT64 DistinctiveWidth = 0x4E5F60;
    const wchar_t* const ExpectedName = L"DxTimingCaptureLibraryFunctionalTestResource";

    auto callbacks = std::make_unique<RecordingApiObjectCallbacks>();
    auto* recorder = callbacks.get();

    SimpleEtwSession session;
    session.SetApiObjectCallbacks(std::move(callbacks));
    session.Begin();

    ComPtr<ID3D12Device> device = CreateDevice();

    D3D12_HEAP_PROPERTIES heapProperties{};
    heapProperties.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC resourceDesc = MakeBufferDesc(DistinctiveWidth);

    ComPtr<ID3D12Resource> resource;
    ThrowFailure(device->CreateCommittedResource(
        &heapProperties,
        D3D12_HEAP_FLAG_NONE,
        &resourceDesc,
        D3D12_RESOURCE_STATE_COMMON,
        nullptr,
        IID_PPV_ARGS(&resource)));

    TimestampWindow window;
    window.Open();
    ThrowFailure(resource->SetName(ExpectedName));
    window.Close();

    session.End();
    session.RethrowConsumerException();

    // Find the object id assigned to our resource, then confirm a name update arrived
    // for it carrying the name we set.
    const auto* resourceRecord = FindRecord(recorder->CommittedResources, [&](const auto& r)
    {
        return r.Desc.Width == DistinctiveWidth;
    });
    ASSERT_NE(resourceRecord, nullptr) << "Expected a committed-resource callback for the named buffer";

    const auto* nameRecord = FindRecord(recorder->NameUpdates, [&](const auto& r)
    {
        return r.ObjectId == resourceRecord->ObjectId && r.Type == ApiObjectType::Resource;
    });
    ASSERT_NE(nameRecord, nullptr) << "Expected a name update for the created resource";
    EXPECT_EQ(nameRecord->Name, ExpectedName);
    window.ExpectContains(nameRecord->Timestamp);
}

TEST(D3D12ObjectTests, ObjectDestruction)
{
    constexpr UINT64 DistinctiveWidth = 0x5F6071;

    auto callbacks = std::make_unique<RecordingApiObjectCallbacks>();
    auto* recorder = callbacks.get();

    SimpleEtwSession session;
    session.SetApiObjectCallbacks(std::move(callbacks));
    session.Begin();

    ComPtr<ID3D12Device> device = CreateDevice();

    D3D12_HEAP_PROPERTIES heapProperties{};
    heapProperties.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC resourceDesc = MakeBufferDesc(DistinctiveWidth);

    ComPtr<ID3D12Resource> resource;
    ThrowFailure(device->CreateCommittedResource(
        &heapProperties,
        D3D12_HEAP_FLAG_NONE,
        &resourceDesc,
        D3D12_RESOURCE_STATE_COMMON,
        nullptr,
        IID_PPV_ARGS(&resource)));

    // Release the resource while the session is still running so the lifetime event is
    // captured, bracketing it so we can confirm the destruction timestamp lines up with
    // when we actually released the object.
    TimestampWindow window;
    window.Open();
    resource.Reset();
    window.Close();

    session.End();
    session.RethrowConsumerException();

    const auto* resourceRecord = FindRecord(recorder->CommittedResources, [&](const auto& r)
    {
        return r.Desc.Width == DistinctiveWidth;
    });
    ASSERT_NE(resourceRecord, nullptr) << "Expected a committed-resource callback for the destroyed buffer";

    const auto* destructionRecord = FindRecord(recorder->Destructions, [&](const auto& r)
    {
        return r.ObjectId == resourceRecord->ObjectId && r.Type == ApiObjectType::Resource;
    });
    ASSERT_NE(destructionRecord, nullptr) << "Expected a destruction callback for the released resource";
    window.ExpectContains(destructionRecord->Timestamp);
}

// ---------------------------------------------------------------------------
// Robustness: callback failure propagation and lost-event reporting
// ---------------------------------------------------------------------------

TEST(D3D12ObjectTests, CallbackFailureIsPropagated)
{
    auto callbacks = std::make_unique<RecordingApiObjectCallbacks>();
    auto* recorder = callbacks.get();
    recorder->FailNextDescriptorHeapCreationWith(E_FAIL);

    SimpleEtwSession session;
    session.SetApiObjectCallbacks(std::move(callbacks));
    session.Begin();

    ComPtr<ID3D12Device> device = CreateDevice();

    D3D12_DESCRIPTOR_HEAP_DESC descriptorHeapDesc{};
    descriptorHeapDesc.NumDescriptors = 8;
    descriptorHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    descriptorHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

    ComPtr<ID3D12DescriptorHeap> heap;
    ThrowFailure(device->CreateDescriptorHeap(&descriptorHeapDesc, IID_PPV_ARGS(&heap)));

    session.End();

    // The library must turn a failing callback into a thrown exception rather than
    // swallowing it; the session captured it off the consumer thread.
    EXPECT_TRUE(session.HadConsumerException()) << "Library should propagate a failing callback HRESULT";
}

TEST(D3D12ObjectTests, LostEventStatisticsAreZeroUnderNormalLoad)
{
    auto callbacks = std::make_unique<RecordingApiObjectCallbacks>();

    SimpleEtwSession session;
    session.SetApiObjectCallbacks(std::move(callbacks));
    session.Begin();

    ComPtr<ID3D12Device> device = CreateDevice();

    D3D12_HEAP_PROPERTIES heapProperties{};
    heapProperties.Type = D3D12_HEAP_TYPE_DEFAULT;

    for (UINT32 i = 0; i < 16; ++i)
    {
        D3D12_RESOURCE_DESC resourceDesc = MakeBufferDesc(0x600000 + i);
        ComPtr<ID3D12Resource> resource;
        ThrowFailure(device->CreateCommittedResource(
            &heapProperties,
            D3D12_HEAP_FLAG_NONE,
            &resourceDesc,
            D3D12_RESOURCE_STATE_COMMON,
            nullptr,
            IID_PPV_ARGS(&resource)));
    }

    session.End();
    session.RethrowConsumerException();

    // A light, in-process workload should not overrun the ETW buffers, so the
    // library should report no lost-event diagnostics through the sink.
    EXPECT_EQ(session.CountDiagnostics(DiagnosticCode::EventsLost), 0u);
    EXPECT_EQ(session.CountDiagnostics(DiagnosticCode::BuffersLost), 0u);
}

// ---------------------------------------------------------------------------
// Heap implied/real classification
//
// These tests drive a D3D12ObjectProcessor directly (no live D3D12 device) so they can
// deterministically order the heap/resource/allocation events that decide whether a heap is an
// implied heap (owned by a committed resource, so not stored on its own) or a real heap (stored).
// ---------------------------------------------------------------------------

namespace
{
    // Drives a D3D12ObjectProcessor and counts the resulting object creations/destructions.
    class ApiObjectProcessorHarness
    {
        static constexpr uint64_t kDeviceAddress = 0xD000;
        static constexpr uint32_t kProcessId = 1234;
        static constexpr uint32_t kThreadId = 5678;

        AllocationTracker m_allocationTracker;
        RecordingApiObjectCallbacks m_callbacks;
        D3D12ObjectProcessor m_processor{ m_allocationTracker };
        uint64_t m_nextTimestamp = 0;

    public:
        int HeapCreationCount = 0;
        int ResourceCreationCount = 0;
        int DestructionCount = 0;

        ApiObjectProcessorHarness()
        {
            // The processor raises this for every inserted object; the owner normally hooks it up.
            m_processor.OnApiObjectInserted = [](D3D12ObjectProcessor::ApiObjectInsertedEventArgs) {};

            // Register a device so heaps and resources can resolve a device id.
            D3D12ObjectProcessor::DeviceEntry deviceEntry{};
            deviceEntry.ObjectAddress = kDeviceAddress;
            deviceEntry.Timestamp = NextTimestamp();
            m_processor.AddDevice(deviceEntry, &m_callbacks);
            RefreshCounts();
        }

        // Registers a virtual-address allocation for an object so that, once its heap event arrives,
        // the heap is "complete" (has a GpuVirtualAddress) and gets inserted immediately.
        void AddAllocation(uint64_t objectAddress, uint64_t gpuVirtualAddress, uint64_t size)
        {
            AllocationInfo info{};
            info.device = kDeviceAddress;
            info.object = objectAddress;

            VirtualAddressInfos virtualAddress{};
            virtualAddress.startAddress = gpuVirtualAddress;
            virtualAddress.endAddress = gpuVirtualAddress + size;
            info.virtualAddressInfos.push_back(virtualAddress);

            const auto handle = m_allocationTracker.AddAllocationInfo(NextTimestamp(), info);
            m_processor.AddAllocationInfo(handle, NextTimestamp(), &m_callbacks);
            RefreshCounts();
        }

        void AddHeap(uint64_t heapAddress, uint64_t conjoinedResource, uint64_t size)
        {
            D3D12ObjectProcessor::HeapEntry heapEntry;
            heapEntry.Timestamp = NextTimestamp();
            heapEntry.ObjectAddress = heapAddress;
            heapEntry.ProcessId = kProcessId;
            heapEntry.ThreadId = kThreadId;
            heapEntry.ConjoinedResource = conjoinedResource;
            heapEntry.Placement.GpuVirtualSize = size;
            m_processor.AddHeap(heapEntry, kDeviceAddress, &m_callbacks);
            RefreshCounts();
        }

        void AddCommittedResource(uint64_t resourceAddress, uint64_t size)
        {
            D3D12ObjectProcessor::ResourceEntry entry;
            entry.Timestamp = NextTimestamp();
            entry.ObjectAddress = resourceAddress;
            entry.ProcessId = kProcessId;
            entry.ThreadId = kThreadId;
            entry.ResourceHeapType = ResourceHeapType::ImplicitHeap;
            entry.Desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            entry.Desc.Width = size;
            m_processor.AddResource(entry, kDeviceAddress, &m_callbacks);
            RefreshCounts();
        }

        void AddPlacedResource(uint64_t resourceAddress, uint64_t heapAddress, uint64_t heapOffset, uint64_t size)
        {
            D3D12ObjectProcessor::ResourceEntry entry;
            entry.Timestamp = NextTimestamp();
            entry.ObjectAddress = resourceAddress;
            entry.ProcessId = kProcessId;
            entry.ThreadId = kThreadId;
            entry.ResourceHeapType = ResourceHeapType::ImmutableHeap;
            entry.ResourceHeapData.HeapObjectAddress = heapAddress;
            entry.ResourceHeapData.ImmutableHeapOffset = heapOffset;
            entry.ResourceHeapData.PlacedSize = size;
            entry.Desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            entry.Desc.Width = size;
            m_processor.AddResource(entry, kDeviceAddress, &m_callbacks);
            RefreshCounts();
        }

        void AddImplicitResource(uint64_t resourceAddress)
        {
            D3D12ObjectProcessor::ResourceEntry entry;
            entry.Timestamp = NextTimestamp();
            entry.ObjectAddress = resourceAddress;
            entry.ProcessId = kProcessId;
            entry.ThreadId = kThreadId;
            entry.ResourceHeapType = ResourceHeapType::ImplicitResource;
            entry.Desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            m_processor.AddResource(entry, kDeviceAddress, &m_callbacks);
            RefreshCounts();
        }

        void DestroyHeap(uint64_t heapAddress)
        {
            m_processor.DestroyApiObject(ApiObjectType::Heap, heapAddress, NextTimestamp(), &m_callbacks);
            RefreshCounts();
        }

        void Finalize()
        {
            m_processor.ProcessDeferredEntries(&m_callbacks);
            RefreshCounts();
        }

    private:
        uint64_t NextTimestamp() { return ++m_nextTimestamp; }

        // The recording callbacks capture every creation/destruction; surface the counts the
        // classification tests assert on. Committed, placed and reserved resources are all
        // "resources" here (implicit resources are never stored as their own object).
        void RefreshCounts()
        {
            HeapCreationCount = static_cast<int>(m_callbacks.Heaps.size());
            ResourceCreationCount = static_cast<int>(
                m_callbacks.CommittedResources.size()
                + m_callbacks.PlacedResources.size()
                + m_callbacks.ReservedResources.size());
            DestructionCount = static_cast<int>(m_callbacks.Destructions.size());
        }
    };
}

// An implied heap observed before its owning committed resource (rundown-style order) must not be
// stored as its own object; the committed resource owns the memory instead.
TEST(D3D12ObjectTests, ImpliedHeap_HeapBeforeResource_NotStored)
{
    constexpr uint64_t resourceAddress = 0xA0;
    constexpr uint64_t heapAddress = 0xE0;
    constexpr uint64_t size = 4096;

    ApiObjectProcessorHarness harness;
    harness.AddAllocation(heapAddress, 0x10000, size);
    harness.AddHeap(heapAddress, /*conjoinedResource*/ resourceAddress, size);

    // The heap is ambiguous (non-zero conjoined resource) so it's held back, not stored.
    EXPECT_EQ(harness.HeapCreationCount, 0);

    harness.AddCommittedResource(resourceAddress, size);

    // The committed resource is stored and the implied heap is dropped, even after finalization.
    EXPECT_EQ(harness.ResourceCreationCount, 1);
    EXPECT_EQ(harness.HeapCreationCount, 0);

    harness.Finalize();
    EXPECT_EQ(harness.HeapCreationCount, 0);
    EXPECT_EQ(harness.ResourceCreationCount, 1);
}

// An implied heap observed after its owning committed resource (live-style order) must not be stored.
TEST(D3D12ObjectTests, ImpliedHeap_ResourceBeforeHeap_NotStored)
{
    constexpr uint64_t resourceAddress = 0xA0;
    constexpr uint64_t heapAddress = 0xE0;
    constexpr uint64_t size = 4096;

    ApiObjectProcessorHarness harness;
    harness.AddCommittedResource(resourceAddress, size);
    EXPECT_EQ(harness.ResourceCreationCount, 0); // deferred until the heap arrives

    harness.AddAllocation(heapAddress, 0x10000, size);
    harness.AddHeap(heapAddress, /*conjoinedResource*/ resourceAddress, size);

    EXPECT_EQ(harness.ResourceCreationCount, 1);
    EXPECT_EQ(harness.HeapCreationCount, 0);

    harness.Finalize();
    EXPECT_EQ(harness.HeapCreationCount, 0);
    EXPECT_EQ(harness.ResourceCreationCount, 1);
}

// A heap with no conjoined resource is treated as a real heap and is stored immediately.
TEST(D3D12ObjectTests, RealHeap_NoConjoinedResource_StoredImmediately)
{
    constexpr uint64_t heapAddress = 0xE0;
    constexpr uint64_t size = 65536;

    ApiObjectProcessorHarness harness;
    harness.AddAllocation(heapAddress, 0x10000, size);
    harness.AddHeap(heapAddress, /*conjoinedResource*/ 0, size);

    EXPECT_EQ(harness.HeapCreationCount, 1);

    harness.Finalize();
    EXPECT_EQ(harness.HeapCreationCount, 1); // not double-stored
}

// A real heap's conjoined resource points at a runtime implicit resource. In the common (live) order
// that implicit resource is reported before the heap, so the heap is recognized as real and stored
// immediately without waiting for the capture to end.
TEST(D3D12ObjectTests, RealHeap_ImplicitResourceBeforeHeap_StoredImmediately)
{
    constexpr uint64_t heapAddress = 0xE0;
    constexpr uint64_t implicitResourceAddress = 0xA0;
    constexpr uint64_t size = 65536;

    ApiObjectProcessorHarness harness;
    harness.AddImplicitResource(implicitResourceAddress);
    EXPECT_EQ(harness.ResourceCreationCount, 0); // implicit resources aren't stored as their own object

    harness.AddAllocation(heapAddress, 0x10000, size);
    harness.AddHeap(heapAddress, /*conjoinedResource*/ implicitResourceAddress, size);

    EXPECT_EQ(harness.HeapCreationCount, 1);
    EXPECT_EQ(harness.ResourceCreationCount, 0);

    harness.Finalize();
    EXPECT_EQ(harness.HeapCreationCount, 1); // not double-stored
    EXPECT_EQ(harness.ResourceCreationCount, 0);
}

// If the heap arrives before its implicit resource it's held back as ambiguous. When the implicit
// resource arrives it reveals the heap is real, so the heap is materialized right then rather than
// waiting for the capture to end.
TEST(D3D12ObjectTests, RealHeap_ImplicitResourceAfterHeap_StoredWhenResourceArrives)
{
    constexpr uint64_t heapAddress = 0xE0;
    constexpr uint64_t implicitResourceAddress = 0xA0;
    constexpr uint64_t size = 65536;

    ApiObjectProcessorHarness harness;
    harness.AddAllocation(heapAddress, 0x10000, size);
    harness.AddHeap(heapAddress, /*conjoinedResource*/ implicitResourceAddress, size);
    EXPECT_EQ(harness.HeapCreationCount, 0); // ambiguous, held back

    harness.AddImplicitResource(implicitResourceAddress);
    EXPECT_EQ(harness.HeapCreationCount, 1); // implicit resource reveals the heap is real, materialized now
    EXPECT_EQ(harness.ResourceCreationCount, 0);

    harness.Finalize();
    EXPECT_EQ(harness.HeapCreationCount, 1); // not double-stored
    EXPECT_EQ(harness.ResourceCreationCount, 0);
}

// A placed resource referencing a heap proves the heap is a real (immutable) heap, so an ambiguous
// heap observed first is materialized once the placed resource arrives.
TEST(D3D12ObjectTests, ExplicitHeap_PlacedResourceAfterHeap_Stored)
{
    constexpr uint64_t heapAddress = 0xE0;
    constexpr uint64_t resourceAddress = 0xA0;
    constexpr uint64_t size = 65536;

    ApiObjectProcessorHarness harness;
    harness.AddAllocation(heapAddress, 0x10000, size);
    harness.AddHeap(heapAddress, /*conjoinedResource*/ 0x9999, size); // non-zero, ambiguous
    EXPECT_EQ(harness.HeapCreationCount, 0);

    harness.AddPlacedResource(resourceAddress, heapAddress, /*heapOffset*/ 0, /*size*/ 4096);

    EXPECT_EQ(harness.HeapCreationCount, 1);
    EXPECT_EQ(harness.ResourceCreationCount, 1);

    harness.Finalize();
    EXPECT_EQ(harness.HeapCreationCount, 1); // not double-stored
    EXPECT_EQ(harness.ResourceCreationCount, 1);
}

// A placed resource observed before its heap also results in the heap being stored.
TEST(D3D12ObjectTests, ExplicitHeap_PlacedResourceBeforeHeap_Stored)
{
    constexpr uint64_t heapAddress = 0xE0;
    constexpr uint64_t resourceAddress = 0xA0;
    constexpr uint64_t size = 65536;

    ApiObjectProcessorHarness harness;
    harness.AddPlacedResource(resourceAddress, heapAddress, /*heapOffset*/ 0, /*size*/ 4096);
    EXPECT_EQ(harness.ResourceCreationCount, 0); // deferred until the heap arrives

    harness.AddAllocation(heapAddress, 0x10000, size);
    harness.AddHeap(heapAddress, /*conjoinedResource*/ 0x9999, size); // non-zero, but a placed resource referenced it

    EXPECT_EQ(harness.HeapCreationCount, 1);
    EXPECT_EQ(harness.ResourceCreationCount, 1);

    harness.Finalize();
    EXPECT_EQ(harness.HeapCreationCount, 1); // not double-stored
    EXPECT_EQ(harness.ResourceCreationCount, 1);
}

// An ambiguous heap that's never claimed by a committed resource is assumed to be a real heap and
// is stored when the capture ends.
TEST(D3D12ObjectTests, AmbiguousHeap_Unclaimed_StoredAtFinalize)
{
    constexpr uint64_t heapAddress = 0xE0;
    constexpr uint64_t size = 65536;

    ApiObjectProcessorHarness harness;
    harness.AddAllocation(heapAddress, 0x10000, size);
    harness.AddHeap(heapAddress, /*conjoinedResource*/ 0x9999, size);
    EXPECT_EQ(harness.HeapCreationCount, 0);

    harness.Finalize();
    EXPECT_EQ(harness.HeapCreationCount, 1);
}

// An ambiguous heap destroyed before it's classified is materialized (so real heaps aren't lost)
// and then destroyed.
TEST(D3D12ObjectTests, AmbiguousHeap_DestroyedBeforeClassification_Materialized)
{
    constexpr uint64_t heapAddress = 0xE0;
    constexpr uint64_t size = 65536;

    ApiObjectProcessorHarness harness;
    harness.AddAllocation(heapAddress, 0x10000, size);
    harness.AddHeap(heapAddress, /*conjoinedResource*/ 0x9999, size);
    EXPECT_EQ(harness.HeapCreationCount, 0);

    harness.DestroyHeap(heapAddress);
    EXPECT_EQ(harness.HeapCreationCount, 1);
    EXPECT_EQ(harness.DestructionCount, 1);

    harness.Finalize();
    EXPECT_EQ(harness.HeapCreationCount, 1); // not double-stored
}
