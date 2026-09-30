// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#pragma once

// No-op base implementations of the callback interfaces.
// These are used if you don't provide your own implementation of a given callback interface.

#include <atomic>

#include <DxTimingCaptureLibrary/EtwCallbacks.h>

namespace DirectX::Etw
{

class NoOpApiObjectCallbacks : public ApiObjectCallbacks
{
    std::atomic<UINT64> m_nextObjectId{ 1 };

public:
    HRESULT OnDeviceCreation(INT64, const DeviceInfo*, UINT64* objectId) override { *objectId = ++m_nextObjectId; return S_OK; }
    HRESULT OnDescriptorHeapCreation(INT64, UINT64, UINT32, UINT32, const D3D12_DESCRIPTOR_HEAP_DESC*, const ObjectPlacementInfo*, UINT64* objectId) override { *objectId = ++m_nextObjectId; return S_OK; }
    HRESULT OnCommittedResourceCreation(INT64, UINT64, UINT32, UINT32, const D3D12_RESOURCE_DESC*, const ObjectPlacementInfo*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS, UINT64* objectId) override { *objectId = ++m_nextObjectId; return S_OK; }
    HRESULT OnPlacedResourceCreation(INT64, UINT64, UINT32, UINT32, const D3D12_RESOURCE_DESC*, const ObjectPlacementInfo*, UINT64* objectId) override { *objectId = ++m_nextObjectId; return S_OK; }
    HRESULT OnReservedResourceCreation(INT64, UINT64, UINT32, UINT32, const D3D12_RESOURCE_DESC*, const ReservedResourceInfo*, UINT64* objectId) override { *objectId = ++m_nextObjectId; return S_OK; }
    HRESULT OnHeapCreation(INT64, UINT64, UINT32, UINT32, const D3D12_HEAP_DESC*, const ObjectPlacementInfo*, UINT64* objectId) override { *objectId = ++m_nextObjectId; return S_OK; }
    HRESULT OnPipelineStateCreation(INT64, UINT64, UINT32, UINT32, const ObjectPlacementInfo*, UINT64* objectId) override { *objectId = ++m_nextObjectId; return S_OK; }
    HRESULT OnStateObjectCreation(INT64, UINT64, UINT32, UINT32, const ObjectPlacementInfo*, UINT64* objectId) override { *objectId = ++m_nextObjectId; return S_OK; }
    HRESULT OnCommandAllocatorCreation(INT64, UINT64, UINT32, UINT32, D3D12_COMMAND_LIST_TYPE, const ObjectPlacementInfo*, UINT64* objectId) override { *objectId = ++m_nextObjectId; return S_OK; }
    HRESULT OnMetaCommandCreation(INT64, UINT64, UINT32, UINT32, REFGUID, const ObjectPlacementInfo*, UINT64* objectId) override { *objectId = ++m_nextObjectId; return S_OK; }
    HRESULT OnObjectDestruction(INT64, ApiObjectType, UINT64) override { return S_OK; }
    HRESULT OnApiObjectName(INT64, ApiObjectType, UINT64, std::wstring_view) override { return S_OK; }
    HRESULT OnApiObjectSizeAndAddress(INT64, ApiObjectType, UINT64, UINT64, UINT64) override { return S_OK; }
};

class NoOpPixCounterCallbacks : public PixCounterCallbacks
{
    std::atomic<UINT64> m_nextId{ 1 };

public:
    HRESULT OnPixCounterInfo(UINT64, UINT32, PCWSTR, PCWSTR, PCWSTR, CounterFlags, double, double, UINT64* counterId) override { *counterId = ++m_nextId; return S_OK; }
    HRESULT OnPixCounterInfoWithDefinition(UINT64, UINT32, PCWSTR, PCWSTR, PCWSTR, PCWSTR, CounterFlags, double, double, UINT64* counterId) override { *counterId = ++m_nextId; return S_OK; }
    HRESULT OnPixCounterBudget(UINT64, UINT64, CounterBudgetType, CounterBudgetSource) override { return S_OK; }
    HRESULT OnPixCounterData(const CounterDataPoint*, UINT32) override { return S_OK; }
    HRESULT OnPixCounterGroup(UINT64, PCWSTR, PCWSTR, UINT64* groupId) override { *groupId = ++m_nextId; return S_OK; }
};

class NoOpResidencyEventCallbacks : public ResidencyEventCallbacks
{
public:
    HRESULT OnResidencyOperation(const ResidencyOperation*) override { return S_OK; }
    HRESULT OnDemotedAllocations(const DemotedAllocation*, UINT32) override { return S_OK; }
    HRESULT OnAllocationMigrations(const AllocationMigration*, UINT32) override { return S_OK; }
};

class NoOpPixEventCallbacks : public PixEventCallbacks
{
    std::atomic<UINT64> m_nextEventId{ 1 };

public:
    HRESULT OnPixEvents(UINT32, UINT32, const PixCpuEvent*, UINT64* pContextIds, UINT32 count) override
    {
        for (UINT32 i = 0; i < count; ++i)
        {
            pContextIds[i] = ++m_nextEventId;
        }
        return S_OK;
    }
    HRESULT OnPixGpuEvents(const GpuEvent*, UINT32) override { return S_OK; }
    HRESULT FinalizeGpuEventsForCpuEventIds(const UINT64*, UINT32) override { return S_OK; }
    HRESULT OnMemoryEvent(const PixMemoryEvent*) override { return S_OK; }
};

class NoOpMonitorEventCallbacks : public MonitorEventCallbacks
{
    std::atomic<UINT64> m_nextMonitorId{ 1 };

public:
    HRESULT OnMonitor(wchar_t const*, wchar_t const*, UINT64* monitorId) override { *monitorId = ++m_nextMonitorId; return S_OK; }
    HRESULT OnMonitorUpdate(UINT64, wchar_t const*, wchar_t const*) override { return S_OK; }
    HRESULT OnVSync(UINT64, INT64) override { return S_OK; }
};

class NoOpGpuTimingsCallbacks : public GpuTimingsCallbacks
{
    std::atomic<UINT64> m_nextExecutionId{ 1 };
    std::atomic<UINT64> m_nextMarkerId{ 1 };

public:
    HRESULT OnGpuExecutionBegin(UINT32, UINT32, UINT64, INT64, UINT64, UINT64* gpuExecutionId) override { *gpuExecutionId = ++m_nextExecutionId; return S_OK; }
    HRESULT OnGpuExecutionComplete(UINT64) override { return S_OK; }
    HRESULT OnGpuWork(UINT64, UINT64, UINT32, UINT64, INT64, INT64) override { return S_OK; }
    HRESULT OnApiMarker(UINT32, UINT32, PCWSTR, INT64, UINT64* apiMarkerId) override { *apiMarkerId = ++m_nextMarkerId; return S_OK; }
    HRESULT OnCommandListName(UINT64, UINT32, PCWSTR) override { return S_OK; }
};

class NoOpDxgkObjectCallbacks : public DxgkObjectCallbacks
{
    std::atomic<UINT64> m_nextId{ 1 };

public:
    HRESULT OnHardwareAdapter(UINT64* adapterId) override { *adapterId = ++m_nextId; return S_OK; }
    HRESULT OnHardwareAdapterName(UINT64, PCWSTR) override { return S_OK; }
    HRESULT OnHardwareCommandQueue(UINT64, UINT64* hardwareCommandQueueId) override { *hardwareCommandQueueId = ++m_nextId; return S_OK; }
    HRESULT OnHardwareCommandQueueName(UINT64, PCWSTR) override { return S_OK; }
    HRESULT OnApiCommandQueue(UINT32, UINT64, PCWSTR, INT64, UINT64* apiCommandQueueId) override { *apiCommandQueueId = ++m_nextId; return S_OK; }
    HRESULT OnApiCommandQueueName(UINT64, PCWSTR) override { return S_OK; }
    HRESULT OnApiCommandQueueEnd(UINT64, INT64) override { return S_OK; }
};

class NoOpGpuEngineActivityCallbacks : public GpuEngineActivityCallbacks
{
public:
    HRESULT OnGpuEngineActivity(UINT32, UINT64, UINT32, INT64, INT64, bool) override { return S_OK; }
};

class NoOpPipelineStateEventCallbacks : public PipelineStateEventCallbacks
{
public:
    HRESULT OnPsoCompilation(UINT32, UINT32, UINT64, INT64, INT64) override { return S_OK; }
};

class NoOpRuntimeFailureCallbacks : public RuntimeFailureCallbacks
{
public:
    HRESULT OnD3D12JournalEntry(INT64, UINT32, UINT32, UINT32, std::string_view) override { return S_OK; }
};

class NoOpDirectStorageCallbacks : public DirectStorageCallbacks
{
public:
    HRESULT OnDirectStorageFile(UINT16, INT64, std::wstring_view) override { return S_OK; }
    HRESULT OnDirectStorageQueue(UINT16, INT64, std::string_view) override { return S_OK; }
    HRESULT OnDirectStorageReadRequest(const ReadRequest&) override { return S_OK; }
    HRESULT OnDirectStorageStatus(const StatusNotification&) override { return S_OK; }
    HRESULT OnDirectStorageFenceSignal(const FenceSignal&) override { return S_OK; }
    HRESULT OnDirectStorageSetEvent(const SetEventNotification&) override { return S_OK; }
    HRESULT OnDirectStorageSubmit(const Submit&) override { return S_OK; }
};

} // namespace DirectX::Etw