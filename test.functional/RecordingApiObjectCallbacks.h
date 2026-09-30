// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#pragma once

#include <atomic>
#include <optional>
#include <string>
#include <vector>

#include <DxTimingCaptureLibrary/EtwCallbacks.h>

using namespace DirectX::Etw;

// A test double for ApiObjectCallbacks that records every callback the library makes,
// with all of its parameters, into per-kind vectors. Tests inspect those vectors after
// the session ends.
//
// Why record every callback (rather than capturing just one per kind): a live D3D12
// device and its runtime/driver create their own internal objects (resources, heaps,
// ...), so more than one callback of a given kind fires per session. Capturing only one
// risks recording a driver-internal object instead of the one the test created.
// Recording all of them lets a test search for the specific object it expects.
//
// Threading: the library invokes these callbacks on the ETW consumer thread. Tests
// read the vectors only after SimpleEtwSession::End() has joined that thread, which is
// a synchronization barrier, so no locking is needed here.
class RecordingApiObjectCallbacks : public ApiObjectCallbacks
{
public:
    struct DeviceRecord
    {
        INT64 Timestamp;
        DirectX::Etw::DeviceInfo Info;
        UINT64 ObjectId;
    };

    struct DescriptorHeapRecord
    {
        INT64 Timestamp;
        UINT64 DeviceId;
        UINT32 ProcessId;
        UINT32 ThreadId;
        D3D12_DESCRIPTOR_HEAP_DESC Desc;
        DirectX::Etw::ObjectPlacementInfo Placement;
        UINT64 ObjectId;
    };

    struct CommittedResourceRecord
    {
        INT64 Timestamp;
        UINT64 DeviceId;
        UINT32 ProcessId;
        UINT32 ThreadId;
        D3D12_RESOURCE_DESC Desc;
        DirectX::Etw::ObjectPlacementInfo Placement;
        D3D12_HEAP_PROPERTIES HeapProperties;
        D3D12_HEAP_FLAGS HeapFlags;
        UINT64 ObjectId;
    };

    struct PlacedResourceRecord
    {
        INT64 Timestamp;
        UINT64 DeviceId;
        UINT32 ProcessId;
        UINT32 ThreadId;
        D3D12_RESOURCE_DESC Desc;
        DirectX::Etw::ObjectPlacementInfo Placement;
        UINT64 ObjectId;
    };

    struct ReservedResourceRecord
    {
        INT64 Timestamp;
        UINT64 DeviceId;
        UINT32 ProcessId;
        UINT32 ThreadId;
        D3D12_RESOURCE_DESC Desc;
        DirectX::Etw::ReservedResourceInfo Reserved;
        UINT64 ObjectId;
    };

    struct HeapRecord
    {
        INT64 Timestamp;
        UINT64 DeviceId;
        UINT32 ProcessId;
        UINT32 ThreadId;
        D3D12_HEAP_DESC Desc;
        DirectX::Etw::ObjectPlacementInfo Placement;
        UINT64 ObjectId;
    };

    struct PipelineStateRecord
    {
        INT64 Timestamp;
        UINT64 DeviceId;
        UINT32 ProcessId;
        UINT32 ThreadId;
        DirectX::Etw::ObjectPlacementInfo Placement;
        UINT64 ObjectId;
    };

    struct StateObjectRecord
    {
        INT64 Timestamp;
        UINT64 DeviceId;
        UINT32 ProcessId;
        UINT32 ThreadId;
        DirectX::Etw::ObjectPlacementInfo Placement;
        UINT64 ObjectId;
    };

    struct CommandAllocatorRecord
    {
        INT64 Timestamp;
        UINT64 DeviceId;
        UINT32 ProcessId;
        UINT32 ThreadId;
        D3D12_COMMAND_LIST_TYPE CommandListType;
        DirectX::Etw::ObjectPlacementInfo Placement;
        UINT64 ObjectId;
    };

    struct MetaCommandRecord
    {
        INT64 Timestamp;
        UINT64 DeviceId;
        UINT32 ProcessId;
        UINT32 ThreadId;
        GUID CommandId;
        DirectX::Etw::ObjectPlacementInfo Placement;
        UINT64 ObjectId;
    };

    struct NameUpdateRecord
    {
        INT64 Timestamp;
        DirectX::Etw::ApiObjectType Type;
        UINT64 ObjectId;
        std::wstring Name;
    };

    struct SizeAndAddressUpdateRecord
    {
        INT64 Timestamp;
        DirectX::Etw::ApiObjectType Type;
        UINT64 ObjectId;
        UINT64 AllocationSize;
        UINT64 BaseAddress;
    };

    struct DestructionRecord
    {
        INT64 Timestamp;
        DirectX::Etw::ApiObjectType Type;
        UINT64 ObjectId;
    };

    std::vector<DeviceRecord> Devices;
    std::vector<DescriptorHeapRecord> DescriptorHeaps;
    std::vector<CommittedResourceRecord> CommittedResources;
    std::vector<PlacedResourceRecord> PlacedResources;
    std::vector<ReservedResourceRecord> ReservedResources;
    std::vector<HeapRecord> Heaps;
    std::vector<PipelineStateRecord> PipelineStates;
    std::vector<StateObjectRecord> StateObjects;
    std::vector<CommandAllocatorRecord> CommandAllocators;
    std::vector<MetaCommandRecord> MetaCommands;
    std::vector<NameUpdateRecord> NameUpdates;
    std::vector<SizeAndAddressUpdateRecord> SizeAndAddressUpdates;
    std::vector<DestructionRecord> Destructions;

    // When set to a failure HRESULT, the next OnDescriptorHeapCreation returns it
    // instead of S_OK. Used to verify the library propagates callback failures.
    void FailNextDescriptorHeapCreationWith(HRESULT result)
    {
        m_forcedDescriptorHeapResult = result;
    }

    HRESULT OnDeviceCreation(INT64 timestamp, const DirectX::Etw::DeviceInfo* device, _Out_ UINT64* objectId) override
    {
        *objectId = ++m_nextObjectId;
        Devices.push_back({ timestamp, *device, *objectId });
        return S_OK;
    }

    HRESULT OnDescriptorHeapCreation(
        INT64 timestamp,
        UINT64 deviceId,
        UINT32 processId,
        UINT32 threadId,
        const D3D12_DESCRIPTOR_HEAP_DESC* desc,
        const DirectX::Etw::ObjectPlacementInfo* placement,
        _Out_ UINT64* objectId) override
    {
        *objectId = ++m_nextObjectId;

        const HRESULT forced = m_forcedDescriptorHeapResult.exchange(S_OK);
        if (FAILED(forced))
        {
            return forced;
        }

        DescriptorHeaps.push_back({ timestamp, deviceId, processId, threadId, *desc, *placement, *objectId });
        return S_OK;
    }

    HRESULT OnCommittedResourceCreation(
        INT64 timestamp,
        UINT64 deviceId,
        UINT32 processId,
        UINT32 threadId,
        const D3D12_RESOURCE_DESC* desc,
        const DirectX::Etw::ObjectPlacementInfo* placement,
        const D3D12_HEAP_PROPERTIES* heapProperties,
        D3D12_HEAP_FLAGS heapFlags,
        _Out_ UINT64* objectId) override
    {
        *objectId = ++m_nextObjectId;
        CommittedResources.push_back({ timestamp, deviceId, processId, threadId, *desc, *placement, *heapProperties, heapFlags, *objectId });
        return S_OK;
    }

    HRESULT OnPlacedResourceCreation(
        INT64 timestamp,
        UINT64 deviceId,
        UINT32 processId,
        UINT32 threadId,
        const D3D12_RESOURCE_DESC* desc,
        const DirectX::Etw::ObjectPlacementInfo* placement,
        _Out_ UINT64* objectId) override
    {
        *objectId = ++m_nextObjectId;
        PlacedResources.push_back({ timestamp, deviceId, processId, threadId, *desc, *placement, *objectId });
        return S_OK;
    }

    HRESULT OnReservedResourceCreation(
        INT64 timestamp,
        UINT64 deviceId,
        UINT32 processId,
        UINT32 threadId,
        const D3D12_RESOURCE_DESC* desc,
        const DirectX::Etw::ReservedResourceInfo* reserved,
        _Out_ UINT64* objectId) override
    {
        *objectId = ++m_nextObjectId;
        ReservedResources.push_back({ timestamp, deviceId, processId, threadId, *desc, *reserved, *objectId });
        return S_OK;
    }

    HRESULT OnHeapCreation(
        INT64 timestamp,
        UINT64 deviceId,
        UINT32 processId,
        UINT32 threadId,
        const D3D12_HEAP_DESC* desc,
        const DirectX::Etw::ObjectPlacementInfo* placement,
        _Out_ UINT64* objectId) override
    {
        *objectId = ++m_nextObjectId;
        Heaps.push_back({ timestamp, deviceId, processId, threadId, *desc, *placement, *objectId });
        return S_OK;
    }

    HRESULT OnPipelineStateCreation(
        INT64 timestamp,
        UINT64 deviceId,
        UINT32 processId,
        UINT32 threadId,
        const DirectX::Etw::ObjectPlacementInfo* placement,
        _Out_ UINT64* objectId) override
    {
        *objectId = ++m_nextObjectId;
        PipelineStates.push_back({ timestamp, deviceId, processId, threadId, *placement, *objectId });
        return S_OK;
    }

    HRESULT OnStateObjectCreation(
        INT64 timestamp,
        UINT64 deviceId,
        UINT32 processId,
        UINT32 threadId,
        const DirectX::Etw::ObjectPlacementInfo* placement,
        _Out_ UINT64* objectId) override
    {
        *objectId = ++m_nextObjectId;
        StateObjects.push_back({ timestamp, deviceId, processId, threadId, *placement, *objectId });
        return S_OK;
    }

    HRESULT OnCommandAllocatorCreation(
        INT64 timestamp,
        UINT64 deviceId,
        UINT32 processId,
        UINT32 threadId,
        D3D12_COMMAND_LIST_TYPE commandListType,
        const DirectX::Etw::ObjectPlacementInfo* placement,
        _Out_ UINT64* objectId) override
    {
        *objectId = ++m_nextObjectId;
        CommandAllocators.push_back({ timestamp, deviceId, processId, threadId, commandListType, *placement, *objectId });
        return S_OK;
    }

    HRESULT OnMetaCommandCreation(
        INT64 timestamp,
        UINT64 deviceId,
        UINT32 processId,
        UINT32 threadId,
        REFGUID commandId,
        const DirectX::Etw::ObjectPlacementInfo* placement,
        _Out_ UINT64* objectId) override
    {
        *objectId = ++m_nextObjectId;
        MetaCommands.push_back({ timestamp, deviceId, processId, threadId, commandId, *placement, *objectId });
        return S_OK;
    }

    HRESULT OnObjectDestruction(INT64 timestamp, DirectX::Etw::ApiObjectType type, UINT64 objectId) override
    {
        Destructions.push_back({ timestamp, type, objectId });
        return S_OK;
    }

    HRESULT OnApiObjectName(INT64 timestamp, DirectX::Etw::ApiObjectType type, UINT64 objectId, std::wstring_view name) override
    {
        NameUpdates.push_back({ timestamp, type, objectId, std::wstring{ name } });
        return S_OK;
    }

    HRESULT OnApiObjectSizeAndAddress(INT64 timestamp, DirectX::Etw::ApiObjectType type, UINT64 objectId, UINT64 allocationSize, UINT64 baseAddress) override
    {
        SizeAndAddressUpdates.push_back({ timestamp, type, objectId, allocationSize, baseAddress });
        return S_OK;
    }

private:
    std::atomic<UINT64> m_nextObjectId{ 1 };
    std::atomic<HRESULT> m_forcedDescriptorHeapResult{ S_OK };
};
