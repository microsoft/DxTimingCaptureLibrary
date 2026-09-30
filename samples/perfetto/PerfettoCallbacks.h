// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#pragma once

#include <optional>
#include <unordered_map>

#include <DxTimingCaptureLibrary/NoOpCallbacks.h>

#include "PerfettoTraceWriter.h"

using namespace DirectX::Etw;

// Callback implementations that forward the DxTimingCaptureLibrary events into a
// PerfettoTraceWriter. Each derives from the matching no-op base and overrides
// only the methods that map onto the Perfetto timeline; the _Out_ id methods are
// overridden so every id comes from the writer's single shared counter (which
// keeps ids globally unique - convenient for keying Perfetto tracks).

class PerfettoApiObjectCallbacks : public NoOpApiObjectCallbacks
{
    PerfettoTraceWriter& m_w;

    HRESULT Create(const char* track, INT64 ts, std::wstring label, UINT64* objectId)
    {
        // The library may pass a null out-pointer when it does not need the id
        // back; keep a local id for our own bookkeeping and only write if asked.
        uint64_t id = m_w.NextId();
        if (objectId) *objectId = id;
        m_w.ObjectCreated(id, track, ts, std::move(label));
        return S_OK;
    }

public:
    explicit PerfettoApiObjectCallbacks(PerfettoTraceWriter& w) : m_w(w) {}

    HRESULT OnDeviceCreation(INT64 ts, const DirectX::Etw::DeviceInfo*, UINT64* id) override
    {
        return Create("Devices", ts, L"Device", id);
    }
    HRESULT OnDescriptorHeapCreation(INT64 ts, UINT64, UINT32, UINT32, const D3D12_DESCRIPTOR_HEAP_DESC*, const DirectX::Etw::ObjectPlacementInfo*, UINT64* id) override
    {
        return Create("Descriptor Heaps", ts, L"Descriptor Heap", id);
    }
    HRESULT OnCommittedResourceCreation(INT64 ts, UINT64, UINT32, UINT32, const D3D12_RESOURCE_DESC*, const DirectX::Etw::ObjectPlacementInfo*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS, UINT64* id) override
    {
        return Create("Resources", ts, L"Committed Resource", id);
    }
    HRESULT OnPlacedResourceCreation(INT64 ts, UINT64, UINT32, UINT32, const D3D12_RESOURCE_DESC*, const DirectX::Etw::ObjectPlacementInfo*, UINT64* id) override
    {
        return Create("Resources", ts, L"Placed Resource", id);
    }
    HRESULT OnReservedResourceCreation(INT64 ts, UINT64, UINT32, UINT32, const D3D12_RESOURCE_DESC*, const DirectX::Etw::ReservedResourceInfo*, UINT64* id) override
    {
        return Create("Resources", ts, L"Reserved Resource", id);
    }
    HRESULT OnHeapCreation(INT64 ts, UINT64, UINT32, UINT32, const D3D12_HEAP_DESC*, const DirectX::Etw::ObjectPlacementInfo*, UINT64* id) override
    {
        return Create("Heaps", ts, L"Heap", id);
    }
    HRESULT OnPipelineStateCreation(INT64 ts, UINT64, UINT32, UINT32, const DirectX::Etw::ObjectPlacementInfo*, UINT64* id) override
    {
        return Create("Pipeline States", ts, L"Pipeline State", id);
    }
    HRESULT OnStateObjectCreation(INT64 ts, UINT64, UINT32, UINT32, const DirectX::Etw::ObjectPlacementInfo*, UINT64* id) override
    {
        return Create("State Objects", ts, L"State Object", id);
    }
    HRESULT OnCommandAllocatorCreation(INT64 ts, UINT64, UINT32, UINT32, D3D12_COMMAND_LIST_TYPE, const DirectX::Etw::ObjectPlacementInfo*, UINT64* id) override
    {
        return Create("Command Allocators", ts, L"Command Allocator", id);
    }
    HRESULT OnMetaCommandCreation(INT64 ts, UINT64, UINT32, UINT32, REFGUID, const DirectX::Etw::ObjectPlacementInfo*, UINT64* id) override
    {
        return Create("Meta Commands", ts, L"Meta Command", id);
    }
    HRESULT OnObjectDestruction(INT64 ts, DirectX::Etw::ApiObjectType, UINT64 objectId) override
    {
        m_w.ObjectDestroyed(objectId, ts);
        return S_OK;
    }
    HRESULT OnApiObjectName(INT64, DirectX::Etw::ApiObjectType, UINT64 objectId, std::wstring_view name) override
    {
        m_w.ObjectNamed(objectId, std::wstring(name).c_str());
        return S_OK;
    }
};

class PerfettoDxgkObjectCallbacks : public NoOpDxgkObjectCallbacks
{
    PerfettoTraceWriter& m_w;

public:
    explicit PerfettoDxgkObjectCallbacks(PerfettoTraceWriter& w) : m_w(w) {}

    HRESULT OnHardwareAdapter(UINT64* adapterId) override
    {
        if (adapterId) *adapterId = m_w.NextId();
        return S_OK;
    }
    HRESULT OnHardwareCommandQueue(UINT64, UINT64* hwQueueId) override
    {
        if (hwQueueId) *hwQueueId = m_w.NextId();
        return S_OK;
    }
    HRESULT OnHardwareCommandQueueName(UINT64 hwQueueId, PCWSTR name) override
    {
        m_w.NameHwQueue(hwQueueId, name);
        return S_OK;
    }
    HRESULT OnApiCommandQueue(UINT32, UINT64, PCWSTR type, INT64 beginTs, UINT64* apiQueueId) override
    {
        uint64_t id = m_w.NextId();
        if (apiQueueId) *apiQueueId = id;
        m_w.DefineApiQueue(id, beginTs, type);
        return S_OK;
    }
    HRESULT OnApiCommandQueueName(UINT64 apiQueueId, PCWSTR name) override
    {
        m_w.NameApiQueue(apiQueueId, name);
        return S_OK;
    }
    HRESULT OnApiCommandQueueEnd(UINT64 apiQueueId, INT64 endTs) override
    {
        m_w.EndApiQueue(apiQueueId, endTs);
        return S_OK;
    }
};

// GPU work is tracked per API command queue, not per hardware engine: engines are
// shared machine-wide, so keying on one puts two apps' work on a single track.
// targetProcessId 0 gives every D3D12 process its own track; otherwise only the
// target gets one and the rest are left to the engine-activity tracks.
class PerfettoGpuTimingsCallbacks : public NoOpGpuTimingsCallbacks
{
    PerfettoTraceWriter& m_w;
    std::optional<UINT32> m_targetProcessId;

    struct ExecutionInfo { UINT32 processId; UINT64 apiQueueId; };

    // Only OnGpuExecutionBegin knows the process and queue. Single ETW consumer
    // thread, so no locking.
    std::unordered_map<UINT64, ExecutionInfo> m_executions;

    bool IsTracked(UINT32 processId) const
    {
        return !m_targetProcessId || processId == *m_targetProcessId;
    }

public:
    PerfettoGpuTimingsCallbacks(PerfettoTraceWriter& w, std::optional<UINT32> targetProcessId)
        : m_w(w), m_targetProcessId(targetProcessId) {}

    HRESULT OnGpuExecutionBegin(UINT32 processId, UINT32, UINT64 apiCommandQueueId, INT64, UINT64, UINT64* gpuExecutionId) override
    {
        // The library always needs an id back, even for work we go on to drop.
        uint64_t id = m_w.NextId();
        if (gpuExecutionId) *gpuExecutionId = id;
        m_executions[id] = ExecutionInfo{ processId, apiCommandQueueId };
        return S_OK;
    }
    HRESULT OnGpuExecutionComplete(UINT64 gpuExecutionId) override
    {
        m_executions.erase(gpuExecutionId);
        return S_OK;
    }
    HRESULT OnGpuWork(UINT64 hwQueueId, UINT64 gpuExecutionId, UINT32, UINT64 apiMarkerId, INT64 beginTs, INT64 endTs) override
    {
        // Without the submitting execution there is no process to attribute the work
        // to, and no way to keep it off another process's track.
        auto execution = m_executions.find(gpuExecutionId);
        if (execution == m_executions.end() || !IsTracked(execution->second.processId))
        {
            return S_OK;
        }

        m_w.EmitGpuWork(execution->second.processId, execution->second.apiQueueId, hwQueueId, apiMarkerId, beginTs, endTs);
        return S_OK;
    }
    HRESULT OnApiMarker(UINT32 processId, UINT32, PCWSTR name, INT64 ts, UINT64* apiMarkerId) override
    {
        uint64_t id = m_w.NextId();
        if (apiMarkerId) *apiMarkerId = id;

        // An unregistered id leaves the GPU work slice with its default label.
        if (IsTracked(processId))
        {
            m_w.RegisterApiMarker(id, name, ts);
        }
        return S_OK;
    }
};

// Covers the processes that have no detailed track of their own; the writer drops
// the ones that do.
class PerfettoGpuEngineActivityCallbacks : public NoOpGpuEngineActivityCallbacks
{
    PerfettoTraceWriter& m_writer;

public:
    explicit PerfettoGpuEngineActivityCallbacks(PerfettoTraceWriter& writer)
        : m_writer(writer) {}

    HRESULT OnGpuEngineActivity(
        UINT32 processId,
        UINT64 hardwareCommandQueueId,
        UINT32,
        INT64 submitTimestamp,
        INT64 completeTimestamp,
        bool hardwareScheduled) override
    {
        m_writer.EmitOtherProcessGpuWork(processId, hardwareCommandQueueId, submitTimestamp, completeTimestamp, hardwareScheduled);
        return S_OK;
    }
};

class PerfettoPipelineStateEventCallbacks : public NoOpPipelineStateEventCallbacks
{
    PerfettoTraceWriter& m_w;

public:
    explicit PerfettoPipelineStateEventCallbacks(PerfettoTraceWriter& w) : m_w(w) {}

    HRESULT OnPsoCompilation(UINT32, UINT32 threadId, UINT64, INT64 startTs, INT64 endTs) override
    {
        m_w.EmitPsoCompile(threadId, startTs, endTs);
        return S_OK;
    }
};
