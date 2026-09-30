// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#pragma once

#include <atomic>

#include <DxTimingCaptureLibrary/DxTimingCaptureLibrary.h>

#include "MemoryModel.h"

namespace memorymap
{
    using namespace DirectX::Etw;

    // Turns the library's API-object callbacks into MemoryModel records. We only
    // keep heaps and resources; everything else still gets a unique id (the
    // library needs one back) but is otherwise ignored.
    class ApiObjectSink : public ApiObjectCallbacks
    {
    public:
        explicit ApiObjectSink(MemoryModel& model) : m_model(model) {}

        HRESULT OnDeviceCreation(INT64, const DeviceInfo*, _Out_ UINT64* objectId) override
        {
            *objectId = NewId();
            return S_OK;
        }

        HRESULT OnDescriptorHeapCreation(INT64, UINT64, UINT32, UINT32, const D3D12_DESCRIPTOR_HEAP_DESC*, const ObjectPlacementInfo*, _Out_ UINT64* objectId) override
        {
            *objectId = NewId();
            return S_OK;
        }

        HRESULT OnCommittedResourceCreation(INT64, UINT64, UINT32, UINT32, const D3D12_RESOURCE_DESC*, const ObjectPlacementInfo* placement, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS, _Out_ UINT64* objectId) override
        {
            *objectId = AddResource(ObjectKind::CommittedResource, placement);
            return S_OK;
        }

        HRESULT OnPlacedResourceCreation(INT64, UINT64, UINT32, UINT32, const D3D12_RESOURCE_DESC*, const ObjectPlacementInfo* placement, _Out_ UINT64* objectId) override
        {
            *objectId = AddResource(ObjectKind::PlacedResource, placement);
            return S_OK;
        }

        HRESULT OnReservedResourceCreation(INT64, UINT64, UINT32, UINT32, const D3D12_RESOURCE_DESC*, const ReservedResourceInfo*, _Out_ UINT64* objectId) override
        {
            // Reserved (tiled) resources have no backing of their own at creation:
            // their tiles are mapped into one or more heaps later, and can span
            // pools, so we don't assign a single size/address or segment group.
            ObjectRecord record;
            record.Id = NewId();
            record.Kind = ObjectKind::ReservedResource;
            m_model.Add(record);
            *objectId = record.Id;
            return S_OK;
        }

        HRESULT OnHeapCreation(INT64, UINT64, UINT32, UINT32, const D3D12_HEAP_DESC* desc, const ObjectPlacementInfo* placement, _Out_ UINT64* objectId) override
        {
            ObjectRecord record;
            record.Id = NewId();
            record.Kind = ObjectKind::Heap;
            record.Size = desc ? desc->SizeInBytes : (placement ? placement->GpuVirtualSize : 0);
            if (placement)
            {
                record.BaseAddress = placement->GpuVirtualAddress;
                record.Group = placement->ResidentSegmentGroup;
            }
            m_model.Add(record);
            *objectId = record.Id;
            return S_OK;
        }

        HRESULT OnPipelineStateCreation(INT64, UINT64, UINT32, UINT32, const ObjectPlacementInfo*, _Out_ UINT64* objectId) override
        {
            *objectId = NewId();
            return S_OK;
        }

        HRESULT OnStateObjectCreation(INT64, UINT64, UINT32, UINT32, const ObjectPlacementInfo*, _Out_ UINT64* objectId) override
        {
            *objectId = NewId();
            return S_OK;
        }

        HRESULT OnCommandAllocatorCreation(INT64, UINT64, UINT32, UINT32, D3D12_COMMAND_LIST_TYPE, const ObjectPlacementInfo*, _Out_ UINT64* objectId) override
        {
            *objectId = NewId();
            return S_OK;
        }

        HRESULT OnMetaCommandCreation(INT64, UINT64, UINT32, UINT32, REFGUID, const ObjectPlacementInfo*, _Out_ UINT64* objectId) override
        {
            *objectId = NewId();
            return S_OK;
        }

        HRESULT OnObjectDestruction(INT64, ApiObjectType, UINT64 objectId) override
        {
            m_model.Remove(objectId);
            return S_OK;
        }

        HRESULT OnApiObjectName(INT64, ApiObjectType, UINT64 objectId, std::wstring_view name) override
        {
            m_model.SetName(objectId, name);
            return S_OK;
        }

        HRESULT OnApiObjectSizeAndAddress(INT64, ApiObjectType, UINT64 objectId, UINT64 allocationSize, UINT64 baseAddress) override
        {
            m_model.SetSizeAndAddress(objectId, allocationSize, baseAddress);
            return S_OK;
        }

    private:
        uint64_t NewId() { return m_nextId.fetch_add(1); }

        uint64_t AddResource(ObjectKind kind, const ObjectPlacementInfo* placement)
        {
            ObjectRecord record;
            record.Id = NewId();
            record.Kind = kind;
            if (placement)
            {
                record.Size = placement->GpuVirtualSize;
                record.BaseAddress = placement->GpuVirtualAddress;
                record.Group = placement->ResidentSegmentGroup;
            }
            m_model.Add(record);
            return record.Id;
        }

        MemoryModel& m_model;
        std::atomic<uint64_t> m_nextId{ 1 };
    };

    // The migration signal: an object's backing allocation moved between video
    // and system memory. Placed resources follow their heap, so updating the
    // heap here is enough to move all of its contents at render time.
    class ResidencySink : public ResidencyEventCallbacks
    {
    public:
        explicit ResidencySink(MemoryModel& model) : m_model(model) {}

        HRESULT OnResidencyOperation(const ResidencyOperation*) override { return S_OK; }
        HRESULT OnDemotedAllocations(const DemotedAllocation*, UINT32) override { return S_OK; }
        HRESULT OnAllocationMigrations(const AllocationMigration*, UINT32) override { return S_OK; }

        HRESULT OnAllocationSegmentGroupChanges(const AllocationSegmentGroupChange* data, UINT32 count) override
        {
            for (UINT32 i = 0; i < count; ++i)
            {
                m_model.SetGroup(data[i].ObjectId, data[i].SegmentGroup);
            }
            return S_OK;
        }

    private:
        MemoryModel& m_model;
    };
}
