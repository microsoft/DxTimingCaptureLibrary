// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "pch.h"

#include <optional>
#include <vector>

// MarkerOp.h (pulled in transitively) expands the D3D12_MARKER_API_* constants
// from the D3D12 ETW manifest headers, so those must be included first.
#include <d3d12.h>
#include <D3D12Events.h>

#include <DxTimingCaptureLibrary/EtwCallbacks.h>

// D3D12ObjectProcessor.h is an internal library header that uses DirectX::Etw names
// unqualified, so open the namespace before including it.
using namespace DirectX::Etw;

#include <lib/D3D12ObjectProcessor.h>

// These tests drive D3D12ObjectProcessor directly with synthetic entries to exercise
// the deferred/out-of-order correlation logic — the part a live ETW session cannot
// cover deterministically because it cannot force the order events arrive in. The
// processor turns three streams of ETW events (object creation, the owning device, and
// the backing GPU allocation) into object-creation callbacks; the order is not
// guaranteed, so it defers an object until every piece it needs has arrived.

namespace
{
    // Records the object-creation and destruction callbacks the processor makes, with
    // the fields these tests assert on.
    class RecordingCallbacks : public ApiObjectCallbacks
    {
    public:
        struct DeviceRecord { UINT64 ObjectId; };
        struct CommandAllocatorRecord { UINT64 DeviceId; D3D12_COMMAND_LIST_TYPE CommandListType; UINT64 ObjectId; };
        struct ResourceRecord { UINT64 DeviceId; UINT64 GpuVirtualAddress; UINT64 ObjectId; };
        struct HeapRecord { UINT64 DeviceId; UINT64 GpuVirtualAddress; UINT64 ObjectId; };
        struct DestructionRecord { ApiObjectType Type; UINT64 ObjectId; };

        std::vector<DeviceRecord> Devices;
        std::vector<CommandAllocatorRecord> CommandAllocators;
        std::vector<ResourceRecord> CommittedResources;
        std::vector<ResourceRecord> PlacedResources;
        std::vector<HeapRecord> Heaps;
        std::vector<DestructionRecord> Destructions;

        HRESULT OnDeviceCreation(INT64, const DeviceInfo*, UINT64* objectId) override
        {
            *objectId = ++m_nextObjectId;
            Devices.push_back({ *objectId });
            return S_OK;
        }

        HRESULT OnCommittedResourceCreation(INT64, UINT64 deviceId, UINT32, UINT32, const D3D12_RESOURCE_DESC*, const ObjectPlacementInfo* placement, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS, UINT64* objectId) override
        {
            *objectId = ++m_nextObjectId;
            CommittedResources.push_back({ deviceId, placement->GpuVirtualAddress, *objectId });
            return S_OK;
        }

        HRESULT OnPlacedResourceCreation(INT64, UINT64 deviceId, UINT32, UINT32, const D3D12_RESOURCE_DESC*, const ObjectPlacementInfo* placement, UINT64* objectId) override
        {
            *objectId = ++m_nextObjectId;
            PlacedResources.push_back({ deviceId, placement->GpuVirtualAddress, *objectId });
            return S_OK;
        }

        HRESULT OnHeapCreation(INT64, UINT64 deviceId, UINT32, UINT32, const D3D12_HEAP_DESC*, const ObjectPlacementInfo* placement, UINT64* objectId) override
        {
            *objectId = ++m_nextObjectId;
            Heaps.push_back({ deviceId, placement->GpuVirtualAddress, *objectId });
            return S_OK;
        }

        HRESULT OnCommandAllocatorCreation(INT64, UINT64 deviceId, UINT32, UINT32, D3D12_COMMAND_LIST_TYPE commandListType, const ObjectPlacementInfo*, UINT64* objectId) override
        {
            *objectId = ++m_nextObjectId;
            CommandAllocators.push_back({ deviceId, commandListType, *objectId });
            return S_OK;
        }

        HRESULT OnObjectDestruction(INT64, ApiObjectType type, UINT64 objectId) override
        {
            Destructions.push_back({ type, objectId });
            return S_OK;
        }

        // Unused by these tests.
        HRESULT OnReservedResourceCreation(INT64, UINT64, UINT32, UINT32, const D3D12_RESOURCE_DESC*, const ReservedResourceInfo*, UINT64* objectId) override { *objectId = ++m_nextObjectId; return S_OK; }
        HRESULT OnDescriptorHeapCreation(INT64, UINT64, UINT32, UINT32, const D3D12_DESCRIPTOR_HEAP_DESC*, const ObjectPlacementInfo*, UINT64* objectId) override { *objectId = ++m_nextObjectId; return S_OK; }
        HRESULT OnPipelineStateCreation(INT64, UINT64, UINT32, UINT32, const ObjectPlacementInfo*, UINT64* objectId) override { *objectId = ++m_nextObjectId; return S_OK; }
        HRESULT OnStateObjectCreation(INT64, UINT64, UINT32, UINT32, const ObjectPlacementInfo*, UINT64* objectId) override { *objectId = ++m_nextObjectId; return S_OK; }
        HRESULT OnMetaCommandCreation(INT64, UINT64, UINT32, UINT32, REFGUID, const ObjectPlacementInfo*, UINT64* objectId) override { *objectId = ++m_nextObjectId; return S_OK; }
        HRESULT OnApiObjectName(INT64, ApiObjectType, UINT64, std::wstring_view) override { return S_OK; }
        HRESULT OnApiObjectSizeAndAddress(INT64, ApiObjectType, UINT64, UINT64, UINT64) override { return S_OK; }

    private:
        UINT64 m_nextObjectId{ 1 };
    };

    // A processor plus its allocation tracker and callbacks, wired together. The
    // processor requires OnApiObjectInserted to be set (it invokes it on every insert),
    // so a no-op is installed here.
    struct ProcessorFixture
    {
        AllocationTracker AllocationTracker;
        RecordingCallbacks Callbacks;
        D3D12ObjectProcessor Processor{ AllocationTracker };

        ProcessorFixture()
        {
            Processor.OnApiObjectInserted = [](D3D12ObjectProcessor::ApiObjectInsertedEventArgs) {};
        }

        // Registers a GPU allocation for an object so the processor can resolve that
        // object's GPU virtual address, mirroring a DXGK AllocationInfo event.
        void RegisterAllocation(uint64_t objectAddress, uint64_t gpuVirtualAddress, uint64_t size)
        {
            AllocationInfo info{};
            info.object = objectAddress;
            info.virtualAddressInfos.push_back({ 0, 0, gpuVirtualAddress, gpuVirtualAddress + size });
            info.kmtInfos.push_back({ 0, 0, 0, size });
            const AllocationTracker::Handle handle = AllocationTracker.AddAllocationInfo(2000, info);
            Processor.AddAllocationInfo(handle, 2000, &Callbacks);
        }
    };

    constexpr uint64_t DeviceAddress = 0xD000;

    D3D12ObjectProcessor::DeviceEntry MakeDevice(uint64_t objectAddress = DeviceAddress)
    {
        D3D12ObjectProcessor::DeviceEntry entry;
        entry.ObjectAddress = objectAddress;
        entry.Timestamp = 1000;
        entry.Data = {};
        return entry;
    }

    D3D12ObjectProcessor::CommandAllocatorEntry MakeCommandAllocator(uint64_t objectAddress, D3D12_COMMAND_LIST_TYPE type)
    {
        D3D12ObjectProcessor::CommandAllocatorEntry entry;
        entry.ObjectAddress = objectAddress;
        entry.Timestamp = 1000;
        entry.CommandListType = static_cast<uint32_t>(type);
        return entry;
    }

    // A committed resource owns an implicit heap. The heap event names the resource via
    // its ConjoinedResource field, which is how the two are correlated.
    D3D12ObjectProcessor::ResourceEntry MakeCommittedResource(uint64_t objectAddress)
    {
        D3D12ObjectProcessor::ResourceEntry entry;
        entry.ObjectAddress = objectAddress;
        entry.Timestamp = 1000;
        entry.ResourceHeapType = ResourceHeapType::ImplicitHeap;
        return entry;
    }

    // A placed resource lives at an offset within a separately-created heap.
    D3D12ObjectProcessor::ResourceEntry MakePlacedResource(uint64_t objectAddress, uint64_t heapAddress, uint64_t heapOffset)
    {
        D3D12ObjectProcessor::ResourceEntry entry;
        entry.ObjectAddress = objectAddress;
        entry.Timestamp = 1000;
        entry.ResourceHeapType = ResourceHeapType::ImmutableHeap;
        entry.ResourceHeapData.HeapObjectAddress = heapAddress;
        entry.ResourceHeapData.ImmutableHeapOffset = heapOffset;
        entry.ResourceHeapData.PlacedSize = 0x1000;
        entry.ResourceHeapData.PlacedAlignment = 0x10000;
        return entry;
    }

    D3D12ObjectProcessor::HeapEntry MakeHeap(uint64_t objectAddress, uint64_t conjoinedResource = D3D12ObjectProcessor::HeapEntry::InvalidValue)
    {
        D3D12ObjectProcessor::HeapEntry entry;
        entry.ObjectAddress = objectAddress;
        entry.Timestamp = 1000;
        entry.ConjoinedResource = conjoinedResource;
        entry.Desc = {};
        return entry;
    }
}

// --- Device correlation -----------------------------------------------------

TEST(D3D12ObjectProcessorTests, CommandAllocator_InsertedWhenDeviceKnown)
{
    ProcessorFixture fixture;

    fixture.Processor.AddDevice(MakeDevice(), &fixture.Callbacks);
    ASSERT_EQ(fixture.Callbacks.Devices.size(), 1u);
    const UINT64 deviceId = fixture.Callbacks.Devices.front().ObjectId;

    fixture.Processor.AddCommandAllocator(MakeCommandAllocator(0xA000, D3D12_COMMAND_LIST_TYPE_DIRECT), DeviceAddress, &fixture.Callbacks);

    ASSERT_EQ(fixture.Callbacks.CommandAllocators.size(), 1u);
    EXPECT_EQ(fixture.Callbacks.CommandAllocators.front().DeviceId, deviceId);
    EXPECT_EQ(fixture.Callbacks.CommandAllocators.front().CommandListType, D3D12_COMMAND_LIST_TYPE_DIRECT);
}

TEST(D3D12ObjectProcessorTests, CommandAllocator_DeferredUntilDeviceArrives)
{
    ProcessorFixture fixture;

    // The command allocator's owning device has not been reported yet, so the
    // processor must hold the allocator rather than insert it with an unknown device.
    fixture.Processor.AddCommandAllocator(MakeCommandAllocator(0xA000, D3D12_COMMAND_LIST_TYPE_COMPUTE), DeviceAddress, &fixture.Callbacks);
    EXPECT_TRUE(fixture.Callbacks.CommandAllocators.empty()) << "Allocator should be deferred until its device is known";

    fixture.Processor.AddDevice(MakeDevice(), &fixture.Callbacks);
    const UINT64 deviceId = fixture.Callbacks.Devices.front().ObjectId;

    ASSERT_EQ(fixture.Callbacks.CommandAllocators.size(), 1u) << "Allocator should flush once its device arrives";
    EXPECT_EQ(fixture.Callbacks.CommandAllocators.front().DeviceId, deviceId);
    EXPECT_EQ(fixture.Callbacks.CommandAllocators.front().CommandListType, D3D12_COMMAND_LIST_TYPE_COMPUTE);
}

// --- Committed resource / implicit heap correlation -------------------------

TEST(D3D12ObjectProcessorTests, CommittedResource_ResourceReportedBeforeHeap)
{
    constexpr uint64_t HeapAddress = 0x4000;
    constexpr uint64_t ResourceAddress = 0x5000;
    constexpr uint64_t HeapBaseAddress = 0x100000;

    ProcessorFixture fixture;
    fixture.Processor.AddDevice(MakeDevice(), &fixture.Callbacks);
    const UINT64 deviceId = fixture.Callbacks.Devices.front().ObjectId;

    // The committed resource is reported before its implicit heap, so its address is not
    // yet known and the processor defers it.
    fixture.Processor.AddResource(MakeCommittedResource(ResourceAddress), DeviceAddress, &fixture.Callbacks);
    EXPECT_TRUE(fixture.Callbacks.CommittedResources.empty()) << "Committed resource should be deferred until its heap arrives";

    // The implicit heap (with its allocation) arrives and resolves the resource's
    // address through the conjoined-resource link.
    fixture.RegisterAllocation(HeapAddress, HeapBaseAddress, 0x1000);
    fixture.Processor.AddHeap(MakeHeap(HeapAddress, /*conjoinedResource*/ ResourceAddress), DeviceAddress, &fixture.Callbacks);

    ASSERT_EQ(fixture.Callbacks.CommittedResources.size(), 1u) << "Committed resource should flush once its heap arrives";
    EXPECT_EQ(fixture.Callbacks.CommittedResources.front().DeviceId, deviceId);
    EXPECT_EQ(fixture.Callbacks.CommittedResources.front().GpuVirtualAddress, HeapBaseAddress);

    // The implicit heap is not surfaced as its own object (it belongs to the resource).
    EXPECT_TRUE(fixture.Callbacks.Heaps.empty());
}

TEST(D3D12ObjectProcessorTests, CommittedResource_HeapReportedBeforeResource)
{
    constexpr uint64_t HeapAddress = 0x4000;
    constexpr uint64_t ResourceAddress = 0x5000;
    constexpr uint64_t HeapBaseAddress = 0x200000;

    ProcessorFixture fixture;
    fixture.Processor.AddDevice(MakeDevice(), &fixture.Callbacks);
    const UINT64 deviceId = fixture.Callbacks.Devices.front().ObjectId;

    // Heap (with its allocation) arrives first; the committed resource follows and is
    // matched to the heap's address via the conjoined-resource link.
    fixture.RegisterAllocation(HeapAddress, HeapBaseAddress, 0x1000);
    fixture.Processor.AddHeap(MakeHeap(HeapAddress, /*conjoinedResource*/ ResourceAddress), DeviceAddress, &fixture.Callbacks);

    fixture.Processor.AddResource(MakeCommittedResource(ResourceAddress), DeviceAddress, &fixture.Callbacks);

    ASSERT_EQ(fixture.Callbacks.CommittedResources.size(), 1u);
    EXPECT_EQ(fixture.Callbacks.CommittedResources.front().DeviceId, deviceId);
    EXPECT_EQ(fixture.Callbacks.CommittedResources.front().GpuVirtualAddress, HeapBaseAddress);

    // The implicit heap is not surfaced as its own object regardless of arrival order.
    // A heap reported before its committed resource is held back as an ambiguous candidate;
    // once the committed resource arrives and claims it, the heap is recognized as implied
    // and dropped (matching the resource-first ordering in the test above).
    EXPECT_TRUE(fixture.Callbacks.Heaps.empty());
}

// --- Placed resource / explicit heap correlation ----------------------------

TEST(D3D12ObjectProcessorTests, PlacedResource_ResourceReportedBeforeHeap)
{
    constexpr uint64_t HeapAddress = 0x6000;
    constexpr uint64_t ResourceAddress = 0x7000;
    constexpr uint64_t HeapBaseAddress = 0x300000;
    constexpr uint64_t HeapOffset = 0x100;

    ProcessorFixture fixture;
    fixture.Processor.AddDevice(MakeDevice(), &fixture.Callbacks);
    const UINT64 deviceId = fixture.Callbacks.Devices.front().ObjectId;

    // The placed resource is reported before its heap, so it cannot yet know its
    // address; the processor defers it against the heap it is waiting on.
    fixture.Processor.AddResource(MakePlacedResource(ResourceAddress, HeapAddress, HeapOffset), DeviceAddress, &fixture.Callbacks);
    EXPECT_TRUE(fixture.Callbacks.PlacedResources.empty()) << "Placed resource should be deferred until its heap arrives";

    fixture.RegisterAllocation(HeapAddress, HeapBaseAddress, 0x10000);
    fixture.Processor.AddHeap(MakeHeap(HeapAddress), DeviceAddress, &fixture.Callbacks);

    ASSERT_EQ(fixture.Callbacks.PlacedResources.size(), 1u) << "Placed resource should flush once its heap arrives";
    EXPECT_EQ(fixture.Callbacks.PlacedResources.front().DeviceId, deviceId);
    EXPECT_EQ(fixture.Callbacks.PlacedResources.front().GpuVirtualAddress, HeapBaseAddress + HeapOffset);

    // The explicit heap is itself a tracked object.
    ASSERT_EQ(fixture.Callbacks.Heaps.size(), 1u);
    EXPECT_EQ(fixture.Callbacks.Heaps.front().GpuVirtualAddress, HeapBaseAddress);
}

// --- End-of-stream flush ----------------------------------------------------

TEST(D3D12ObjectProcessorTests, ProcessDeferredEntries_InsertsHeapWhoseAllocationNeverArrived)
{
    constexpr uint64_t HeapAddress = 0x8000;

    ProcessorFixture fixture;
    fixture.Processor.AddDevice(MakeDevice(), &fixture.Callbacks);

    // No allocation is registered for this heap, so it cannot be completed and is held.
    fixture.Processor.AddHeap(MakeHeap(HeapAddress), DeviceAddress, &fixture.Callbacks);
    EXPECT_TRUE(fixture.Callbacks.Heaps.empty());

    // At end of stream the processor flushes what it was holding. A heap whose
    // allocation never arrived is still emitted, but with a sentinel GPU virtual
    // address (UINT64_MAX) to mark the address as unknown.
    fixture.Processor.ProcessDeferredEntries(&fixture.Callbacks);

    ASSERT_EQ(fixture.Callbacks.Heaps.size(), 1u);
    EXPECT_EQ(fixture.Callbacks.Heaps.front().GpuVirtualAddress, UINT64_MAX);
}

// --- Destruction ------------------------------------------------------------

TEST(D3D12ObjectProcessorTests, Destroy_InsertedObject_FiresDestruction)
{
    constexpr uint64_t AllocatorAddress = 0xA000;

    ProcessorFixture fixture;
    fixture.Processor.AddDevice(MakeDevice(), &fixture.Callbacks);
    fixture.Processor.AddCommandAllocator(MakeCommandAllocator(AllocatorAddress, D3D12_COMMAND_LIST_TYPE_DIRECT), DeviceAddress, &fixture.Callbacks);
    ASSERT_EQ(fixture.Callbacks.CommandAllocators.size(), 1u);
    const UINT64 allocatorId = fixture.Callbacks.CommandAllocators.front().ObjectId;

    fixture.Processor.DestroyApiObject(ApiObjectType::CommandAllocator, AllocatorAddress, 3000, &fixture.Callbacks);

    ASSERT_EQ(fixture.Callbacks.Destructions.size(), 1u);
    EXPECT_EQ(fixture.Callbacks.Destructions.front().Type, ApiObjectType::CommandAllocator);
    EXPECT_EQ(fixture.Callbacks.Destructions.front().ObjectId, allocatorId);
}

TEST(D3D12ObjectProcessorTests, Destroy_DeferredPlacedResource_IsCleanedUp)
{
    constexpr uint64_t HeapAddress = 0x6000;
    constexpr uint64_t ResourceAddress = 0x7000;
    constexpr uint64_t HeapBaseAddress = 0x400000;

    ProcessorFixture fixture;
    fixture.Processor.AddDevice(MakeDevice(), &fixture.Callbacks);

    // Defer a placed resource against a heap that has not arrived...
    fixture.Processor.AddResource(MakePlacedResource(ResourceAddress, HeapAddress, 0x100), DeviceAddress, &fixture.Callbacks);

    // ...then destroy it before the heap shows up. It was never inserted, so there is
    // no destruction callback, and it must be removed from the pending-on-heap set.
    fixture.Processor.DestroyApiObject(ApiObjectType::Resource, ResourceAddress, 3000, &fixture.Callbacks);
    EXPECT_TRUE(fixture.Callbacks.Destructions.empty());

    // When the heap finally arrives the destroyed resource must not resurface (and the
    // processor must not trip over the stale pending entry).
    fixture.RegisterAllocation(HeapAddress, HeapBaseAddress, 0x10000);
    fixture.Processor.AddHeap(MakeHeap(HeapAddress), DeviceAddress, &fixture.Callbacks);

    EXPECT_TRUE(fixture.Callbacks.PlacedResources.empty()) << "A destroyed deferred resource must not be inserted later";
}
