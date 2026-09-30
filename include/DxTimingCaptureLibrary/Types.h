// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#pragma once

// Public data types for DxTimingCaptureLibrary.
//
// Generally the callbacks from DxTimingCaptureLibrary try to use standard D3D12 or PIX data types.
// The types in this file cover additional data that don't have standard header equivalents.

#include <d3d12.h>
#include <cstdint>

#include <DecodedPixEventTypes.h> // PixEventType, PixCpuEvent (MIT-licensed, ships with WinPixEventRuntime)

namespace DirectX::Etw
{
    // Flags describing a reported counter.
    enum class CounterFlags : uint32_t
    {
        None = 0x00000000,
        IsInternal = 0x00000001,
        IsFractional = 0x00000002,
    };

    // Whether a counter budget represents an upper or lower bound.
    enum class CounterBudgetType
    {
        Maximum,
        Minimum,
    };

    // Whether a counter budget was imposed by the system or chosen by the user.
    enum class CounterBudgetSource
    {
        System,
        User,
    };

    // ------------------------------------------------------------------
    // D3D12 API objects
    // ------------------------------------------------------------------

    // The kind of a tracked D3D12 API object.
    enum class ApiObjectType
    {
        Invalid = 0,
        Device = 1,
        Resource = 2,
        Heap = 3,
        PipelineState = 4,
        StateObject = 5,
        CommandAllocator = 6,
        DescriptorHeap = 7,
        MetaCommand = 8,
        Max = 9,
    };

    // Metadata for a created device. There is no public D3D12 struct that
    // captures this, so the library reports it directly.
    struct DeviceInfo
    {
        D3D_FEATURE_LEVEL FeatureLevel;
        UINT32 KmAdapter;
        UINT64 UmAdapter;
        UINT64 UmAdapterVersion;
        UINT32 KmDevice;
        UINT64 UmDeviceVersion;
    };

    // Which memory pool a GPU allocation lives in. Values match
    // D3DKMT_MEMORY_SEGMENT_GROUP so kernel values cast straight across.
    // Local = video memory, NonLocal = system memory, Unknown = not seen yet
    // (or no GPU allocation, e.g. a CPU-visible descriptor heap).
    enum class MemorySegmentGroup : uint32_t
    {
        Unknown = 0xFFFFFFFF,
        Local = 0,
        NonLocal = 1,
    };

    // The GPU virtual address range an object occupies. Accompanies the
    // D3D12 description in the creation callbacks (the description itself does
    // not carry an address).
    struct ObjectPlacementInfo
    {
        UINT64 GpuVirtualAddress;
        UINT64 GpuVirtualSize;

        // Where the backing allocation lives right now. Often Unknown at creation
        // time; watch OnAllocationSegmentGroupChanges for updates.
        MemorySegmentGroup ResidentSegmentGroup = MemorySegmentGroup::Unknown;
    };

    // Tiling information for a reserved (tiled) resource.
    struct ReservedResourceInfo
    {
        UINT32 NumTilesForResource;
        UINT32 NumPackedMips;
        UINT32 NumTilesForPackedMips;
    };

    // ------------------------------------------------------------------
    // GPU residency / allocation migration
    // ------------------------------------------------------------------

    enum class ResidencyOperationType
    {
        MakeResident = 0,
        Evict = 1,
        PageIn = 2,
        PageOut = 3,
    };

    struct ResidencyOperation
    {
        INT64 Timestamp;
        UINT64 ObjectId;
        UINT32 ResidencyCount;
        ResidencyOperationType OperationType;
        ApiObjectType ObjectType;
    };

    struct DemotedAllocation
    {
        UINT64 ObjectId;
        INT64 Timestamp;
    };

    enum class AllocationMigrationResult
    {
        Failed = 0,
        Succeeded = 1,
    };

    struct AllocationMigration
    {
        UINT64 ObjectId;
        INT64 StartTime;
        INT64 EndTime;
        AllocationMigrationResult Result;
    };

    // An object's backing allocation moved between video and system memory
    // (paged out, or promoted back). Combined with the group reported at
    // creation, this tracks where every object lives over time.
    struct AllocationSegmentGroupChange
    {
        UINT64 ObjectId;
        INT64 Timestamp;
        ApiObjectType ObjectType;
        MemorySegmentGroup SegmentGroup;
    };

    // ------------------------------------------------------------------
    // PIX-runtime events
    // ------------------------------------------------------------------

    // A GPU-timeline event correlated from PIX runtime markers.
    struct GpuEvent
    {
        UINT64 CpuContextId;
        UINT64 ApiCommandQueueId;
        INT64 Timestamp;
        PixEventType Type;
    };

    enum class MemoryOperation
    {
        Allocate,
        Free,
    };

    // A PIX custom memory allocation/free event (PIXReportMemoryAllocation /
    // PIXReportMemoryFree).
    struct PixMemoryEvent
    {
        INT64 Timestamp;
        UINT32 ProcessId;
        UINT32 ThreadId;
        UINT16 AllocatorId;
        UINT64 BaseAddress;
        UINT64 Size;
        UINT64 UserData;
        MemoryOperation Operation;
    };
}
