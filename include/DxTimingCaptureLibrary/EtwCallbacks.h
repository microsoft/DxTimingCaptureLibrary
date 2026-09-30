// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#pragma once

#include <string_view>

#include <DxTimingCaptureLibrary/Types.h>

namespace DirectX::Etw
{

// Callback interfaces that you implement, that the library will call into to tell
// you when relevant things are reported via ETW.
//
// Threading: all callbacks run synchronously on the thread that calls
// DxTimingCaptureEventHandler::HandleEventRecord/OnDataComplete. The library holds no
// locks. Of course, guard any storage shared with other threads yourself.
// 
// Methods with an _Out_ id parameter must set it on S_OK to a unique, stable,
// non-zero value (a monotonically increasing counter works), because the library
// feeds those ids into later callbacks.
//
// Return-value: return S_OK on success. A failure HRESULT is fatal - the
// library throws it out of HandleEventRecord / OnDataComplete (as a decode failure
// would) and it must not unwind through ETW.

class PixCounterCallbacks
{
public:
    struct CounterDataPoint
    {
        UINT64 CounterId;
        INT64 Timestamp; // Time (in ns) since the beginning of the capture session
        double Value;
    };

    virtual ~PixCounterCallbacks() = default;

    virtual HRESULT OnPixCounterInfo(
        UINT64 groupId,
        UINT32 processId,
        PCWSTR name,
        PCWSTR description,
        PCWSTR units,
        CounterFlags flags,
        double min,
        double max,
        _Out_ UINT64* counterId) = 0;

    virtual HRESULT OnPixCounterInfoWithDefinition(
        UINT64 groupId,
        UINT32 processId,
        PCWSTR name,
        PCWSTR description,
        PCWSTR counterDefinition,
        PCWSTR units,
        CounterFlags flags,
        double min,
        double max,
        _Out_ UINT64* counterId) = 0;

    virtual HRESULT OnPixCounterBudget(
        UINT64 counterId,
        UINT64 budgetCounterId,
        CounterBudgetType budgetType,
        CounterBudgetSource budgetSource) = 0;

    virtual HRESULT OnPixCounterData(
        const _In_count_(count) CounterDataPoint* dataPoints,
        UINT32 count) = 0;

    virtual HRESULT OnPixCounterGroup(
        UINT64 parentGroupId,
        PCWSTR name,
        PCWSTR description,
        _Out_ UINT64* groupId) = 0;
};

class ResidencyEventCallbacks
{
public:
    virtual ~ResidencyEventCallbacks() = default;

    virtual HRESULT OnResidencyOperation(const ResidencyOperation* data) = 0;
    virtual HRESULT OnDemotedAllocations(const DemotedAllocation* data, UINT32 count) = 0;
    virtual HRESULT OnAllocationMigrations(const AllocationMigration* data, UINT32 count) = 0;

    // One or more objects moved between video and system memory. Optional to
    // implement - defaults to a no-op so existing callers keep compiling.
    virtual HRESULT OnAllocationSegmentGroupChanges(const AllocationSegmentGroupChange* data, UINT32 count)
    {
        (void)data;
        (void)count;
        return S_OK;
    }
};

class PixEventCallbacks
{
public:
    virtual ~PixEventCallbacks() = default;

    virtual HRESULT OnPixEvents(
        UINT32 processId,
        UINT32 threadId,
        _In_count_(count) const PixCpuEvent* pEvents,
        _In_count_(count) UINT64* pContextIds,
        UINT32 count) = 0;

    virtual HRESULT OnPixGpuEvents(_In_count_(count) const GpuEvent* pGpuEvents, UINT32 count) = 0;
    virtual HRESULT FinalizeGpuEventsForCpuEventIds(_In_count_(count) const UINT64* pEventIds, UINT32 count) = 0;

    virtual HRESULT OnMemoryEvent(const PixMemoryEvent* data) = 0;
};

class MonitorEventCallbacks
{
public:
    virtual ~MonitorEventCallbacks() = default;

    virtual HRESULT OnMonitor(wchar_t const* monitorDescription, wchar_t const* adapterDescription, _Out_ UINT64* monitorId) = 0;
    virtual HRESULT OnMonitorUpdate(UINT64 monitorId, wchar_t const* monitorDescription, wchar_t const* adapterDescription) = 0;

    virtual HRESULT OnVSync(UINT64 monitorId, INT64 timestamp) = 0;
};

class GpuTimingsCallbacks
{
public:
    virtual ~GpuTimingsCallbacks() = default;

    virtual HRESULT OnGpuExecutionBegin(
        UINT32 processId,
        UINT32 threadId,
        UINT64 apiCommandQueueId,
        INT64 cpuSubmitTime,
        UINT64 presentToken,
        _Out_ UINT64* gpuExecutionId) = 0;

    virtual HRESULT OnGpuExecutionComplete(UINT64 gpuExecutionId) = 0;

    virtual HRESULT OnGpuWork(
        UINT64 hardwareCommandQueueId,
        UINT64 gpuExecutionId,
        UINT32 commandListIndexInExecution,
        UINT64 apiMarkerId,
        INT64 beginTimestamp,
        INT64 endTimestamp) = 0;

    virtual HRESULT OnApiMarker(UINT32 processId, UINT32 threadId, PCWSTR name, INT64 timeStamp, _Out_ UINT64* apiMarkerId) = 0;

    // Associates a friendly name (set via ID3D12Object::SetName) with a command list within a gpu execution.
    // The commandListIndexInExecution matches the value passed to OnGpuWork for the same command list.
    // Precondition: OnGpuWork must already have been called for this gpuExecutionId/commandListIndexInExecution
    // (and before OnGpuExecutionComplete). Naming a command list that recorded no work, or an unknown execution,
    // is treated as inconsistent capture data and fails with E_MISSING_REF_CAPTURE_DATA.
    virtual HRESULT OnCommandListName(UINT64 gpuExecutionId, UINT32 commandListIndexInExecution, PCWSTR name) = 0;
};

class DxgkObjectCallbacks
{
public:
    virtual ~DxgkObjectCallbacks() = default;

    virtual HRESULT OnHardwareAdapter(_Out_ UINT64* adapterId) = 0;
    virtual HRESULT OnHardwareAdapterName(UINT64 adapterId, PCWSTR name) = 0;

    virtual HRESULT OnHardwareCommandQueue(UINT64 adapterId, _Out_ UINT64* hardwareCommandQueueId) = 0;
    virtual HRESULT OnHardwareCommandQueueName(UINT64 hardwareCommandQueueId, PCWSTR name) = 0;

    virtual HRESULT OnApiCommandQueue(
        UINT32 processId,
        UINT64 hardwareAdapterId,
        PCWSTR commandQueueType,
        INT64 beginTimestamp,
        _Out_ UINT64* apiCommandQueueId) = 0;

    virtual HRESULT OnApiCommandQueueName(UINT64 apiCommandQueueId, PCWSTR name) = 0;
    virtual HRESULT OnApiCommandQueueEnd(UINT64 apiCommandQueueId, INT64 endTimestamp) = 0;
};

// What the GPU engines were doing, across every process on the machine rather
// than just the traced one. Needs DxTimingCaptureLibraryOptions::TrackGpuEngineActivity
// and a trace carrying the DxgKrnl provider.
class GpuEngineActivityCallbacks
{
public:
    virtual ~GpuEngineActivityCallbacks() = default;

    // Treat the span as an upper bound on engine time rather than a measurement of it.
    // It opens when the packet is handed to the driver, which can be well before the engine picks it up.
    //
    // processId is 0 when the packet could not be traced back to a process.
    // hardwareCommandQueueId is 0 when the engine is not known yet.
    virtual HRESULT OnGpuEngineActivity(
        UINT32 processId,
        UINT64 hardwareCommandQueueId,
        UINT32 engineAffinity,
        INT64 submitTimestamp,
        INT64 completeTimestamp,
        bool hardwareScheduled) = 0;
};

class PipelineStateEventCallbacks
{
public:
    virtual ~PipelineStateEventCallbacks() = default;

    virtual HRESULT OnPsoCompilation(
        UINT32 processId,
        UINT32 threadId,
        UINT64 apiObjectId,
        INT64 startTimestamp,
        INT64 endTimestamp) = 0;
};

class ApiObjectCallbacks
{
public:
    virtual ~ApiObjectCallbacks() = default;

    virtual HRESULT OnDeviceCreation(
        INT64 timestamp,
        const DeviceInfo* device,
        _Out_ UINT64* objectId) = 0;

    virtual HRESULT OnDescriptorHeapCreation(
        INT64 timestamp,
        UINT64 deviceId,
        UINT32 processId,
        UINT32 threadId,
        const D3D12_DESCRIPTOR_HEAP_DESC* desc,
        const ObjectPlacementInfo* placement,
        _Out_ UINT64* objectId) = 0;

    // A committed resource owns an implicit heap, so it carries that heap's
    // properties and flags along with its GPU virtual address range.
    virtual HRESULT OnCommittedResourceCreation(
        INT64 timestamp,
        UINT64 deviceId,
        UINT32 processId,
        UINT32 threadId,
        const D3D12_RESOURCE_DESC* desc,
        const ObjectPlacementInfo* placement,
        const D3D12_HEAP_PROPERTIES* heapProperties,
        D3D12_HEAP_FLAGS heapFlags,
        _Out_ UINT64* objectId) = 0;

    // A placed resource lives within a separately-created heap; it only adds a
    // GPU virtual address range on top of the resource description.
    virtual HRESULT OnPlacedResourceCreation(
        INT64 timestamp,
        UINT64 deviceId,
        UINT32 processId,
        UINT32 threadId,
        const D3D12_RESOURCE_DESC* desc,
        const ObjectPlacementInfo* placement,
        _Out_ UINT64* objectId) = 0;

    // A reserved (tiled) resource reserves a GPU virtual address range at
    // creation but has no physical backing until tiles are mapped
    // (UpdateTileMappings).
    virtual HRESULT OnReservedResourceCreation(
        INT64 timestamp,
        UINT64 deviceId,
        UINT32 processId,
        UINT32 threadId,
        const D3D12_RESOURCE_DESC* desc,
        const ReservedResourceInfo* reserved,
        _Out_ UINT64* objectId) = 0;

    virtual HRESULT OnHeapCreation(
        INT64 timestamp,
        UINT64 deviceId,
        UINT32 processId,
        UINT32 threadId,
        const D3D12_HEAP_DESC* desc,
        const ObjectPlacementInfo* placement,
        _Out_ UINT64* objectId) = 0;

    virtual HRESULT OnPipelineStateCreation(
        INT64 timestamp,
        UINT64 deviceId,
        UINT32 processId,
        UINT32 threadId,
        const ObjectPlacementInfo* placement,
        _Out_ UINT64* objectId) = 0;

    virtual HRESULT OnStateObjectCreation(
        INT64 timestamp,
        UINT64 deviceId,
        UINT32 processId,
        UINT32 threadId,
        const ObjectPlacementInfo* placement,
        _Out_ UINT64* objectId) = 0;

    virtual HRESULT OnCommandAllocatorCreation(
        INT64 timestamp,
        UINT64 deviceId,
        UINT32 processId,
        UINT32 threadId,
        D3D12_COMMAND_LIST_TYPE commandListType,
        const ObjectPlacementInfo* placement,
        _Out_ UINT64* objectId) = 0;

    virtual HRESULT OnMetaCommandCreation(
        INT64 timestamp,
        UINT64 deviceId,
        UINT32 processId,
        UINT32 threadId,
        REFGUID commandId,
        const ObjectPlacementInfo* placement,
        _Out_ UINT64* objectId) = 0;

    virtual HRESULT OnObjectDestruction(INT64 timestamp, ApiObjectType type, UINT64 objectId) = 0;

    virtual HRESULT OnApiObjectName(INT64 timestamp, ApiObjectType type, UINT64 objectId, std::wstring_view name) = 0;
    virtual HRESULT OnApiObjectSizeAndAddress(INT64 timestamp, ApiObjectType type, UINT64 objectId, UINT64 allocationSize, UINT64 baseAddress) = 0;
};

// Optional interface for the D3D12 runtime's "failure journal" - the runtime's
// running record of its most recent internal failures: validation errors,
// driver-reported errors, out-of-memory, and command-list removals (for example a
// CopyBufferRegion rejected while recording, which is surfaced to the app only
// later at Close()).
class RuntimeFailureCallbacks
{
public:
    virtual ~RuntimeFailureCallbacks() = default;

    // Called once per journaled failure.
    //   timestamp - when the failure was recorded, on the nanosecond timeline the
    //               library uses for all callbacks.
    //   index     - the runtime's journal slot; it wraps, so it is not a unique id.
    //   code      - the failing HRESULT or NTSTATUS (e.g. E_INVALIDARG 0x80070057).
    //   threadId  - the OS thread that hit the failure.
    //   message   - a short description; may be empty or truncated to 63 characters.
    //
    // The same failure may be reported more than once (once when it happens, and
    // again if the trace replays recent history when a session starts);
    // de-duplicate on (code, threadId, message) if you need each failure once.
    virtual HRESULT OnD3D12JournalEntry(
        INT64 timestamp,
        UINT32 index,
        UINT32 code,
        UINT32 threadId,
        std::string_view message) = 0;
};

class DirectStorageCallbacks
{
public:
    virtual ~DirectStorageCallbacks() = default;

    // A file referenced by DirectStorage, reported once when first seen. path is the
    // file's name when the file was introduced by an OpenFile event; when a request
    // references a file that was never opened in the trace, the library synthesizes a
    // placeholder name of the form "file# <id>" so the file is still identifiable.
    virtual HRESULT OnDirectStorageFile(UINT16 fileId, INT64 timestamp, std::wstring_view path) = 0;

    // A DirectStorage queue, reported once when first seen. name is the queue's name
    // when it was introduced by a CreateQueue event, and empty when the queue was
    // first seen via an enqueue on a queue whose creation was not captured.
    virtual HRESULT OnDirectStorageQueue(UINT16 queueId, INT64 timestamp, std::string_view name) = 0;

    // A read request whose enqueue and completion have been correlated. Reported only
    // when its enqueue was observed in the trace; a completion whose enqueue predates
    // the trace is dropped (no callback) rather than reported with empty fields --
    // unlike the status, fence and set-event callbacks below, which still report an
    // uncorrelated completion (with EnqueueTimestamp equal to CompletionTimestamp).
    struct ReadRequest
    {
        UINT16 QueueId;
        UINT16 FileId;
        UINT32 EnqueueThreadId;
        INT64 EnqueueTimestamp;
        INT64 CompletionTimestamp;
        UINT64 Offset;
        UINT32 Size;
        UINT8 CompressionType;
        UINT8 ShuffleType;
    };
    virtual HRESULT OnDirectStorageReadRequest(const ReadRequest& request) = 0;

    // A status-array notification whose enqueue and completion have been correlated.
    // When the completion is seen without a matching enqueue, EnqueueTimestamp equals
    // CompletionTimestamp.
    struct StatusNotification
    {
        UINT16 QueueId;
        UINT32 EnqueueThreadId;
        INT64 EnqueueTimestamp;
        INT64 CompletionTimestamp;
        UINT64 StatusArray;
        UINT64 Index;
    };
    virtual HRESULT OnDirectStorageStatus(const StatusNotification& status) = 0;

    // A fence signal whose enqueue and completion have been correlated. When the
    // completion is seen without a matching enqueue, EnqueueTimestamp equals
    // CompletionTimestamp.
    struct FenceSignal
    {
        UINT16 QueueId;
        UINT32 EnqueueThreadId;
        INT64 EnqueueTimestamp;
        INT64 CompletionTimestamp;
        UINT64 Fence;
        UINT64 Value;
    };
    virtual HRESULT OnDirectStorageFenceSignal(const FenceSignal& signal) = 0;

    // A Win32-event set request whose enqueue and completion have been correlated.
    // When the completion is seen without a matching enqueue, EnqueueTimestamp equals
    // CompletionTimestamp.
    struct SetEventNotification
    {
        UINT16 QueueId;
        UINT32 EnqueueThreadId;
        INT64 EnqueueTimestamp;
        INT64 CompletionTimestamp;
        UINT64 Handle;
    };
    virtual HRESULT OnDirectStorageSetEvent(const SetEventNotification& setEvent) = 0;

    // A queue submit (the point at which enqueued requests are handed to the runtime
    // for execution).
    struct Submit
    {
        UINT16 QueueId;
        UINT32 EnqueueThreadId;
        INT64 Timestamp;
        bool AutoSubmitted;
    };
    virtual HRESULT OnDirectStorageSubmit(const Submit& submit) = 0;
};

} // namespace DirectX::Etw
