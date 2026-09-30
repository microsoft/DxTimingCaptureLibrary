// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "pch.h"
#include "AllocationTracker.h"

namespace DirectX::Etw
{

AllocationTracker::Handle AllocationTracker::AddAllocationInfo(uint64_t timestamp, AllocationInfo info)
{
    Handle handle = m_allocationInfos.size();
    m_allocationInfos.push_back(std::make_tuple(timestamp, std::move(info)));
    return handle;
}

const AllocationInfo& AllocationTracker::GetAllocationInfo(Handle handle) const
{
    return std::get<1>(m_allocationInfos[handle]);
}

void AllocationTracker::AddDeviceAllocation(DeviceAllocation allocation)
{
    m_deviceAllocations.push_back(std::move(allocation));
}

// vidMmAlloc and vidMmGlobalAlloc uniquely identify a device allocation

void AllocationTracker::EraseDeviceAllocationByVidMm(uint64_t vidMmAlloc, uint64_t vidMmGlobalAlloc)
{
    m_deviceAllocations.erase(
        std::remove_if(m_deviceAllocations.begin(), m_deviceAllocations.end(),
            [vidMmGlobalAlloc, vidMmAlloc](const DeviceAllocation& alloc)
            {
                return alloc.VidMmGlobalAlloc == vidMmGlobalAlloc
                    && alloc.VidMmAlloc == vidMmAlloc;
            }),
        m_deviceAllocations.end());
}

void AllocationTracker::AddAdapterAllocation(AdapterAllocation allocation)
{
    m_adapterAllocations.push_back(std::move(allocation));
}

void AllocationTracker::EraseAdapterAllocation(uint64_t vidMmGlobalAlloc)
{
    m_adapterAllocations.erase(
        std::remove_if(m_adapterAllocations.begin(), m_adapterAllocations.end(),
            [vidMmGlobalAlloc](const AdapterAllocation& alloc)
            {
                return alloc.VidMmGlobalAlloc == vidMmGlobalAlloc;
            }),
        m_adapterAllocations.end());

    m_residentGroupByVidMmGlobalAlloc.erase(vidMmGlobalAlloc);
}

const AllocationTracker::AllocationInfoEntry* const AllocationTracker::TryFindAllocationInfoByThunk(uint64_t thunkAllocation) const
{
    const auto allocationInfoIt = std::find_if(m_allocationInfos.cbegin(), m_allocationInfos.cend(),
        [thunkAllocation](const AllocationTracker::AllocationInfoEntry& info)
        {
            // Bug 40407969: AllocationInfo possibly missing kernel allocation information for large allocations (>4GB)
            return std::get<1>(info).kmtInfos.size() > 0
                && std::get<1>(info).kmtInfos[0].kmAllocation == thunkAllocation;
        });

    if (allocationInfoIt != m_allocationInfos.cend())
    {
        return allocationInfoIt._Ptr;
    }
    else
    {
        return nullptr;
    }
}

const AllocationTracker::DeviceAllocation* const AllocationTracker::TryFindDeviceAllocationByVidMmAlloc(uint64_t vidMmAlloc) const
{
    DeviceAllocationIterator deviceAllocationIt = std::find_if(m_deviceAllocations.cbegin(), m_deviceAllocations.cend(),
            [vidMmAlloc](const DeviceAllocation& alloc) { return alloc.VidMmAlloc == vidMmAlloc; });

    if (deviceAllocationIt != m_deviceAllocations.cend())
    {
        return deviceAllocationIt._Ptr;
    }
    else
    {
        return nullptr;
    }
}

const AllocationTracker::DeviceAllocation* const AllocationTracker::TryFindDeviceAllocationByThunk(uint64_t thunkAllocation) const
{
    DeviceAllocationIterator deviceAllocationIt = std::find_if(m_deviceAllocations.cbegin(), m_deviceAllocations.cend(),
            [thunkAllocation](const DeviceAllocation& alloc) { return alloc.ThunkAllocation == thunkAllocation; });

    if (deviceAllocationIt != m_deviceAllocations.cend())
    {
        return deviceAllocationIt._Ptr;
    }
    else
    {
        return nullptr;
    }
}

std::vector<const AllocationTracker::DeviceAllocation*> AllocationTracker::TryFindDeviceAllocationsByVidMmGlobalAlloc(uint64_t vidMmGlobalAlloc) const
{
    std::vector<const AllocationTracker::DeviceAllocation*> results;

    for (size_t i = 0; i < m_deviceAllocations.size(); i++)
    {
        if (m_deviceAllocations[i].VidMmGlobalAlloc == vidMmGlobalAlloc)
        {
            results.push_back(&m_deviceAllocations[i]);
        }
    }

    return results;
}

const AllocationTracker::AdapterAllocation* const AllocationTracker::TryFindAdapterAllocation(uint64_t dxgAdapter, uint64_t vidMmGlobalAlloc) const
{
    const auto it = std::find_if(m_adapterAllocations.cbegin(), m_adapterAllocations.cend(),
        [=](const AdapterAllocation& alloc) { return alloc.VidMmGlobalAlloc == vidMmGlobalAlloc && alloc.DxgAdapter == dxgAdapter; });

    if (it != m_adapterAllocations.cend())
    {
        return it._Ptr;
    }

    return nullptr;
}

uint64_t AllocationTracker::GetMemoryUsage(Handle handle) const
{
    return GetMemoryUsage(GetAllocationInfo(handle));
}

/* static */
uint64_t AllocationTracker::GetMemoryUsage(const AllocationInfo& info)
{
    uint64_t result = 0;
    for (const auto& kmtInfo : info.kmtInfos)
    {
        result += kmtInfo.size;
    }
    return result;
}

void AllocationTracker::SetResidentGroup(uint64_t vidMmGlobalAlloc, MemorySegmentGroup group)
{
    m_residentGroupByVidMmGlobalAlloc[vidMmGlobalAlloc] = group;
}

std::optional<MemorySegmentGroup> AllocationTracker::TryGetResidentGroup(uint64_t vidMmGlobalAlloc) const
{
    const auto it = m_residentGroupByVidMmGlobalAlloc.find(vidMmGlobalAlloc);
    if (it != m_residentGroupByVidMmGlobalAlloc.cend())
    {
        return it->second;
    }

    return std::nullopt;
}

} // namespace DirectX::Etw