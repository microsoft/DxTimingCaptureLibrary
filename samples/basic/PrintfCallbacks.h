// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#pragma once

#define PRINT_MONITOR_CALLBACKS 0
#define PRINT_API_OBJECT_CALLBACKS 1
#define PRINT_DXGK_OBJECT_CALLBACKS 0
#define PRINT_PIX_COUNTER_CALLBACKS 0
#define PRINT_RESIDENCY_CALLBACKS 1

using namespace DirectX::Etw;

class MonitorCallbacksPrintf : public MonitorEventCallbacks
{
    std::atomic<UINT64> m_nextMonitorId{ 1 };

    int MaybePrint(const wchar_t* format, ...)
    {
        int result = 0;

#if PRINT_MONITOR_CALLBACKS
        va_list args;
        va_start(args, format);
        result = vwprintf(format, args);
        va_end(args);
#endif

        return result;
    }

public:
    HRESULT OnMonitor(wchar_t const* monitorDescription, wchar_t const* adapterDescription, UINT64* monitorId) override
    {
        *monitorId = ++m_nextMonitorId;
        MaybePrint(L"Monitor found: %ls on %ls\n", monitorDescription, adapterDescription);
        return S_OK;
    }

    HRESULT OnMonitorUpdate(UINT64 monitorId, wchar_t const* monitorDescription, wchar_t const* adapterDescription) override
    {
        MaybePrint(L"Monitor updated: %ls on %ls\n", monitorDescription, adapterDescription);
        return S_OK;
    }

    HRESULT OnVSync(UINT64 monitorId, INT64 timestamp) override
    {
        MaybePrint(L"VSYNC: %llu, timestamp %lld\n", monitorId, timestamp);
        return S_OK;
    }
};

class ApiObjectCallbacksPrintf : public ApiObjectCallbacks
{
    std::atomic<UINT64> m_nextObjectId{ 1 };

    int MaybePrint(const wchar_t* format, ...)
    {
        int result = 0;

#if PRINT_API_OBJECT_CALLBACKS
        va_list args;
        va_start(args, format);
        result = vwprintf(format, args);
        va_end(args);
#endif

        return result;
    }

public:
    HRESULT OnDeviceCreation(INT64 timestamp, const DeviceInfo* device, _Out_ UINT64* objectId) override
    {
        *objectId = ++m_nextObjectId;
        MaybePrint(L"%lld Device reported: FeatureLevel 0x%X\n", timestamp, device->FeatureLevel);
        return S_OK;
    }

    HRESULT OnDescriptorHeapCreation(
        INT64 timestamp,
        UINT64 deviceId,
        UINT32 processId,
        UINT32 threadId,
        const D3D12_DESCRIPTOR_HEAP_DESC* desc,
        const ObjectPlacementInfo* placement,
        _Out_ UINT64* objectId) override
    {
        *objectId = ++m_nextObjectId;
        MaybePrint(L"%lld DescriptorHeap reported: Type %d, Count %d\n", timestamp, (int)desc->Type, desc->NumDescriptors);
        return S_OK;
    }

    HRESULT OnCommittedResourceCreation(
        INT64 timestamp,
        UINT64 deviceId,
        UINT32 processId,
        UINT32 threadId,
        const D3D12_RESOURCE_DESC* desc,
        const ObjectPlacementInfo* placement,
        const D3D12_HEAP_PROPERTIES* heapProperties,
        D3D12_HEAP_FLAGS heapFlags,
        _Out_ UINT64* objectId) override
    {
        *objectId = ++m_nextObjectId;
        MaybePrint(L"%lld Committed resource reported: Width %llu, Size %llu, HeapType %d\n", timestamp, desc->Width, placement->GpuVirtualSize, (int)heapProperties->Type);
        return S_OK;
    }

    HRESULT OnPlacedResourceCreation(
        INT64 timestamp,
        UINT64 deviceId,
        UINT32 processId,
        UINT32 threadId,
        const D3D12_RESOURCE_DESC* desc,
        const ObjectPlacementInfo* placement,
        _Out_ UINT64* objectId) override
    {
        *objectId = ++m_nextObjectId;
        MaybePrint(L"%lld Placed resource reported: Width %llu, Size %llu\n", timestamp, desc->Width, placement->GpuVirtualSize);
        return S_OK;
    }

    HRESULT OnReservedResourceCreation(
        INT64 timestamp,
        UINT64 deviceId,
        UINT32 processId,
        UINT32 threadId,
        const D3D12_RESOURCE_DESC* desc,
        const ReservedResourceInfo* reserved,
        _Out_ UINT64* objectId) override
    {
        *objectId = ++m_nextObjectId;
        MaybePrint(L"%lld Reserved resource reported: Width %llu, Tiles %u\n", timestamp, desc->Width, reserved->NumTilesForResource);
        return S_OK;
    }

    HRESULT OnHeapCreation(
        INT64 timestamp,
        UINT64 deviceId,
        UINT32 processId,
        UINT32 threadId,
        const D3D12_HEAP_DESC* desc,
        const ObjectPlacementInfo* placement,
        _Out_ UINT64* objectId) override
    {
        *objectId = ++m_nextObjectId;
        MaybePrint(L"%lld Heap reported: Size %llu\n", timestamp, placement->GpuVirtualSize);
        return S_OK;
    }

    HRESULT OnPipelineStateCreation(
        INT64 timestamp,
        UINT64 deviceId,
        UINT32 processId,
        UINT32 threadId,
        const ObjectPlacementInfo* placement,
        _Out_ UINT64* objectId) override
    {
        *objectId = ++m_nextObjectId;
        MaybePrint(L"%lld PipelineState reported: Size %llu\n", timestamp, placement->GpuVirtualSize);
        return S_OK;
    }

    HRESULT OnStateObjectCreation(
        INT64 timestamp,
        UINT64 deviceId,
        UINT32 processId,
        UINT32 threadId,
        const ObjectPlacementInfo* placement,
        _Out_ UINT64* objectId) override
    {
        *objectId = ++m_nextObjectId;
        MaybePrint(L"%lld StateObject reported: Size %llu\n", timestamp, placement->GpuVirtualSize);
        return S_OK;
    }

    HRESULT OnCommandAllocatorCreation(
        INT64 timestamp,
        UINT64 deviceId,
        UINT32 processId,
        UINT32 threadId,
        D3D12_COMMAND_LIST_TYPE commandListType,
        const ObjectPlacementInfo* placement,
        _Out_ UINT64* objectId) override
    {
        *objectId = ++m_nextObjectId;
        MaybePrint(L"%lld CommandAllocator reported: Type %d, Size %llu\n", timestamp, (int)commandListType, placement->GpuVirtualSize);
        return S_OK;
    }

    HRESULT OnMetaCommandCreation(
        INT64 timestamp,
        UINT64 deviceId,
        UINT32 processId,
        UINT32 threadId,
        REFGUID commandId,
        const ObjectPlacementInfo* placement,
        _Out_ UINT64* objectId) override
    {
        *objectId = ++m_nextObjectId;
        MaybePrint(L"%lld MetaCommand reported: GUID %08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X, Size %llu\n", timestamp,
            commandId.Data1,
            commandId.Data2,
            commandId.Data3,
            commandId.Data4[0],
            commandId.Data4[1],
            commandId.Data4[2],
            commandId.Data4[3],
            commandId.Data4[4],
            commandId.Data4[5],
            commandId.Data4[6],
            commandId.Data4[7],
            placement->GpuVirtualSize);
        return S_OK;
    }

    HRESULT OnObjectDestruction(INT64 timestamp, ApiObjectType type, UINT64 objectId) override
    {
        MaybePrint(L"%lld Object destroyed: Type %d, Id %llu\n", timestamp, (int)type, objectId);
        return S_OK;
    }

    HRESULT OnApiObjectName(INT64 timestamp, ApiObjectType type, UINT64 objectId, std::wstring_view name) override
    {
        MaybePrint(L"%lld Object name updated: Type %d, Id %llu, Name %ls\n", timestamp, (int)type, objectId, std::wstring(name).c_str());
        return S_OK;
    }

    HRESULT OnApiObjectSizeAndAddress(INT64 timestamp, ApiObjectType type, UINT64 objectId, UINT64 allocationSize, UINT64 baseAddress) override
    {
        MaybePrint(L"%lld Object size and address updated: Type %d, Id %llu, AllocationSize %llu, BaseAddress %llu\n", timestamp, (int)type, objectId, allocationSize, baseAddress);
        return S_OK;
    }
};

class DxgkObjectCallbacksPrintf : public DxgkObjectCallbacks
{
    std::atomic<UINT64> m_nextAdapterId{ 1 };
    std::atomic<UINT64> m_nextHardwareCommandQueueId{ 1 };
    std::atomic<UINT64> m_nextApiCommandQueueId{ 1 };

    int MaybePrint(const wchar_t* format, ...)
    {
        int result = 0;

#if PRINT_DXGK_OBJECT_CALLBACKS
        va_list args;
        va_start(args, format);
        result = vwprintf(format, args);
        va_end(args);
#endif

        return result;
    }

public:
    HRESULT OnHardwareAdapter(UINT64* adapterId) override
    {
        *adapterId = ++m_nextAdapterId;
        MaybePrint(L"Hardware Adapter defined: Id %llu\n", *adapterId);
        return S_OK;
    }

    HRESULT OnHardwareAdapterName(UINT64 adapterId, PCWSTR name) override
    {
        MaybePrint(L"Hardware Adapter updated: Id %llu, Name %ls\n", adapterId, name);
        return S_OK;
    }

    HRESULT OnHardwareCommandQueue(UINT64 adapterId, UINT64* hardwareCommandQueueId) override
    {
        *hardwareCommandQueueId = ++m_nextHardwareCommandQueueId;
        MaybePrint(L"Hardware Command Queue defined: AdapterId %llu, CommandQueueId %llu\n", adapterId, *hardwareCommandQueueId);
        return S_OK;
    }

    HRESULT OnHardwareCommandQueueName(UINT64 hardwareCommandQueueId, PCWSTR name) override
    {
        MaybePrint(L"Hardware Command Queue updated: CommandQueueId %llu, Name %ls\n", hardwareCommandQueueId, name);
        return S_OK;
    }

    HRESULT OnApiCommandQueue(
        UINT32 processId,
        UINT64 hardwareAdapterId,
        PCWSTR commandQueueType,
        INT64 beginTimestamp,
        UINT64* apiCommandQueueId) override
    {
        *apiCommandQueueId = ++m_nextApiCommandQueueId;
        MaybePrint(L"API Command Queue defined: ProcessId %u, HardwareAdapterId %llu, Type %ls, CommandQueueId %llu\n",
            processId,
            hardwareAdapterId,
            commandQueueType,
            *apiCommandQueueId);
        return S_OK;
    }

    HRESULT OnApiCommandQueueName(UINT64 apiCommandQueueId, PCWSTR name) override
    {
        MaybePrint(L"API Command Queue updated: CommandQueueId %llu, Name %ls\n", apiCommandQueueId, name);
        return S_OK;
    }

    HRESULT OnApiCommandQueueEnd(UINT64 apiCommandQueueId, INT64 endTimestamp) override
    {
        MaybePrint(L"API Command Queue ended: CommandQueueId %llu, EndTimestamp %lld\n", apiCommandQueueId, endTimestamp);
        return S_OK;
    }
};

class PixCounterCallbacksPrintf : public PixCounterCallbacks
{
    std::atomic<UINT64> m_nextCounterId{ 1 };
    std::atomic<UINT64> m_nextGroupId{ 1 };

    int MaybePrint(const wchar_t* format, ...)
    {
        int result = 0;

#if PRINT_PIX_COUNTER_CALLBACKS
        va_list args;
        va_start(args, format);
        result = vwprintf(format, args);
        va_end(args);
#endif

        return result;
    }

public:
    HRESULT OnPixCounterInfo(
        UINT64 groupId,
        UINT32 processId,
        PCWSTR name,
        PCWSTR description,
        PCWSTR units,
        CounterFlags flags,
        double min,
        double max,
        UINT64* counterId) override
    {
        *counterId = ++m_nextCounterId;
        MaybePrint(L"Pix Counter Info stored: GroupId %llu, ProcessId %u, Name %ls, Description %ls, Units %ls, CounterId %llu\n",
            groupId,
            processId,
            name,
            description,
            units,
            *counterId);
        return S_OK;
    }

    HRESULT OnPixCounterInfoWithDefinition(
        UINT64 groupId,
        UINT32 processId,
        PCWSTR name,
        PCWSTR description,
        PCWSTR counterDefinition,
        PCWSTR units,
        CounterFlags flags,
        double min,
        double max,
        UINT64* counterId) override
    {
        *counterId = ++m_nextCounterId;
        MaybePrint(L"Pix Counter Info with Definition stored: GroupId %llu, ProcessId %u, Name %ls, Description %ls, Definition %ls, Units %ls, CounterId %llu\n",
            groupId,
            processId,
            name,
            description,
            counterDefinition,
            units,
            *counterId);
        return S_OK;
    }

    HRESULT OnPixCounterBudget(UINT64 counterId, UINT64 budgetCounterId, CounterBudgetType budgetType, CounterBudgetSource budgetSource) override
    {
        MaybePrint(L"Pix Counter Budget stored: CounterId %llu, BudgetCounterId %llu, BudgetType %d, BudgetSource %d\n",
            counterId,
            budgetCounterId,
            (int)budgetType,
            (int)budgetSource);
        return S_OK;
    }

    HRESULT OnPixCounterData(const _In_count_(count) CounterDataPoint* dataPoints, UINT32 count) override
    {
        for (auto i = 0u; i < count; ++i)
        {
            MaybePrint(L"Pix Counter Data Point stored: CounterId %llu, Timestamp %lld, Value %f\n",
                dataPoints[i].CounterId,
                dataPoints[i].Timestamp,
                dataPoints[i].Value);
        }
        return S_OK;
    }

    HRESULT OnPixCounterGroup(UINT64 parentGroupId, PCWSTR name, PCWSTR description, UINT64* groupId) override
    {
        *groupId = ++m_nextGroupId;
        MaybePrint(L"Pix Counter Group stored: ParentGroupId %llu, Name %ls, Description %ls, GroupId %llu\n",
            parentGroupId,
            name,
            description,
            *groupId);
        return S_OK;
    }
};

class ResidencyEventCallbacksPrintf : public ResidencyEventCallbacks
{
    int MaybePrint(const wchar_t* format, ...)
    {
        int result = 0;

#if PRINT_RESIDENCY_CALLBACKS
        va_list args;
        va_start(args, format);
        result = vwprintf(format, args);
        va_end(args);
#endif

        return result;
    }

public:
    HRESULT OnResidencyOperation(const ResidencyOperation* data) override
    {
        MaybePrint(L"%lld Residence Operation: ObjectId %llu, ResidencyCount %u, OperationType %d, ObjectType %d\n",
            data->Timestamp,
            data->ObjectId,
            data->ResidencyCount,
            (int)data->OperationType,
            (int)data->ObjectType);

        return S_OK;
    }

    HRESULT OnDemotedAllocations(const DemotedAllocation* data, UINT32 count) override
    {
        for (auto i = 0u; i < count; ++i)
        {
            MaybePrint(L"%lld Demoted Allocation: ObjectId %llu\n",
                data[i].Timestamp,
                data[i].ObjectId);
        }

        return S_OK;
    }

    HRESULT OnAllocationMigrations(const AllocationMigration* data, UINT32 count) override
    {
        for (auto i = 0u; i < count; ++i)
        {
            MaybePrint(L"%lld-%lld Allocation Migration: ObjectId %llu, Result %d\n",
                data[i].StartTime,
                data[i].EndTime,
                data[i].ObjectId,
                (int)data[i].Result);
        }

        return S_OK;
    }
};

// The remaining three callback interfaces are required by the handler even
// though this sample does not capture GPU timing, so provide minimal
// implementations that hand back monotonically increasing ids.
class PixEventCallbacksPrintf : public PixEventCallbacks
{
    std::atomic<UINT64> m_nextEventId{ 1 };

public:
    HRESULT OnPixEvents(UINT32 processId, UINT32 threadId, const PixCpuEvent* pEvents, UINT64* pContextIds, UINT32 count) override
    {
        for (UINT32 i = 0; i < count; ++i)
        {
            pContextIds[i] = ++m_nextEventId;
        }
        return S_OK;
    }

    HRESULT OnPixGpuEvents(const GpuEvent* pGpuEvents, UINT32 count) override { return S_OK; }
    HRESULT FinalizeGpuEventsForCpuEventIds(const UINT64* pEventIds, UINT32 count) override { return S_OK; }
    HRESULT OnMemoryEvent(const PixMemoryEvent* data) override { return S_OK; }
};

class GpuTimingsCallbacksPrintf : public GpuTimingsCallbacks
{
    std::atomic<UINT64> m_nextExecutionId{ 1 };
    std::atomic<UINT64> m_nextMarkerId{ 1 };

public:
    HRESULT OnGpuExecutionBegin(UINT32 processId, UINT32 threadId, UINT64 apiCommandQueueId, INT64 cpuSubmitTime, UINT64 presentToken, UINT64* gpuExecutionId) override
    {
        *gpuExecutionId = ++m_nextExecutionId;
        return S_OK;
    }

    HRESULT OnGpuExecutionComplete(UINT64 gpuExecutionId) override { return S_OK; }

    HRESULT OnGpuWork(UINT64 hardwareCommandQueueId, UINT64 gpuExecutionId, UINT32 commandListIndexInExecution, UINT64 apiMarkerId, INT64 beginTimestamp, INT64 endTimestamp) override
    {
        return S_OK;
    }

    HRESULT OnApiMarker(UINT32 processId, UINT32 threadId, PCWSTR name, INT64 timeStamp, UINT64* apiMarkerId) override
    {
        *apiMarkerId = ++m_nextMarkerId;
        return S_OK;
    }

    HRESULT OnCommandListName(UINT64 gpuExecutionId, UINT32 commandListIndexInExecution, PCWSTR name) override
    {
        return S_OK;
    }
};

class PipelineStateEventCallbacksPrintf : public PipelineStateEventCallbacks
{
public:
    HRESULT OnPsoCompilation(UINT32 processId, UINT32 threadId, UINT64 apiObjectId, INT64 startTimestamp, INT64 endTimestamp) override
    {
        return S_OK;
    }
};

// An application embedding the library owns its console, so this sample sink
// simply prints the library's warnings and errors to stdout. A real integration
// would route them to its own logging instead.
class DiagnosticsSinkPrintf : public DiagnosticsSink
{
public:
    void OnDiagnostic(DiagnosticSeverity severity, DiagnosticCode code, std::wstring_view message) override
    {
        const wchar_t* severityText = severity == DiagnosticSeverity::Error ? L"ERROR" : L"WARNING";
        wprintf(L"[DxTimingCaptureLibrary %ls] (code %d) %.*ls\n", severityText, (int)code, (int)message.size(), message.data());
    }
};

// Prints every D3D12 runtime failure journal entry.
class RuntimeFailureCallbacksPrintf : public RuntimeFailureCallbacks
{
public:
    HRESULT OnD3D12JournalEntry(INT64 timestamp, UINT32 index, UINT32 code, UINT32 threadId, std::string_view message) override
    {
        printf("[D3D12 runtime failure] code=0x%08X thread=%u slot=%u: %.*s\n", code, threadId, index, (int)message.size(), message.data());
        return S_OK;
    }
};

// Prints correlated DirectStorage activity decoded from the DirectStorage ETW provider.
class DirectStorageCallbacksPrintf : public DirectStorageCallbacks
{
public:
    HRESULT OnDirectStorageFile(UINT16 fileId, INT64 timestamp, std::wstring_view path) override
    {
        wprintf(L"[DirectStorage] file id=%u: %.*ls\n", fileId, (int)path.size(), path.data());
        return S_OK;
    }

    HRESULT OnDirectStorageQueue(UINT16 queueId, INT64 timestamp, std::string_view name) override
    {
        printf("[DirectStorage] queue id=%u: %.*s\n", queueId, (int)name.size(), name.data());
        return S_OK;
    }

    HRESULT OnDirectStorageReadRequest(const ReadRequest& request) override
    {
        printf("[DirectStorage] read queue=%u file=%u offset=%llu size=%u compression=%u shuffle=%u enqueue=%lld completion=%lld\n",
            request.QueueId, request.FileId, (unsigned long long)request.Offset, request.Size,
            request.CompressionType, request.ShuffleType, (long long)request.EnqueueTimestamp, (long long)request.CompletionTimestamp);
        return S_OK;
    }

    HRESULT OnDirectStorageStatus(const StatusNotification& status) override
    {
        printf("[DirectStorage] status queue=%u array=0x%llX index=%llu enqueue=%lld completion=%lld\n",
            status.QueueId, (unsigned long long)status.StatusArray, (unsigned long long)status.Index,
            (long long)status.EnqueueTimestamp, (long long)status.CompletionTimestamp);
        return S_OK;
    }

    HRESULT OnDirectStorageFenceSignal(const FenceSignal& signal) override
    {
        printf("[DirectStorage] fence queue=%u fence=0x%llX value=%llu enqueue=%lld completion=%lld\n",
            signal.QueueId, (unsigned long long)signal.Fence, (unsigned long long)signal.Value,
            (long long)signal.EnqueueTimestamp, (long long)signal.CompletionTimestamp);
        return S_OK;
    }

    HRESULT OnDirectStorageSetEvent(const SetEventNotification& setEvent) override
    {
        printf("[DirectStorage] setevent queue=%u handle=0x%llX enqueue=%lld completion=%lld\n",
            setEvent.QueueId, (unsigned long long)setEvent.Handle,
            (long long)setEvent.EnqueueTimestamp, (long long)setEvent.CompletionTimestamp);
        return S_OK;
    }

    HRESULT OnDirectStorageSubmit(const Submit& submit) override
    {
        printf("[DirectStorage] submit queue=%u auto=%d timestamp=%lld\n",
            submit.QueueId, submit.AutoSubmitted ? 1 : 0, (long long)submit.Timestamp);
        return S_OK;
    }
};
