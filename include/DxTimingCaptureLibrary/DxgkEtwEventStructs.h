// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#pragma once

#include <string_view>
#include "EventData.h"
#include "EventTypes.h"
#include <d3dkmthk.h>

// Strongly-typed event structs representing kernel ETW events, see LDDMCore.man

namespace DirectX::Etw
{
    struct DxgkVSyncDPCArgs
    {
        uint64_t dxgAdapter;
        uint32_t vidPnTargetId;
        uint64_t scannedPhysicalAddress;
        uint32_t vidPnSourceId;
        uint32_t frameNumber;
        int64_t frameQPCTime;
        uint64_t flipDevice;
        uint32_t flipType;
        uint64_t flipFenceId;
    };

    struct DxgkCalibrateGpuClockArgs
    {
        uint64_t adapter;
        uint32_t nodeOrdinal;
        uint32_t engineOrdinal;
        uint64_t gpuFrequency;
        uint64_t gpuClock;
        uint64_t cpuClock;
    };

    struct DxgkHistoryBufferArgs
    {
        uint64_t context;
        uint32_t renderCbSequence;
        uint32_t dmaSubmissionSequence;
        uint32_t precision;
        uint32_t historyBufferSize;
        uint8_t* historyBuffer;
    };

    struct DxgkContextArgs
    {
        uint64_t device;
        uint32_t nodeOrdinal;
        uint32_t engineAffinity;
        uint32_t dmaBufferSize;
        uint32_t dmaBufferSegmentSet;
        uint32_t dmaBufferPrivateDataSize;
        uint32_t allocationListSize;
        uint32_t patchAllocationListSize;
        uint32_t contextFlags;
        uint64_t context;
        uint64_t contextHandle;
        uint64_t parentDxgContext;
    };

    struct DxgkDeviceArgs
    {
        uint64_t processId;
        uint64_t dxgAdapter;
        uint32_t clientType;
        uint64_t device;
        uint32_t requestVsync;
        uint32_t disableGpuTimeout;
        uint32_t thunkHandle;
    };

    struct DxgkDpiReportAdapterArgs
    {
        uint64_t dxgAdapter;
        uint32_t configSpaceSize;
        uint8_t* configSpaceData;
        uint32_t chainUid;
        uint32_t numberOfLinksInChain;
        uint32_t leadLink;
        uint32_t busType; // DISPLAYCONFIG_BUSTYPE
        uint32_t vendorId;
        uint32_t deviceId;
        uint32_t subVendorId;
        uint32_t subSystemId;
        uint32_t revisionId;
        uint64_t adapterLuid;
    };

    struct DxgkNodeMetadataArgs
    {
        uint64_t dxgAdapter;
        uint32_t nodeOrdinal;
        DXGK_ENGINE_TYPE engineType; // DXGK_ENGINE_TYPE
        std::wstring_view friendlyName;
    };

    struct DxgkHwQueueArgs
    {
        uint64_t parentDxgContext;
        uint64_t hwQueueHandle;
        uint64_t hwQueue;
    };

    struct DxgkDmaReleaseToGpuArgs
    {
        uint64_t hHwQueue;
        uint64_t progressFenceValue;
        uint64_t pDmaBuffer;
        uint32_t ntStatus;
        uint32_t numberOfQueuedPendingFlip;
    };

    struct DxgkDmaCompleteByGpuArgs
    {
        uint64_t hHwQueue;
        uint64_t progressFenceValue;
    };

    struct DxgkDmaSubmitArgs
    {
        uint64_t hContext;
        uint64_t hQueuePacketContext;
        uint32_t packetType;
        uint64_t submissionId;
    };

    struct DxgkDmaIsrCompleteArgs
    {
        uint64_t hContext;
        uint32_t packetType;
        uint64_t completionId;
    };

    struct DxgkAdapterAllocationArgs
    {
        uint64_t hProcessId;
        uint64_t hDevice;
        uint64_t pDxgAdapter;
        uint32_t flags;
        uint64_t allocSize;
        uint32_t alignment;
        uint32_t readSegment;
        uint32_t writeSegment;
        uint32_t preferredSegment;
        uint32_t hintedBank;
        uint32_t evictionSegment;
        uint32_t priority;
        uint64_t hVidMmGlobalAlloc;
        uint64_t hDxgGlobalAlloc;
        uint64_t hDxgSharedResource;
        uint32_t usageVersion;
        uint32_t usageFlags;
        uint32_t d3dFormat;
        uint32_t swizzledFormat;
        uint32_t byteOffset;
        uint32_t width;
        uint32_t height;
        uint32_t pitch;
        uint32_t depth;
        uint32_t slicePitch;
        bool backingStoreWasPinned;
        uint64_t pSectionObject;
        uint16_t physicalAdapterIndex;
        bool pageTableOrDirection;
    };

    struct DxgkDeviceAllocationArgs
    {
        uint64_t processId;
        uint64_t device;
        uint64_t dxgAdapter;
        uint64_t vidMmAlloc;
        uint64_t vidMmGlobalAlloc;
        uint64_t dxgResource;
        uint64_t dxgSharedResource;
        uint64_t thunkAllocation;
        uint64_t thunkResource;
        uint64_t privateRuntimeResourceHandle;
        uint64_t virtualAddress;
        uint64_t processAllocDetails;
    };

    enum DXGK_MEMORY_TRANSFER_DIRECTION
    {
        DXGK_MEMORY_TRANSFER_LOCAL_TO_SYSTEM = 0,
        DXGK_MEMORY_TRANSFER_SYSTEM_TO_LOCAL = 1,
        DXGK_MEMORY_TRANSFER_LOCAL_TO_LOCAL = 2,
    };

    struct DxgkPagingOpVirtualTransferArgs
    {
        uint64_t dxgAdapter;
        uint64_t hDmaBuffer;
        uint32_t continueNextBuffer;
        uint64_t hAllocationGlobalHandle;
        uint64_t allocationOffset;
        uint64_t transferSize;
        uint32_t sourceSegmentId;
        uint32_t destinationSegmentId;
        uint64_t sourceVirtualAddress;
        uint64_t destinationVirtualAddress;
        uint64_t sourcePageTable;
        DXGK_MEMORY_TRANSFER_DIRECTION transferDirection;
        uint32_t transferFlags;
        uint64_t destinationPageTable;
        uint64_t sourceSegmentOffset;
        uint64_t destinationSegmentOffset;
    };

    struct DxgkVidMmProcessBudgetChangeArgs
    {
        uint64_t newBudget;
        uint64_t oldBudget;
        uint64_t dxgAdapter;
        uint32_t processId;
        uint16_t physicalAdapterIndex;
        uint8_t newPriorityBand;    // VIDMM_BUDGET_PRIORITY_BAND
        uint8_t oldPriorityBand;    // VIDMM_BUDGET_PRIORITY_BAND
        uint8_t newVisibilityState; // VIDMM_BUDGET_VISIBILITY_STATE
        uint8_t oldVisibilityState; // VIDMM_BUDGET_VISIBILITY_STATE
        D3DKMT_MEMORY_SEGMENT_GROUP memorySegmentGroup;
    };

    struct DxgkVidMmProcessUsageChangeArgs
    {
        uint64_t newUsage;
        uint64_t oldUsage;
        uint64_t dxgAdapter;
        uint32_t processId;
        uint16_t physicalAdapterIndex;
        D3DKMT_MEMORY_SEGMENT_GROUP memorySegmentGroup;
    };

    struct DxgkVidMmProcessCommitmentChangeArgs
    {
        uint64_t newCommitment;
        uint64_t oldCommitment;
        uint64_t dxgAdapter;
        uint32_t processId;
        uint16_t physicalAdapterIndex;
        D3DKMT_MEMORY_SEGMENT_GROUP memorySegmentGroup;
    };

    struct DxgkVidMmProcessDemotedCommitmentChangeArgs
    {
        uint64_t newCommitment;
        uint64_t oldCommitment;
        uint64_t dxgAdapter;
        uint32_t processId;
        uint16_t physicalAdapterIndex;
        uint8_t priorityClass; // VIDMM_ALLOCATION_PRIORITY_CLASS
    };

    struct DxgkVidMmMakeResidentArgs
    {
        uint64_t vidMmAlloc;
        uint32_t residencyCount;
    };

    struct DxgkVidMmEvictArgs : DxgkVidMmMakeResidentArgs
    {};

    struct DxgkReportSegmentArgs
    {
        //<data name = "ulSegmentId" inType = "win:UInt32" / >
        uint32_t segmentId;
        //<data name = "pDxgAdapter" inType = "win:Pointer" / >
        uint64_t dxgAdapter;
        //<data name = "BaseAddress" inType = "win:UInt64" / >
        uint64_t baseAddress;
        //<data name = "CpuTranslatedAddress" inType = "win:UInt64" / >
        uint64_t cpuTranslatedAddress;
        //<data name = "Size" inType = "win:UInt64" / >
        uint64_t size;
        //<data name = "NbOfBanks" inType = "win:UInt32" / >
        uint32_t numberOfBanks;
        //<data name = "Flags" inType = "win:UInt32" / >
        uint32_t flags;
        //<data name = "CommitLimit" inType = "win:UInt64" / >
        uint64_t commitLimit;
        //<data name = "SystemMemoryEndAddress" inType = "win:Pointer" / >
        uint64_t systemMemoryEndAddress;
        //<data name = "MemorySegmentGroup" inType = "win:UInt8" / >
        D3DKMT_MEMORY_SEGMENT_GROUP memorySegmentGroup;
    };

    struct DxgkPagingOpVirtualFillArgs
    {
        //<data name = "pDxgAdapter" inType = "win:Pointer" / >
        uint64_t dxgAdapter;
        //<data name = "hDmaBuffer" inType = "win:Pointer" / >
        uint64_t dmaBuffer;
        //<data name = "ContinueNextBuffer" inType = "win:Boolean" / >
        bool continueNextBuffer;
        //<data name = "hAllocationGlobalHandle" inType = "win:Pointer" / >
        uint64_t allocationGlobalHandle;
        //<data name = "AllocationOffset" inType = "win:UInt64" / >
        uint64_t allocationOffset;
        //<data name = "FillSize" inType = "win:UInt64" / >
        uint64_t fillSize;
        //<data name = "FillPattern" inType = "win:UInt32" / >
        uint32_t fillPattern;
        //<data name = "SegmentId" inType = "win:UInt32" / >
        uint32_t segmentId;
        //<data name = "DestinationVirtualAddress" inType = "win:UInt64" / >
        uint64_t destinationVirtualAddress;
        //<data name = "SegmentOffset" inType = "win:UInt64" / >
        uint64_t segmentOffset;
    };

    struct DxgkPagingOpSysmemCommitArgs
    {
        //<data name = "pDxgAdapter" inType = "win:Pointer" / >
        uint64_t dxgAdapter;
        //<data name = "hAllocationGlobalHandle" inType = "win:Pointer" / >
        uint64_t allocationGlobalHandle;
        //<data name = "SegmentId" inType = "win:UInt32" / >
        uint32_t segmentId;
    };

    struct DxgkPagingOpMapApertureSegmentArgs
    {
        //<data name = "pDxgAdapter" inType = "win:Pointer" / >
        uint64_t dxgAdapter;
        //<data name = "hDmaBuffer" inType = "win:Pointer" / >
        uint64_t dmaBuffer;
        //<data name = "ContinueNextBuffer" inType = "win:Boolean" / >
        bool continueNextBuffer;
        //<data name = "hAllocationGlobalHandle" inType = "win:Pointer" / >
        uint64_t allocationGlobalHandle;
        //<data name = "SegmentId" inType = "win:UInt32" / >
        uint32_t segmentId;
        //<data name = "OffsetInPages" inType = "win:UInt64" / >
        uint64_t offsetInPages;
        //<data name = "NumberOfPages" inType = "win:UInt64" / >
        uint64_t numberOfPages;
        //<data name = "Flags" inType = "win:UInt32" / >
        uint32_t flags;
        //<data name = "EvictionResource" inType = "win:Boolean" / >
        bool evictionResource;
    };

    struct DxgkMigrateAllocationArgs
    {
        uint64_t allocationGlobalHandle;
    };

    struct DxgkCompleteAllocationMigrationArgs
    {
        uint64_t allocationGlobalHandle;
        uint32_t status;
    };
} // namespace DirectX::Etw
