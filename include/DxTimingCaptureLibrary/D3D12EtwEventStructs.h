// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#pragma once

#include <string_view>
#include "EventData.h"
#include "EventTypes.h"

// ETW dispatcher argument structs.
// These are just strongly-typed structs based on the D3D12 ETW event manifest.

namespace DirectX::Etw
{
    struct D3D12NamedObjectArgs
    {
        uint64_t object;
        std::string_view name;
    };

    struct D3D12NamedObjectWideArgs
    {
        uint64_t object;
        std::wstring_view name;
    };

    struct D3D12RenameObjectArgs
    {
        uint64_t object;
        std::string_view oldName;
        std::string_view newName;
    };

    struct D3D12RenameObjectWideArgs
    {
        uint64_t object;
        std::wstring_view oldName;
        std::wstring_view newName;
    };

    struct D3D12DeviceArgs
    {
        uint64_t device;
        uint32_t featureLevel;
        uint32_t kmAdapter;
        uint64_t umAdapter;
        uint64_t umAdapterVersion;
        uint32_t kmDevice;
        uint64_t umDeviceVersion;
    };

    struct D3D12CommandListArgs
    {
        uint64_t device;
        uint64_t commandList;
        uint64_t sequenceNumber;
        uint32_t commandListType;
    };

    struct D3D12CommandQueueArgs
    {
        uint64_t device;
        uint64_t commandQueue;
        uint32_t commandListType;
        uint32_t contextCount;
        uint32_t* contexts;
        int32_t priority;
        uint32_t commandQueueFlags;
        uint32_t nodeMask;
    };

    struct D3D12HeapArgs
    {
        uint64_t device;
        uint64_t heap;
        uint64_t sizeInBytes;
        uint64_t alignment;
        uint32_t type;
        uint32_t cpuPageProperty;
        uint32_t memoryPoolPreference;
        uint32_t creationNodeMask;
        uint32_t visibleNodeMask;
        uint32_t flags;
        uint64_t conjoinedResource;
        uint32_t kmAllocation;
        uint32_t kmResource;
    };

    struct D3D12ResourceArgs
    {
        uint64_t device;
        uint64_t resource;
        uint64_t umResource;
        uint32_t dimension;
        uint64_t width;
        uint32_t height;
        uint32_t depth;
        uint32_t mipLevels;
        uint32_t arraySize;
        uint32_t planeCount;
        uint32_t format;
        uint32_t sampleCount;
        uint32_t sampleQuality;
        uint32_t layout;
        uint32_t flags;
        uint32_t heapType;
        uint64_t heap;
        uint64_t immutableHeapOffset;
        uint64_t placedAlignment;
        uint64_t placedSize;
        uint32_t numTilesForResource;
        uint32_t numPackedMips;
        uint32_t numTilesForPackedMips;
        uint64_t immutableBuffer;
        uint64_t immutableBufferOffset;
    };

    struct D3D12GraphicsPipelineStateArgs
    {
        uint64_t device;
        uint64_t object;
    };

    struct D3D12StateObjectArgs
    {
        uint64_t device;
        uint64_t object;
    };
    
    struct D3D12CommandAllocatorArgs
    {
        uint64_t device;
        uint64_t object;
        uint32_t commandListType;
    };

    struct D3D12DescriptorHeapArgs
    {
        uint64_t device;
        uint64_t object;
        uint32_t descriptorHeapType;
        uint32_t numDescriptors;
        uint32_t descriptorHeapFlags;
        uint32_t nodeMask;
    };

    struct D3D12MetaCommandArgs
    {
        uint64_t device;
        uint64_t object;
        GUID commandId;
    };

    struct D3D12AllocationInfoArgs
    {
        uint64_t device;
        uint64_t object;
        uint32_t numVirtualAddressInfos;
        VirtualAddressInfos* virtualAddressInfos;
        uint32_t numKMTInfos;
        KMTInfos* kmtInfos;
    };

    struct D3D12MarkerArgs
    {
        uint64_t commandList;
        uint64_t apiSequenceNumber;
        uint32_t metadata;
        uint32_t dataSize;
        uint8_t* data;
    };

    struct D3D12RuntimeMarkerDataArgs
    {
        uint64_t cpuFrequency;
        uint64_t firstApiSequenceNumber;
        uint64_t commandList;
        uint32_t cpuTimeHigh;
        uint8_t threadIdCount;
        uint32_t* threadIds;
        uint32_t dataSize;
        uint8_t* data;
    };

    struct D3D12CommandBufferSubmissionArgs
    {
        uint64_t commandQueue;
        uint32_t contextCount;
        uint32_t* contexts;
        uint32_t loopIteration;
        uint32_t submitCommandCbSequence;
        uint32_t firstApiSequenceNumberHigh;
        uint32_t completedApiSequenceNumberSize;
        uint32_t* completedApiSequenceNumbers;
        uint64_t commandList;
    };

    struct D3D12HistoryBufferCompletionArgs
    {
        uint64_t hwQueueHandle;
        uint32_t renderCbSequence;
        uint64_t hwQueueProgressFenceId;
        uint32_t precision;
        uint32_t historyBufferSize;
        uint8_t* historyBuffer;
    };

    struct D3D12ExecuteCommandListArgs
    {
        uint64_t commandQueue;
        uint64_t commandList;
    };

    struct D3D12ExecuteCommandListsArgs
    {

        uint64_t commandQueue;
        uint32_t commandListCount;
        // note: PointerRange doesn't have a default constructor,
        // so you'll have to initialize this struct with an initializer list
        PointerRange commandLists;
    };

    struct D3D12CreatePipelineStateObjectArgs
    {};

    struct D3D12CreateStateObjectArgs
    {};

    struct D3D12AddToStateObjectArgs
    {};

    struct D3D12CacheStatisticsArgs
    {
        uint32_t numRequiredLookups;
        uint32_t numRequiredHitsInPSDB;
        uint32_t numRequiredHitsInDynamicCache;
        uint32_t numIgnoredHits;
        uint32_t numOptionalLookups;
        uint32_t numOptionalHitsInPSDB;
        uint32_t numOptionalHitsInDynamicCache;
        uint32_t numDynamicCacheStores;
    };

    struct D3D12JournalEntryArgs
    {
        uint32_t index;
        uint32_t code;
        uint32_t threadId;
        std::string_view message;
    };

} // namespace DirectX::Etw
