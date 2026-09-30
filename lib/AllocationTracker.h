// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#pragma once

#include <optional>
#include <unordered_map>
#include <vector>

#include <DxTimingCaptureLibrary/EventTypes.h>
#include <DxTimingCaptureLibrary/Types.h>

namespace DirectX::Etw
{

// Class to help work with ETW allocation events like
// D3D12 AllocationInfo and DXGK DeviceAllocation
class AllocationTracker
{
public:
    using AllocationInfoEntry = std::tuple<uint64_t, AllocationInfo>;
    using AllocationInfoIterator = std::vector<AllocationInfoEntry>::const_iterator;
    using Handle = size_t;

    struct DeviceAllocation
    {
        uint64_t Timestamp;
        uint64_t VidMmAlloc;
        uint64_t VidMmGlobalAlloc;
        uint64_t ThunkAllocation;
    };
    using DeviceAllocationIterator = std::vector<DeviceAllocation>::const_iterator;

    struct AdapterAllocation
    {
        uint64_t DxgAdapter;        // correlate with Adapters::PerAdapterInfo
        uint64_t VidMmGlobalAlloc;  // correlate with DeviceAllocation
        uint32_t PreferredSegment;  // see https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dukmdt/ns-d3dukmdt-_d3dddi_segmentpreference
    };

    Handle AddAllocationInfo(uint64_t timestamp, AllocationInfo info);

    const AllocationInfo& GetAllocationInfo(Handle handle) const;

    void AddDeviceAllocation(DeviceAllocation allocation);

    // vidMmAlloc and vidMmGlobalAlloc uniquely identify a device allocation
    void EraseDeviceAllocationByVidMm(uint64_t vidMmAlloc, uint64_t vidMmGlobalAlloc);

    void AddAdapterAllocation(AdapterAllocation allocation);
    void EraseAdapterAllocation(uint64_t vidMmGlobalAlloc);

    const AllocationInfoEntry* const TryFindAllocationInfoByThunk(uint64_t thunkAllocation) const;
    const DeviceAllocation* const TryFindDeviceAllocationByVidMmAlloc(uint64_t vidMmAlloc) const;
    const DeviceAllocation* const TryFindDeviceAllocationByThunk(uint64_t thunkAllocation) const;
    std::vector<const DeviceAllocation*> TryFindDeviceAllocationsByVidMmGlobalAlloc(uint64_t vidMmGlobalAlloc) const;
    const AdapterAllocation* const TryFindAdapterAllocation(uint64_t dxgAdapter, uint64_t vidMmGlobalAlloc) const;

    uint64_t GetMemoryUsage(Handle handle) const;
    static uint64_t GetMemoryUsage(const AllocationInfo& info);

    // Tracks which memory pool each allocation currently lives in, keyed by
    // VidMm global handle. Populated from the DXGK segment events.
    void SetResidentGroup(uint64_t vidMmGlobalAlloc, MemorySegmentGroup group);
    std::optional<MemorySegmentGroup> TryGetResidentGroup(uint64_t vidMmGlobalAlloc) const;

private:
    std::vector<AllocationInfoEntry> m_allocationInfos; // Since we hand out handles we _cannot_ erase any entries after they've been added. Maybe should start as map with key being the d3d12 object (so can delete by address?)
    std::vector<DeviceAllocation> m_deviceAllocations; // TODO maybe should store as a map with vidMmAlloc + vidMmGlobalAlloc pair as key? or just vidMmGlobalAlloc?
    std::vector<AdapterAllocation> m_adapterAllocations;
    std::unordered_map<uint64_t, MemorySegmentGroup> m_residentGroupByVidMmGlobalAlloc;
};

} // namespace DirectX::Etw