// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#pragma once

#define D3D12_TOKENIZED_PROGRAM_FORMAT_HEADER
#define NTSTATUS long // Defining this here to avoid including certain windows headers that may cause conflicts.
#include <d3dkmthk.h>

#include <evntrace.h>
#include <evntcons.h>

#include <D3D12Events.h>
#include <lddmcoreeventdefs.h>
#ifndef _M_IX86 // only defined for x86 build
#include <DxTimingCaptureLibrary/PixEtwManifest.h>
#endif

#include "D3D12EtwEventStructs.h"

#include "EventData.h"
#include "EventTypes.h"

#include "DxgkEtwEventStructs.h"

#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

#include <dstorage_etw.h>

namespace DirectX::Etw
{
    template<typename>
    struct is_tuple : std::false_type
    {
    };

    template<typename... T>
    struct is_tuple<std::tuple<T...>> : std::true_type
    {
    };

    template<typename T, typename = void>
    struct HasDirectStorageProcessor : std::false_type
    {
    };

    template<typename T>
    struct HasDirectStorageProcessor<T, std::void_t<decltype(std::declval<T&>().GetDirectStorageProcessor())>> : std::true_type
    {
    };

    //
    // This dispatches ETW events to the appropriate handlers.  The handlers
    // have the same name as the function that emits them, with "On" prefixed.
    // For example, the EventD3D12Marker event is emitted with a call to
    // D3D12Trace_EventD3D12Marker and is dispatched to
    // OnD3D12Trace_EventD3D12Marker.  This is to aid looking up how the events
    // are emitted in the runtime code.
    //
    template<typename T>
    class EtwDispatcher
    {
        T* m_obj;

    public:
        EtwDispatcher(T* obj)
            : m_obj(obj)
        {
        }

        void SetTraceInfo(EVENT_TRACE_LOGFILE const& logfile)
        {
            m_obj->SetTraceInfo(logfile);
        }

        void Dispatch(EVENT_RECORD* record)
        {
            auto const& providerId = record->EventHeader.ProviderId;

            if (providerId == Direct3D12EtwProviderGuid)
                DispatchD3D12Trace(record);
            else if (providerId == DxgkControlGuid)
                DispatchDxgkControl(record);
            else if (providerId == PixEtw::PIX_ETW_PROVIDER_WINDOWS)
                DispatchWinPixEvent(record);
            else if (providerId == EventTraceConfigGuid)
                DispatchTraceConfig(record);
            else if (providerId == DirectStorage::DStorageEtwProvider)
            {
                if constexpr (HasDirectStorageProcessor<T>::value)
                {
                    DispatchDirectStorageTrace(record);
                }
            }
        }

    private:
        //
        // The "event" parameter corresponds to the name of the event in the ETW
        // manifest.  We dispatch events via the Template_XXX functions.  The
        // XXX part here corresponds to the Template ID in the manifest.
        //
        // The handler name should match the name that appears in the source
        // code to emit the event, prefixed with "On".  This cannot be
        // determined by the manifest itself (parameters to the tool to build
        // the header from the manifest control this), so when adding new events
        // some detective work is required to figure out the right names.
        //
        // Example ETW manifest:
        //
        // <event
        //     channel="Direct3D12-Analytic-Channel"
        //     keywords="Markers"
        //     level="win:LogAlways"
        //     opcode="win:Info"
        //     symbol="EventD3D12Marker"
        //     task="Marker"
        //     template="D3D12Marker"
        //     value="38"
        //     />
        //
        // Corresponding dispatch:
        //
        //     DISPATCH(EventD3D12Marker, D3D12Marker, OnD3D12Trace_EventD3D12Marker)
        //
        // reSearching for "D3D12Trace_EventD3D12Marker" will locate the code
        // that emits this event.
        //
#define DISPATCH(event, template, handler) case event##_value: Template_##template(record, &T::handler); break

// For D3D12 events, use this dispatch macro instead. This slightly different pattern allows us to avoid adding
// empty definitions to every "T" class whenever we add a new event. The classes that wish to handle the event
// simply definte a template specialization with the appropriate arguments (see D3D12EtwEventStructs.h).
// The opcodes come from the ETW manifest itself. Info is the "default". DCStart are events fired at the beginning
// of an ETW session (also called capture state, rundown, or report).
#define DISPATCH_D3D12(event, template, opcode) case event##_value: Template_##template<template##Args>(record, &T::OnD3D12Event_##opcode); break

        // See DirectX-Direct3D12.man
        void DispatchD3D12Trace(EVENT_RECORD* record)
        {
            switch (record->EventHeader.EventDescriptor.Id)
            {
                DISPATCH_D3D12(EventD3D12ReportName,              D3D12NamedObject,             Info);
                DISPATCH_D3D12(EventD3D12ReportNameWide,          D3D12NamedObjectWide,         Info);
                DISPATCH_D3D12(EventD3D12RenameObject,            D3D12RenameObject,            Info);
                DISPATCH_D3D12(EventD3D12RenameObjectWide,        D3D12RenameObjectWide,        Info);
                DISPATCH_D3D12(EventD3D12CreateDevice,            D3D12Device,                  Start);
                DISPATCH_D3D12(EventD3D12DestroyDevice,           D3D12Device,                  Stop);
                DISPATCH_D3D12(EventD3D12ReportDevice,            D3D12Device,                  DCStart);
                DISPATCH_D3D12(EventD3D12CreateCommandList,       D3D12CommandList,             Start);
                DISPATCH_D3D12(EventD3D12DestroyCommandList,      D3D12CommandList,             Stop);
                DISPATCH_D3D12(EventD3D12ReportCommandList,       D3D12CommandList,             DCStart);
                DISPATCH_D3D12(EventD3D12ResetCommandList,        D3D12CommandList,             Info);
                DISPATCH_D3D12(EventD3D12CreateCommandQueue,      D3D12CommandQueue,            Start);
                DISPATCH_D3D12(EventD3D12DestroyCommandQueue,     D3D12CommandQueue,            Stop);
                DISPATCH_D3D12(EventD3D12ReportCommandQueue,      D3D12CommandQueue,            DCStart);
                DISPATCH_D3D12(EventD3D12CreateHeap,              D3D12Heap,                    Start);
                DISPATCH_D3D12(EventD3D12DestroyHeap,             D3D12Heap,                    Stop);
                DISPATCH_D3D12(EventD3D12ReportHeap,              D3D12Heap,                    DCStart);
                DISPATCH_D3D12(EventD3D12CreateResource,          D3D12Resource,                Start);
                DISPATCH_D3D12(EventD3D12DestroyResource,         D3D12Resource,                Stop);
                DISPATCH_D3D12(EventD3D12ReportResource,          D3D12Resource,                DCStart);
                DISPATCH_D3D12(EventD3D12CreateGraphicsPipelineState,     D3D12GraphicsPipelineState, Start);
                DISPATCH_D3D12(EventD3D12DestroyGraphicsPipelineState,    D3D12GraphicsPipelineState, Stop);
                DISPATCH_D3D12(EventD3D12ReportGraphicsPipelineState,     D3D12GraphicsPipelineState, DCStart);
                DISPATCH_D3D12(EventD3D12CreateStateObject,     D3D12StateObject, Start);
                DISPATCH_D3D12(EventD3D12DestroyStateObject,    D3D12StateObject, Stop);
                DISPATCH_D3D12(EventD3D12ReportStateObject,     D3D12StateObject, DCStart);
                DISPATCH_D3D12(EventD3D12CreateCommandAllocator, D3D12CommandAllocator, Start);
                DISPATCH_D3D12(EventD3D12DestroyCommandAllocator, D3D12CommandAllocator, Stop);
                DISPATCH_D3D12(EventD3D12ReportCommandAllocator, D3D12CommandAllocator, DCStart);
                DISPATCH_D3D12(EventD3D12CreateDescriptorHeap, D3D12DescriptorHeap, Start);
                DISPATCH_D3D12(EventD3D12DestroyDescriptorHeap, D3D12DescriptorHeap, Stop);
                DISPATCH_D3D12(EventD3D12ReportDescriptorHeap, D3D12DescriptorHeap, DCStart);
                DISPATCH_D3D12(EventD3D12CreateMetaCommand, D3D12MetaCommand, Start);
                DISPATCH_D3D12(EventD3D12DestroyMetaCommand, D3D12MetaCommand, Stop);
                DISPATCH_D3D12(EventD3D12ReportMetaCommand, D3D12MetaCommand, DCStart);
                DISPATCH_D3D12(EventD3D12AllocationInfo,          D3D12AllocationInfo,          Info);
                DISPATCH_D3D12(EventD3D12Marker,                  D3D12Marker,                  Info);
                DISPATCH_D3D12(EventD3D12RuntimeMarkerData,       D3D12RuntimeMarkerData,       Info);
                DISPATCH_D3D12(EventD3D12CommandBufferSubmission, D3D12CommandBufferSubmission, Info);
                DISPATCH_D3D12(EventD3D12StartExecuteCommandList, D3D12ExecuteCommandList,      Start);
                DISPATCH_D3D12(EventD3D12StopExecuteCommandList,  D3D12ExecuteCommandList,      Stop);
                DISPATCH_D3D12(EventD3D12HistoryBufferCompletion, D3D12HistoryBufferCompletion, Info);
                DISPATCH_D3D12(EventD3D12StartExecuteCommandLists,D3D12ExecuteCommandLists,     Start);
                DISPATCH_D3D12(EventD3D12StopExecuteCommandLists, D3D12ExecuteCommandLists,     Stop);
                DISPATCH_D3D12(EventD3D12BeginCreatePipelineStateObject,    D3D12CreatePipelineStateObject, Start);
                DISPATCH_D3D12(EventD3D12EndCreatePipelineStateObject,      D3D12CreatePipelineStateObject, Stop);
                DISPATCH_D3D12(EventD3D12BeginCreateStateObject,            D3D12CreateStateObject, Start);
                DISPATCH_D3D12(EventD3D12EndCreateStateObject,              D3D12CreateStateObject, Stop);
                DISPATCH_D3D12(EventD3D12BeginAddToStateObject,             D3D12AddToStateObject, Start);
                DISPATCH_D3D12(EventD3D12EndAddToStateObject,               D3D12AddToStateObject,  Stop);
                DISPATCH_D3D12(EventD3D12CreatePipelineStateObjectCacheStatistics, D3D12CacheStatistics, Info);
                DISPATCH_D3D12(EventD3D12CreateStateObjectCacheStatistics,         D3D12CacheStatistics, Info);
                DISPATCH_D3D12(EventD3D12AddToStateObjectCacheStatistics,          D3D12CacheStatistics, Info);
                DISPATCH_D3D12(EventD3D12JournalEntry,            D3D12JournalEntry,            Info);
            }
        }

#define DISPATCH_DXGK(event, template, opcode) case event##_value: Template_Dxgk##template<Dxgk##template##Args>(record, &T::OnDxgkEvent_##opcode); break

        // See LDDMCore.man
        void DispatchDxgkControl(EVENT_RECORD* record)
        {
            switch (record->EventHeader.EventDescriptor.Id)
            {
                DISPATCH_DXGK(EventVSyncDPC,          VSyncDPC,                  Info);
                DISPATCH_DXGK(EventCalibrateGpuClock, CalibrateGpuClock,         Info);
                DISPATCH_DXGK(EventHistoryBuffer,     HistoryBuffer,             Info);
                DISPATCH_DXGK(EventCreateContext,     Context,                   Start);
                DISPATCH_DXGK(EventDestroyContext,    Context,                   Stop);
                DISPATCH_DXGK(EventReportContext,     Context,                   DCStart);
                DISPATCH_DXGK(EventCreateDevice,      Device,                    Start);
                DISPATCH_DXGK(EventDestroyDevice,     Device,                    Stop);
                DISPATCH_DXGK(EventReportDevice,      Device,                    DCStart);
                DISPATCH_DXGK(EventDpiReportAdapter,  DpiReportAdapter,          Info);
                DISPATCH_DXGK(EventNodeMetadata,      NodeMetadata,              Info);
                DISPATCH_DXGK(EventCreateHwQueue,     HwQueue,                   Start);
                DISPATCH_DXGK(EventDestroyHwQueue,    HwQueue,                   Stop);
                DISPATCH_DXGK(EventReportHwQueue,     HwQueue,                   DCStart);
                DISPATCH_DXGK(EventDmaReleaseToGpu,   DmaReleaseToGpu,           Info);
                DISPATCH_DXGK(EventDmaCompleteByGpu,  DmaCompleteByGpu,          Info);
                DISPATCH_DXGK(EventDmaSubmit,         DmaSubmit,                 Start);
                DISPATCH_DXGK(EventDmaIsrComplete,    DmaIsrComplete,            Info);
                DISPATCH_DXGK(EventCreateAdapterAllocation,  AdapterAllocation,  Start);
                DISPATCH_DXGK(EventReportAdapterAllocation,  AdapterAllocation,  DCStart);
                DISPATCH_DXGK(EventDestroyAdapterAllocation, AdapterAllocation,  Stop);
                DISPATCH_DXGK(EventCreateDeviceAllocation, DeviceAllocation,    Start);
                DISPATCH_DXGK(EventReportDeviceAllocation, DeviceAllocation,    DCStart);
                DISPATCH_DXGK(EventDestroyDeviceAllocation, DeviceAllocation,   Stop);
                DISPATCH_DXGK(EventPagingOpVirtualTransfer, PagingOpVirtualTransfer,           Info);
                DISPATCH_DXGK(EventVidMmProcessBudgetChange, VidMmProcessBudgetChange,      Info);
                DISPATCH_DXGK(EventVidMmProcessUsageChange, VidMmProcessUsageChange,        Info);
                DISPATCH_DXGK(EventVidMmProcessCommitmentChange, VidMmProcessCommitmentChange, Info);
                DISPATCH_DXGK(EventVidMmProcessDemotedCommitmentChange, VidMmProcessDemotedCommitmentChange, Info);
                DISPATCH_DXGK(EventVidMmMakeResident, VidMmMakeResident,                Info);
                DISPATCH_DXGK(EventVidMmEvict, VidMmEvict,                       Info);
                DISPATCH_DXGK(EventReportSegment, ReportSegment, Info);
                DISPATCH_DXGK(EventPagingOpVirtualFill, PagingOpVirtualFill, Info);
                DISPATCH_DXGK(EventPagingOpSysmemCommit, PagingOpSysmemCommit, Info);
                DISPATCH_DXGK(EventPagingOpMapApertureSegment, PagingOpMapApertureSegment, Info);
                // MigrateAllocation and CompleteAllocationMigration don't follow the typical Start/Stop ETW convention or naming scheme.
                // The manifest names also don't appear to match the header (depending on where you get the manifest apparently),
                // but the values check out (see lddmcoreeventdefs.h for the DXGK defines and winmeta.h for ETW opcode values to manually compare).
                DISPATCH_DXGK(EventMigrateAllocation, MigrateAllocation, Info);
                DISPATCH_DXGK(EventCompleteAllocationMigration, CompleteAllocationMigration, Stop);
            }
        }

        // See WinPixEventRuntime's PIXETW.man
        void DispatchWinPixEvent(EVENT_RECORD* record)
        {
            // Bring the vendored PIX event-id constants (PixEtw::*_value) into
            // scope for the DISPATCH macro below. This using-directive is
            // function-local, so it does not leak the names to consumers.
            using namespace PixEtw;

            switch (record->EventHeader.EventDescriptor.Id)
            {
                DISPATCH(PIXRecordTimingBlock_v1, TimingBlock, OnEventWritePIXRecordTimingBlock_v1);
                DISPATCH(PIXRecordTimingBlock_v2, TimingBlock, OnEventWritePIXRecordTimingBlock_v2);
                DISPATCH(PIXReportCounterData, PixCounterData, OnEventWritePIXReportCounterData);
                DISPATCH(PIXTrackMemoryAllocation, PixMemoryEvent, OnEventWritePIXRecordMemoryAllocationEvent);
                DISPATCH(PIXTrackMemoryFree, PixMemoryEvent, OnEventWritePIXRecordMemoryFreeEvent);
            }
        }

#undef DISPATCH

#define DISPATCH_SYSTEM_CONFIG(event, template, handler) case event: Template_##template(record, &T::handler); break

        // See <ntwmi.h>
        // See rundown_pnp.c (search ETW_NT_FLAGS_TRACE_RUNDOWN_V5)
        void DispatchTraceConfig(EVENT_RECORD* record)
        {
            switch (record->EventHeader.EventDescriptor.Opcode)
            {
                DISPATCH_SYSTEM_CONFIG(EVENT_TRACE_TYPE_CONFIG_PNP, PnPDeviceDescription, OnPnpDeviceDescription);
            }
        }

#undef DISPATCH_SYSTEM_CONFIG

        template<typename FN>
        void Template_PnPDeviceDescription(EVENT_RECORD* record, FN target)
        {
            EventData d(record);

            std::wstring_view deviceId;
            std::wstring_view deviceDescription;

            switch (record->EventHeader.EventDescriptor.Version)
            {
                case 3: // WMI_PNP_RECORD_V3
                {
                    d.ReadUint32();  // IDLength (unused because the string is spec'd to be NULL terminated)
                    d.ReadUint32();  // DescriptionLength  (unused because the string is spec'd to be NULL terminated)
                    d.ReadUint32();  // FriendlyNameLength
                    deviceId = d.ReadNullTerminatedUnicodeString();
                    deviceDescription = d.ReadNullTerminatedUnicodeString();
                }
                break;
                case 4: // WMI_PNP_RECORD_V4
                {
                    d.ReadGuid();   // ClassGuid
                    d.ReadUint32(); // UpperFilterCount
                    d.ReadUint32(); // LowerFilterCount
                    deviceId = d.ReadNullTerminatedUnicodeString();
                    deviceDescription = d.ReadNullTerminatedUnicodeString();
                }
                break;
                case 5: // WMI_PNP_RECORD_V5
                {
                    d.ReadGuid();   // ClassGuid
                    d.ReadUint32(); // UpperFilterCount
                    d.ReadUint32(); // LowerFilterCount
                    d.ReadUint32(); // DevStatus
                    d.ReadUint32(); // DevProblem
                    deviceId = d.ReadNullTerminatedUnicodeString();
                    deviceDescription = d.ReadNullTerminatedUnicodeString();
                }
                break;
                default:
                    Errors::ThrowToolException(E_UNEXPECTED);
                break;
            }

            (m_obj->*target)(
                record,
                deviceId,
                deviceDescription);
        }

        template<typename TEventArgs>
        void Template_D3D12NamedObject(EVENT_RECORD* record, void (T::*target)(EVENT_RECORD*, TEventArgs))
        {
            EventData d(record);

            D3D12NamedObjectArgs args;
            args.object = d.ReadPointer();
            args.name = d.ReadSizedString();

            (m_obj->*target)(
                record,
                args);
        }

        template<typename TEventArgs>
        void Template_D3D12JournalEntry(EVENT_RECORD* record, void (T::*target)(EVENT_RECORD*, TEventArgs))
        {
            EventData d(record);

            D3D12JournalEntryArgs args;
            args.index = d.ReadUint32();
            args.code = d.ReadUint32();
            args.threadId = d.ReadUint32();
            args.message = d.ReadRemainingAsString();

            (m_obj->*target)(
                record,
                args);
        }

        template<typename TEventArgs>
        void Template_D3D12NamedObjectWide(EVENT_RECORD* record, void (T::*target)(EVENT_RECORD*, TEventArgs))
        {
            EventData d(record);

            D3D12NamedObjectWideArgs args;
            args.object = d.ReadPointer();
            args.name = d.ReadSizedWideString();

            (m_obj->*target)(
                record,
                args);
        }

        template<typename TEventArgs>
        void Template_D3D12RenameObject(EVENT_RECORD* record, void (T::*target)(EVENT_RECORD*, TEventArgs))
        {
            EventData d(record);

            D3D12RenameObjectArgs args;
            args.object = d.ReadPointer();
            args.oldName = d.ReadSizedString();
            args.newName = d.ReadSizedString();

            (m_obj->*target)(
                record,
                args);
        }

        template<typename TEventArgs>
        void Template_D3D12RenameObjectWide(EVENT_RECORD* record, void (T::*target)(EVENT_RECORD*, TEventArgs))
        {
            EventData d(record);

            D3D12RenameObjectWideArgs args;
            args.object = d.ReadPointer();
            args.oldName = d.ReadSizedWideString();
            args.newName = d.ReadSizedWideString();

            (m_obj->*target)(
                record,
                args);
        }

        template<typename TEventArgs>
        void Template_D3D12Device(EVENT_RECORD* record, void (T::*target)(EVENT_RECORD*, TEventArgs))
        {
            EventData d(record);

            D3D12DeviceArgs args;
            args.device = d.ReadPointer();
            args.featureLevel = d.ReadUint32();
            args.kmAdapter = d.ReadUint32();
            args.umAdapter = d.ReadPointer();
            args.umAdapterVersion = d.ReadUint64();
            args.kmDevice = d.ReadUint32();
            args.umDeviceVersion = d.ReadUint64();

            (m_obj->*target)(
                record,
                args);
        }

        template<typename TEventArgs>
        void Template_D3D12CommandList(EVENT_RECORD* record, void (T::*target)(EVENT_RECORD*, TEventArgs))
        {
            EventData d(record);

            D3D12CommandListArgs args;
            args.device = d.ReadPointer();
            args.commandList = d.ReadPointer();
            args.sequenceNumber = d.ReadUint64();
            args.commandListType = d.ReadUint32();

            (m_obj->*target)(
                record,
                args);
        }

        template<typename TEventArgs>
        void Template_D3D12CommandQueue(EVENT_RECORD* record, void (T::*target)(EVENT_RECORD*, TEventArgs))
        {
            EventData d(record);

            D3D12CommandQueueArgs args;
            args.device = d.ReadPointer();
            args.commandQueue = d.ReadPointer();
            args.commandListType = d.ReadUint32();
            args.contextCount = d.ReadUint32();
            args.contexts = d.ReadArray<uint32_t>(args.contextCount);
            args.priority = d.ReadInt32();
            args.commandQueueFlags = d.ReadUint32();
            args.nodeMask = d.ReadUint32();

            (m_obj->*target)(
                record,
                args);
        }

        template<typename TEventArgs>
        void Template_D3D12Heap(EVENT_RECORD* record, void (T::*target)(EVENT_RECORD*, TEventArgs))
        {
            EventData d(record);

            D3D12HeapArgs args;
            args.device = d.ReadPointer();
            args.heap = d.ReadPointer();
            args.sizeInBytes = d.ReadUint64();
            args.alignment = d.ReadUint64();
            args.type = d.ReadUint32();
            args.cpuPageProperty = d.ReadUint32();
            args.memoryPoolPreference = d.ReadUint32();
            args.creationNodeMask = d.ReadUint32();
            args.visibleNodeMask = d.ReadUint32();
            args.flags = d.ReadUint32();
            args.conjoinedResource = d.ReadPointer();
            args.kmAllocation = d.ReadUint32();
            args.kmResource = d.ReadUint32();

            (m_obj->*target)(
                record,
                args);
        }

        template<typename TEventArgs>
        void Template_D3D12Resource(EVENT_RECORD* record, void (T::*target)(EVENT_RECORD*, TEventArgs))
        {
            EventData d(record);

            D3D12ResourceArgs args;
            args.device = d.ReadPointer();
            args.resource = d.ReadPointer();
            args.umResource = d.ReadPointer();
            args.dimension = d.ReadUint32();
            args.width = d.ReadUint64();
            args.height = d.ReadUint32();
            args.depth = d.ReadUint32();
            args.mipLevels = d.ReadUint32();
            args.arraySize = d.ReadUint32();
            args.planeCount = d.ReadUint32();
            args.format = d.ReadUint32();
            args.sampleCount = d.ReadUint32();
            args.sampleQuality = d.ReadUint32();
            args.layout = d.ReadUint32();
            args.flags = d.ReadUint32();
            args.heapType = d.ReadUint32();
            args.heap = d.ReadPointer();
            args.immutableHeapOffset = d.ReadUint64();
            args.placedAlignment = d.ReadUint64();
            args.placedSize = d.ReadUint64();
            args.numTilesForResource = d.ReadUint32();
            args.numPackedMips = d.ReadUint32();
            args.numTilesForPackedMips = d.ReadUint32();
            args.immutableBuffer = d.ReadPointer();
            args.immutableBufferOffset = d.ReadUint64();

            (m_obj->*target)(
                record,
                args);
        }

        // This event is used for all D3D12 functions that create an ID3D12PipelineState object,
        // which includes CreateGraphicsPipelineState, CreateComputePipelineState, and CreatePipelineState (PipelineStateStreams).
        // StateObjects are a separate event.
        template<typename TEventArgs>
        void Template_D3D12GraphicsPipelineState(EVENT_RECORD* record, void (T::*target)(EVENT_RECORD*, TEventArgs))
        {
            EventData d(record);

            D3D12GraphicsPipelineStateArgs data;
            data.device = d.ReadPointer();
            data.object = d.ReadPointer();

            (m_obj->*target)(
                record,
                data);
        }

        template<typename TEventArgs>
        void Template_D3D12StateObject(EVENT_RECORD* record, void (T::* target)(EVENT_RECORD*, TEventArgs))
        {
            EventData d(record);

            D3D12StateObjectArgs data;
            data.device = d.ReadPointer();
            data.object = d.ReadPointer();

            (m_obj->*target)(
                record,
                data);
        }

        template<typename TEventArgs>
        void Template_D3D12CommandAllocator(EVENT_RECORD* record, void (T::* target)(EVENT_RECORD*, TEventArgs))
        {
            EventData d(record);

            D3D12CommandAllocatorArgs data;
            data.device = d.ReadPointer();
            data.object = d.ReadPointer();
            data.commandListType = d.ReadUint32();

            (m_obj->*target)(
                record,
                data);
        }

        template<typename TEventArgs>
        void Template_D3D12DescriptorHeap(EVENT_RECORD* record, void (T::* target)(EVENT_RECORD*, TEventArgs))
        {
            EventData d(record);

            D3D12DescriptorHeapArgs data;
            data.device = d.ReadPointer();
            data.object = d.ReadPointer();
            data.descriptorHeapType = d.ReadUint32();
            data.numDescriptors = d.ReadUint32();
            data.descriptorHeapFlags = d.ReadUint32();
            data.nodeMask = d.ReadUint32();

            (m_obj->*target)(
                record,
                data);
        }

        template<typename TEventArgs>
        void Template_D3D12MetaCommand(EVENT_RECORD* record, void (T::* target)(EVENT_RECORD*, TEventArgs))
        {
            EventData d(record);

            D3D12MetaCommandArgs data;
            data.device = d.ReadPointer();
            data.object = d.ReadPointer();
            data.commandId = d.ReadGuid();

            (m_obj->*target)(
                record,
                data);
        }

        template<typename TEventArgs>
        void Template_D3D12AllocationInfo(EVENT_RECORD* record, void (T::*target)(EVENT_RECORD*, TEventArgs))
        {
            EventData d(record);

            D3D12AllocationInfoArgs args;
            args.device = d.ReadPointer();
            args.object = d.ReadPointer();

            args.numVirtualAddressInfos = d.ReadUint32();
            args.virtualAddressInfos = d.ReadArray<VirtualAddressInfos>(args.numVirtualAddressInfos);

            args.numKMTInfos = d.ReadUint32();
            args.kmtInfos = d.ReadArray<KMTInfos>(args.numKMTInfos);

            (m_obj->*target)(
                record,
                args);
        }

        template<typename TEventArgs>
        void Template_D3D12Marker(EVENT_RECORD* record, void (T::*target)(EVENT_RECORD*, TEventArgs))
        {
            EventData d(record);

            D3D12MarkerArgs args;
            args.commandList = d.ReadPointer();
            args.apiSequenceNumber = d.ReadPointer();
            args.metadata = d.ReadUint32();
            args.dataSize = d.ReadUint32();
            args.data = d.ReadArray<uint8_t>(args.dataSize);

            (m_obj->*target)(
                record,
                args);
        }

        template<typename TEventArgs>
        void Template_D3D12RuntimeMarkerData(EVENT_RECORD* record, void (T::*target)(EVENT_RECORD*, TEventArgs))
        {
            EventData d(record);

            D3D12RuntimeMarkerDataArgs args;
            args.cpuFrequency = d.ReadUint64();
            args.firstApiSequenceNumber = d.ReadUint64();
            args.commandList = d.ReadPointer();
            args.cpuTimeHigh = d.ReadUint32();
            args.threadIdCount = d.ReadUint8();
            args.threadIds = d.ReadArray<uint32_t>(args.threadIdCount);
            args.dataSize = d.ReadUint32();
            args.data = d.ReadArray<uint8_t>(args.dataSize);

            (m_obj->*target)(
                record,
                args);
        }

        template<typename TEventArgs>
        void Template_D3D12CommandBufferSubmission(EVENT_RECORD* record, void (T::*target)(EVENT_RECORD*, TEventArgs))
        {
            EventData d(record);

            D3D12CommandBufferSubmissionArgs args;
            args.commandQueue = d.ReadPointer();
            args.contextCount = d.ReadUint32();
            args.contexts = d.ReadArray<uint32_t>(args.contextCount);
            args.loopIteration = d.ReadUint32();
            args.submitCommandCbSequence = d.ReadUint32();
            args.firstApiSequenceNumberHigh = d.ReadUint32();
            args.completedApiSequenceNumberSize = d.ReadUint32();
            args.completedApiSequenceNumbers = d.ReadArray<uint32_t>(args.completedApiSequenceNumberSize);

            // The additional commandList parameter was added for Vibranium to support a new D3D12ExecuteCommandLists
            // ETW event. If a driver doesn't support this new ETW feature the D3D runtime will pass a value of 0 (null).
            // This code will also pass a value of 0 if an older version of this ETW record is used.  This will keep
            // the decoding logic for the event the same.
            args.commandList = record->EventHeader.EventDescriptor.Version > 0 ? d.ReadPointer() : 0;

            (m_obj->*target)(
                record,
                args);
        }

        template<typename TEventArgs>
        void Template_D3D12ExecuteCommandList(EVENT_RECORD* record, void (T::*target)(EVENT_RECORD*, TEventArgs))
        {
            EventData d(record);

            D3D12ExecuteCommandListArgs args;
            args.commandQueue = d.ReadPointer();
            args.commandList = d.ReadPointer();

            (m_obj->*target)(
                record,
                args);
        }

        template<typename TEventArgs>
        void Template_D3D12ExecuteCommandLists(EVENT_RECORD* record, void (T::*target)(EVENT_RECORD*, TEventArgs))
        {
            EventData d(record);

            D3D12ExecuteCommandListsArgs args
            {
                d.ReadPointer(), // commandQueue
                d.ReadUint32(), // commandListCount
                d.ReadPointerArray(args.commandListCount) // commandLists
            };

            (m_obj->*target)(
                record,
                args);
        }

        template<typename TEventArgs>
        void Template_D3D12HistoryBufferCompletion(EVENT_RECORD* record, void (T::* target)(EVENT_RECORD*, TEventArgs))
        {
            EventData d(record);

            D3D12HistoryBufferCompletionArgs args;
            args.hwQueueHandle = d.ReadPointer();
            args.renderCbSequence = d.ReadUint32();
            args.hwQueueProgressFenceId = d.ReadUint64();
            args.precision = d.ReadUint32();
            args.historyBufferSize = d.ReadUint32();
            args.historyBuffer = d.ReadArray<uint8_t>(args.historyBufferSize);

            (m_obj->*target)(
                record,
                args);
        }

        template<typename TEventArgs>
        void Template_D3D12CreatePipelineStateObject(EVENT_RECORD* record, void (T::* target)(EVENT_RECORD*, TEventArgs))
        {
            D3D12CreatePipelineStateObjectArgs args;
            (m_obj->*target)(record, args);
        }

        template<typename TEventArgs>
        void Template_D3D12CreateStateObject(EVENT_RECORD* record, void (T::* target)(EVENT_RECORD*, TEventArgs))
        {
            D3D12CreateStateObjectArgs args;
            (m_obj->*target)(record, args);
        }

        template<typename TEventArgs>
        void Template_D3D12AddToStateObject(EVENT_RECORD* record, void (T::* target)(EVENT_RECORD*, TEventArgs))
        {
            D3D12AddToStateObjectArgs args;
            (m_obj->*target)(record, args);
        }

        template<typename TEventArgs>
        void Template_D3D12CacheStatistics(EVENT_RECORD* record, void (T::* target)(EVENT_RECORD*, TEventArgs))
        {
            EventData d(record);

            D3D12CacheStatisticsArgs args;
            args.numRequiredLookups = d.ReadUint32();
            args.numRequiredHitsInPSDB = d.ReadUint32();
            args.numRequiredHitsInDynamicCache = d.ReadUint32();
            args.numIgnoredHits = d.ReadUint32();
            args.numOptionalLookups = d.ReadUint32();
            args.numOptionalHitsInPSDB = d.ReadUint32();
            args.numOptionalHitsInDynamicCache = d.ReadUint32();
            args.numDynamicCacheStores = d.ReadUint32();

            (m_obj->*target)(record, args);
        }

        template<typename TEventArgs>
        void Template_DxgkVSyncDPC(EVENT_RECORD* record, void (T::* target)(EVENT_RECORD*, TEventArgs))
        {
            EventData d(record);

            DxgkVSyncDPCArgs args;
            args.dxgAdapter = d.ReadPointer();
            args.vidPnTargetId = d.ReadUint32();
            args.scannedPhysicalAddress = d.ReadPointer();
            args.vidPnSourceId = d.ReadUint32();
            args.frameNumber = d.ReadUint32();
            args.frameQPCTime = d.ReadInt64();
            args.flipDevice = d.ReadUint64();
            args.flipType = d.ReadUint32();
            args.flipFenceId = d.ReadUint64();

            (m_obj->*target)(
                record,
                args);
        }

        template<typename TEventArgs>
        void Template_DxgkCalibrateGpuClock(EVENT_RECORD* record, void (T::* target)(EVENT_RECORD*, TEventArgs))
        {
            EventData d(record);

            DxgkCalibrateGpuClockArgs args;
            args.adapter = d.ReadPointer();
            args.nodeOrdinal = d.ReadUint32();
            args.engineOrdinal = d.ReadUint32();
            args.gpuFrequency = d.ReadUint64();
            args.gpuClock = d.ReadUint64();
            args.cpuClock = d.ReadUint64();

            (m_obj->*target)(
                record,
                args);
        }

        template<typename TEventArgs>
        void Template_DxgkHistoryBuffer(EVENT_RECORD* record, void (T::* target)(EVENT_RECORD*, TEventArgs))
        {
            EventData d(record);

            DxgkHistoryBufferArgs args;
            args.context = d.ReadPointer();
            args.renderCbSequence = d.ReadUint32();
            args.dmaSubmissionSequence = d.ReadUint32();
            args.precision = d.ReadUint32();
            args.historyBufferSize = d.ReadUint32();
            args.historyBuffer = d.ReadArray<uint8_t>(args.historyBufferSize);

            (m_obj->*target)(
                record,
                args);
        }

        template<typename TEventArgs>
        void Template_DxgkContext(EVENT_RECORD* record, void (T::* target)(EVENT_RECORD*, TEventArgs))
        {
            EventData d(record);

            DxgkContextArgs args;
            args.device = d.ReadPointer();
            args.nodeOrdinal = d.ReadUint32();
            args.engineAffinity = d.ReadUint32();
            args.dmaBufferSize = d.ReadUint32();
            args.dmaBufferSegmentSet = d.ReadUint32();
            args.dmaBufferPrivateDataSize = d.ReadUint32();
            args.allocationListSize = d.ReadUint32();
            args.patchAllocationListSize = d.ReadUint32();
            args.contextFlags = d.ReadUint32();
            args.context = d.ReadPointer();
            args.contextHandle = d.ReadPointer();
            args.parentDxgContext = d.ReadPointer();

            (m_obj->*target)(
                record,
                args);
        }

        template<typename TEventArgs>
        void Template_DxgkDevice(EVENT_RECORD* record, void (T::* target)(EVENT_RECORD*, TEventArgs))
        {
            EventData d(record);

            DxgkDeviceArgs args;
            args.processId = d.ReadPointer();
            args.dxgAdapter = d.ReadPointer();
            args.clientType = d.ReadUint32();
            args.device = d.ReadPointer();
            args.requestVsync = d.ReadUint32();
            args.disableGpuTimeout = d.ReadUint32();
            args.thunkHandle = d.ReadUint32();

            (m_obj->*target)(
                record,
                args);
        }

        template<typename TEventArgs>
        void Template_DxgkDpiReportAdapter(EVENT_RECORD* record, void (T::* target)(EVENT_RECORD*, TEventArgs))
        {
            EventData d(record);

            DxgkDpiReportAdapterArgs args;
            args.dxgAdapter = d.ReadPointer();
            args.configSpaceSize = d.ReadUint32();
            args.configSpaceData = d.ReadBytes(args.configSpaceSize);
            args.chainUid = d.ReadUint32();
            args.numberOfLinksInChain = d.ReadUint32();
            args.leadLink = d.ReadUint32();
            args.busType = d.ReadUint32(); // DISPLAYCONFIG_BUSTYPE
            args.vendorId = d.ReadUint32();
            args.deviceId = d.ReadUint32();
            args.subVendorId = d.ReadUint32();
            args.subSystemId = d.ReadUint32();
            args.revisionId = d.ReadUint32();
            args.adapterLuid = d.ReadUint64();

            (m_obj->*target)(
                record,
                args);
        }

        template<typename TEventArgs>
        void Template_DxgkNodeMetadata(EVENT_RECORD* record, void (T::* target)(EVENT_RECORD*, TEventArgs))
        {
            EventData d(record);

            DxgkNodeMetadataArgs args;
            args.dxgAdapter = d.ReadPointer();
            args.nodeOrdinal = d.ReadUint32();
            args.engineType = (DXGK_ENGINE_TYPE)d.ReadUint32(); // DXGK_ENGINE_TYPE
            args.friendlyName = d.ReadRemainingAsUnicodeString();

            (m_obj->*target)(
                record,
                args);
        }

        template<typename TEventArgs>
        void Template_DxgkHwQueue(EVENT_RECORD* record, void (T::* target)(EVENT_RECORD*, TEventArgs))
        {
            EventData d(record);

            DxgkHwQueueArgs args;
            args.parentDxgContext = d.ReadPointer();
            args.hwQueueHandle = d.ReadPointer();
            args.hwQueue = d.ReadPointer();

            (m_obj->*target)(
                record,
                args);
        }

        template<typename TEventArgs>
        void Template_DxgkDmaReleaseToGpu(EVENT_RECORD* record, void (T::* target)(EVENT_RECORD*, TEventArgs))
        {
            EventData d(record);

            DxgkDmaReleaseToGpuArgs args;
            args.hHwQueue = d.ReadPointer();
            args.progressFenceValue = d.ReadUint64();
            args.pDmaBuffer = d.ReadPointer();
            args.ntStatus = d.ReadUint32();
            args.numberOfQueuedPendingFlip = d.ReadUint32();

            (m_obj->*target)(
                record,
                args);
        }

        template<typename TEventArgs>
        void Template_DxgkDmaCompleteByGpu(EVENT_RECORD* record, void (T::* target)(EVENT_RECORD*, TEventArgs))
        {
            EventData d(record);

            DxgkDmaCompleteByGpuArgs args;
            args.hHwQueue = d.ReadPointer();
            args.progressFenceValue = d.ReadUint64();

            (m_obj->*target)(
                record,
                args);
        }

        template<typename TEventArgs>
        void Template_DxgkDmaSubmit(EVENT_RECORD* record, void (T::* target)(EVENT_RECORD*, TEventArgs))
        {
            EventData d(record);

            DxgkDmaSubmitArgs args;
            args.hContext = d.ReadPointer();
            args.hQueuePacketContext = d.ReadPointer();
            args.packetType = d.ReadUint32();
            args.submissionId = d.ReadUint64();
            // The rest of the payload is fields we don't use.

            (m_obj->*target)(
                record,
                args);
        }

        template<typename TEventArgs>
        void Template_DxgkDmaIsrComplete(EVENT_RECORD* record, void (T::* target)(EVENT_RECORD*, TEventArgs))
        {
            EventData d(record);

            DxgkDmaIsrCompleteArgs args;
            args.hContext = d.ReadPointer();
            args.packetType = d.ReadUint32();
            args.completionId = d.ReadUint64();
            // The rest of the payload is fields we don't use.

            (m_obj->*target)(
                record,
                args);
        }

#pragma warning(push)
#pragma warning(disable: 4189)  // local variable is initialized but not referenced
        template<typename TEventArgs>
        void Template_DxgkAdapterAllocation(EVENT_RECORD* record, void (T::* target)(EVENT_RECORD*, TEventArgs))
        {
            EventData d(record);

            DxgkAdapterAllocationArgs args;
            args.hProcessId = d.ReadPointer();
            args.hDevice = d.ReadPointer();
            args.pDxgAdapter = d.ReadPointer();
            args.flags = d.ReadUint32();
            args.allocSize = d.ReadUint64();
            args.alignment = d.ReadUint32();
            args.readSegment = d.ReadUint32();
            args.writeSegment = d.ReadUint32();
            args.preferredSegment = d.ReadUint32();
            args.hintedBank = d.ReadUint32();
            args.evictionSegment = d.ReadUint32();
            args.priority = d.ReadUint32();
            args.hVidMmGlobalAlloc = d.ReadPointer();
            args.hDxgGlobalAlloc = d.ReadPointer();
            args.hDxgSharedResource = d.ReadPointer();
            args.usageVersion = d.ReadUint32();
            args.usageFlags = d.ReadUint32();
            args.d3dFormat = d.ReadUint32();
            args.swizzledFormat = d.ReadUint32();
            args.byteOffset = d.ReadUint32();
            args.width = d.ReadUint32();
            args.height = d.ReadUint32();
            args.pitch = d.ReadUint32();
            args.depth = d.ReadUint32();
            args.slicePitch = d.ReadUint32();
            args.backingStoreWasPinned = d.ReadUint8() > 0;
            args.pSectionObject = d.ReadPointer();
            args.physicalAdapterIndex = d.ReadUint16();
            args.pageTableOrDirection = d.ReadUint8() > 0;

            (m_obj->*target)(
                record,
                args);
        }

        template<typename TEventArgs>
        void Template_DxgkDeviceAllocation(EVENT_RECORD* record, void (T::* target)(EVENT_RECORD*, TEventArgs))
        {
            EventData d(record);

            DxgkDeviceAllocationArgs args;
            args.processId = d.ReadPointer();
            args.device = d.ReadPointer();
            args.dxgAdapter = d.ReadPointer();
            args.vidMmAlloc = d.ReadPointer();
            args.vidMmGlobalAlloc = d.ReadPointer();
            args.dxgResource = d.ReadPointer();
            args.dxgSharedResource = d.ReadPointer();
            args.thunkAllocation = d.ReadPointer();
            args.thunkResource = d.ReadPointer();
            args.privateRuntimeResourceHandle = d.ReadPointer();
            args.virtualAddress = d.ReadPointer();
            args.processAllocDetails = d.ReadPointer();

            (m_obj->*target)(
                record,
                args);
        }

        template<typename TEventArgs>
        void Template_DxgkPagingOpVirtualTransfer(EVENT_RECORD* record, void (T::* target)(EVENT_RECORD*, TEventArgs))
        {
            EventData d(record);

            DxgkPagingOpVirtualTransferArgs args;
            args.dxgAdapter = d.ReadPointer();
            args.hDmaBuffer = d.ReadPointer();
            args.continueNextBuffer = d.ReadUint32();
            args.hAllocationGlobalHandle = d.ReadPointer();
            args.allocationOffset = d.ReadUint64();
            args.transferSize = d.ReadUint64();
            args.sourceSegmentId = d.ReadUint32();
            args.destinationSegmentId = d.ReadUint32();
            args.sourceVirtualAddress = d.ReadUint64();
            args.destinationVirtualAddress = d.ReadUint64();
            args.sourcePageTable = d.ReadUint64();
            args.transferDirection = (DXGK_MEMORY_TRANSFER_DIRECTION)d.ReadUint32();
            args.transferFlags = d.ReadUint32();
            args.destinationPageTable = d.ReadUint64();
            args.sourceSegmentOffset = d.ReadUint64();
            args.destinationSegmentOffset = d.ReadUint64();

            (m_obj->*target)(
                record,
                args);
        }

        template<typename TEventArgs>
        void Template_DxgkVidMmProcessBudgetChange(EVENT_RECORD* record, void (T::* target)(EVENT_RECORD*, TEventArgs))
        {
            EventData d(record);

            DxgkVidMmProcessBudgetChangeArgs args;
            args.newBudget = d.ReadUint64();
            args.oldBudget = d.ReadUint64();
            args.dxgAdapter = d.ReadPointer();
            args.processId = d.ReadUint32();
            args.physicalAdapterIndex = d.ReadUint16();
            args.newPriorityBand = d.ReadUint8();    // VIDMM_BUDGET_PRIORITY_BAND
            args.oldPriorityBand = d.ReadUint8();    // VIDMM_BUDGET_PRIORITY_BAND
            args.newVisibilityState = d.ReadUint8(); // VIDMM_BUDGET_VISIBILITY_STATE
            args.oldVisibilityState = d.ReadUint8(); // VIDMM_BUDGET_VISIBILITY_STATE
            args.memorySegmentGroup = (D3DKMT_MEMORY_SEGMENT_GROUP)d.ReadUint8();

            (m_obj->*target)(
                record,
                args);
        }

        template<typename TEventArgs>
        void Template_DxgkVidMmProcessUsageChange(EVENT_RECORD* record, void (T::* target)(EVENT_RECORD*, TEventArgs))
        {
            EventData d(record);

            DxgkVidMmProcessUsageChangeArgs args;
            args.newUsage = d.ReadUint64();
            args.oldUsage = d.ReadUint64();
            args.dxgAdapter = d.ReadPointer();
            args.processId = d.ReadUint32();
            args.physicalAdapterIndex = d.ReadUint16();
            args.memorySegmentGroup = (D3DKMT_MEMORY_SEGMENT_GROUP)d.ReadUint8();

            (m_obj->*target)(
                record,
                args);
        }

        template<typename TEventArgs>
        void Template_DxgkVidMmProcessCommitmentChange(EVENT_RECORD* record, void (T::* target)(EVENT_RECORD*, TEventArgs))
        {
            EventData d(record);

            DxgkVidMmProcessCommitmentChangeArgs args;
            args.newCommitment = d.ReadUint64();
            args.oldCommitment = d.ReadUint64();
            args.dxgAdapter = d.ReadPointer();
            args.processId = d.ReadUint32();
            args.physicalAdapterIndex = d.ReadUint16();
            args.memorySegmentGroup = (D3DKMT_MEMORY_SEGMENT_GROUP)d.ReadUint8();

            (m_obj->*target)(
                record,
                args);
        }

        template<typename TEventArgs>
        void Template_DxgkVidMmProcessDemotedCommitmentChange(EVENT_RECORD* record, void (T::* target)(EVENT_RECORD*, TEventArgs))
        {
            EventData d(record);

            DxgkVidMmProcessDemotedCommitmentChangeArgs args;
            args.newCommitment = d.ReadUint64();
            args.oldCommitment = d.ReadUint64();
            args.dxgAdapter = d.ReadPointer();
            args.processId = d.ReadUint32();
            args.physicalAdapterIndex = d.ReadUint16();
            args.priorityClass = d.ReadUint8(); // VIDMM_ALLOCATION_PRIORITY_CLASS

            (m_obj->*target)(
                record,
                args);
        }
#pragma warning(pop)

        template<typename TEventArgs>
        void Template_DxgkVidMmMakeResident(EVENT_RECORD* record, void (T::* target)(EVENT_RECORD*, TEventArgs))
        {
            EventData d(record);

            DxgkVidMmMakeResidentArgs args;
            args.vidMmAlloc = d.ReadPointer();
            args.residencyCount = d.ReadUint32();

            (m_obj->*target)(
                record,
                args);
        }

        template<typename TEventArgs>
        void Template_DxgkVidMmEvict(EVENT_RECORD* record, void (T::* target)(EVENT_RECORD*, TEventArgs))
        {
            EventData d(record);

            DxgkVidMmEvictArgs args;
            args.vidMmAlloc = d.ReadPointer();
            args.residencyCount = d.ReadUint32();

            (m_obj->*target)(
                record,
                args);
        }

        template<typename TEventArgs>
        void Template_DxgkReportSegment(EVENT_RECORD* record, void (T::* target)(EVENT_RECORD*, TEventArgs))
        {
            EventData d(record);

            DxgkReportSegmentArgs args;
            args.segmentId = d.ReadUint32();
            args.dxgAdapter = d.ReadPointer();
            args.baseAddress = d.ReadUint64();
            args.cpuTranslatedAddress = d.ReadUint64();
            args.size = d.ReadUint64();
            args.numberOfBanks = d.ReadUint32();
            args.flags = d.ReadUint32();
            args.commitLimit = d.ReadUint64();
            args.systemMemoryEndAddress = d.ReadPointer();
            args.memorySegmentGroup = (D3DKMT_MEMORY_SEGMENT_GROUP)d.ReadUint8();

            (m_obj->*target)(
                record,
                args);
        }

        template<typename TEventArgs>
        void Template_DxgkPagingOpVirtualFill(EVENT_RECORD* record, void (T::* target)(EVENT_RECORD*, TEventArgs))
        {
            EventData d(record);

            DxgkPagingOpVirtualFillArgs args;
            args.dxgAdapter = d.ReadPointer();
            args.dmaBuffer = d.ReadUint64();
            args.continueNextBuffer = (bool)d.ReadUint32();
            args.allocationGlobalHandle = d.ReadUint64();
            args.allocationOffset = d.ReadUint64();
            args.fillSize = d.ReadUint64();
            args.fillPattern = d.ReadUint32();
            args.segmentId = d.ReadUint32();
            args.destinationVirtualAddress = d.ReadUint64();
            args.segmentOffset = d.ReadUint64();

            (m_obj->*target)(
                record,
                args);
        }

        template<typename TEventArgs>
        void Template_DxgkPagingOpSysmemCommit(EVENT_RECORD* record, void (T::* target)(EVENT_RECORD*, TEventArgs))
        {
            EventData d(record);

            DxgkPagingOpSysmemCommitArgs args;
            args.dxgAdapter = d.ReadPointer();
            args.allocationGlobalHandle = d.ReadPointer();
            args.segmentId = d.ReadUint32();

            (m_obj->*target)(
                record,
                args);
        }

        template<typename TEventArgs>
        void Template_DxgkPagingOpMapApertureSegment(EVENT_RECORD* record, void (T::* target)(EVENT_RECORD*, TEventArgs))
        {
            EventData d(record);

            DxgkPagingOpMapApertureSegmentArgs args;
            args.dxgAdapter = d.ReadPointer();
            args.dmaBuffer = d.ReadPointer();
            args.continueNextBuffer = (bool)d.ReadUint32();
            args.allocationGlobalHandle = d.ReadPointer();
            args.segmentId = d.ReadUint32();
            args.offsetInPages = d.ReadUint64();
            args.numberOfPages = d.ReadUint64();
            args.flags = d.ReadUint32();
            args.evictionResource = (bool)d.ReadUint32();

            (m_obj->*target)(
                record,
                args);
        }

        template<typename TEventArgs>
        void Template_DxgkMigrateAllocation(EVENT_RECORD* record, void (T::* target)(EVENT_RECORD*, TEventArgs))
        {
            EventData d(record);

            DxgkMigrateAllocationArgs args;
            args.allocationGlobalHandle = d.ReadUint64();

            (m_obj->*target)(
                record,
                args);
        }

        template<typename TEventArgs>
        void Template_DxgkCompleteAllocationMigration(EVENT_RECORD* record, void (T::* target)(EVENT_RECORD*, TEventArgs))
        {
            EventData d(record);

            DxgkCompleteAllocationMigrationArgs args;
            args.allocationGlobalHandle = d.ReadUint64();
            args.status = d.ReadUint32();

            (m_obj->*target)(
                record,
                args);
        }

        template<typename FN>
        void Template_TimingBlock(EVENT_RECORD* record, FN target)
        {
            EventData d(record);

            uint32_t extraData = d.ReadUint32();
            uint32_t bufferSize = d.ReadUint32();
            uint8_t* buffer = d.ReadArray<uint8_t>(bufferSize);

            (m_obj->*target)(
                record,
                extraData,
                bufferSize,
                buffer);
        }

        template<typename FN>
        void Template_PixCounterData(EVENT_RECORD* record, FN target)
        {
            EventData d(record);

            float counterValue = d.ReadFloat();
            std::wstring_view name = d.ReadRemainingAsUnicodeString();

            (m_obj->*target)(
                record,
                counterValue,
                name);
        }

        template<typename FN>
        void Template_PixMemoryEvent(EVENT_RECORD* record, FN target)
        {
            EventData d(record);

            UINT16 allocatorId = d.ReadUint16();
            UINT64 baseAddress = d.ReadUint64();
            UINT64 size = d.ReadUint64();
            UINT64 metadata = d.ReadUint64();

            (m_obj->*target)(
                record,
                allocatorId,
                baseAddress,
                size,
                metadata);
        }

        template<typename DATA>
        struct DirectStorageDataTag
        {
            // This works around C++'s lack of partial function specialization
        };

        template<typename DsProcessor>
        void DispatchEnqueueRequestByVersion(EVENT_RECORD* record, DsProcessor* ds)
        {
            if (record->EventHeader.EventDescriptor.Version == 1)
            {
                // DS 1.0: tuple<EnqueueRequestEventData, EndingString>
                // Explicitly cast to the 3-parameter overload
                using HandlerType = void (DsProcessor::*)(EVENT_RECORD*, DirectStorage::EnqueueRequestEventData, std::string_view);
                DispatchDirectStorageNullTerminatedEvent(
                    DirectStorageDataTag<std::tuple<DirectStorage::EnqueueRequestEventData, DirectStorage::EndingString>>{},
                    record,
                    ds,
                    static_cast<HandlerType>(&DsProcessor::OnTraceEnqueueRequestEvent));
            }
            else // DS 1.4 and above
            {
                // DS 1.4: tuple<EnqueueRequestEventData, EndingString, EnqueueRequestEventExtendedData>
                // Explicitly cast to the 4-parameter overload
                using HandlerType = void (DsProcessor::*)(EVENT_RECORD*, DirectStorage::EnqueueRequestEventData, std::string_view, DirectStorage::EnqueueRequestEventExtendedData);
                DispatchDirectStorageNullTerminatedEvent(
                    DirectStorageDataTag<decltype(DirectStorage::EnqueueRequestEvent::Data)>{},
                    record,
                    ds,
                    static_cast<HandlerType>(&DsProcessor::OnTraceEnqueueRequestEvent));
            }
        }

#define DISPATCH_DIRECTSTORAGE(event)                                                                            \
    case static_cast<int>(DirectStorage::event::Desc.Id):                                                        \
        DispatchDirectStorageEvent(DirectStorageDataTag<decltype(DirectStorage::event::Data)>{}, record, ds, &DsProcessor::OnTrace##event); \
        break

        // See src\inc\dstorage_etw.h
        void DispatchDirectStorageTrace(EVENT_RECORD* record)
        {
            using DsProcessor = std::remove_pointer_t<decltype(m_obj->GetDirectStorageProcessor())>;
            DsProcessor* ds = m_obj->GetDirectStorageProcessor();

            switch (record->EventHeader.EventDescriptor.Id)
            {
                case static_cast<int>(DirectStorage::EnqueueRequestEvent::Desc.Id):
                    DispatchEnqueueRequestByVersion(record, ds);
                    break;

                DISPATCH_DIRECTSTORAGE(OpenFileEvent);
                DISPATCH_DIRECTSTORAGE(CloseFileEvent);
                DISPATCH_DIRECTSTORAGE(CreateQueueEvent);
                DISPATCH_DIRECTSTORAGE(CloseQueueEvent);
                DISPATCH_DIRECTSTORAGE(RequestCompletedEvent);
                DISPATCH_DIRECTSTORAGE(EnqueueStatusEvent);
                DISPATCH_DIRECTSTORAGE(StatusCompletedEvent);
                DISPATCH_DIRECTSTORAGE(EnqueueSignalEvent);
                DISPATCH_DIRECTSTORAGE(SignalCompletedEvent);
                DISPATCH_DIRECTSTORAGE(SubmitEvent);
                DISPATCH_DIRECTSTORAGE(EnqueueSetEvent);
                DISPATCH_DIRECTSTORAGE(SetEventCompletedEvent);
            }
        }

#undef DISPATCH_DIRECTSTORAGE

        template<typename DATA, typename Obj, typename FN>
        void DispatchDirectStorageEvent(DirectStorageDataTag<DATA>, EVENT_RECORD* record, Obj* obj, FN&& target)
        {
            if constexpr (is_tuple<DATA>::value)
            {
                constexpr size_t numArgs = std::tuple_size_v<DATA>;
                DispatchDirectStorageMultipartEvent<DATA>(record, obj, std::forward<FN>(target), std::make_index_sequence<numArgs>{});
            }
            else
            {
                EventData d(record);
                (obj->*target)(record, d.Load<DATA>());
            }
        }

        template<typename DATA, typename Obj, typename FN>
        void DispatchDirectStorageNullTerminatedEvent(DirectStorageDataTag<DATA>, EVENT_RECORD* record, Obj* obj, FN&& target)
        {
            if constexpr (is_tuple<DATA>::value)
            {
                constexpr size_t numArgs = std::tuple_size_v<DATA>;
                DispatchDirectStorageMultipartNullTerminatedEvent<DATA>(record, obj, std::forward<FN>(target), std::make_index_sequence<numArgs>{});
            }
            else
            {
                EventData d(record);
                (obj->*target)(record, d.LoadNullTerminated<DATA>());
            }
        }

        template<typename DATA, typename Obj, typename FN, std::size_t... INDICES>
        void DispatchDirectStorageMultipartEvent(EVENT_RECORD* record, Obj* obj, FN&& target, std::index_sequence<INDICES...>)
        {
            EventData d(record);
            DATA args{d.Load<std::tuple_element_t<INDICES, DATA>>()...};

            std::apply([&](auto&&... unpacked) { (obj->*target)(record, unpacked...); }, std::move(args));
        }

        template<typename DATA, typename Obj, typename FN, std::size_t... INDICES>
        void DispatchDirectStorageMultipartNullTerminatedEvent(EVENT_RECORD* record, Obj* obj, FN&& target, std::index_sequence<INDICES...>)
        {
            EventData d(record);
            DATA args{d.LoadNullTerminated<std::tuple_element_t<INDICES, DATA>>()...};

            std::apply([&](auto&&... unpacked) { (obj->*target)(record, unpacked...); }, std::move(args));
        }

    };
} // namespace DirectX::Etw
