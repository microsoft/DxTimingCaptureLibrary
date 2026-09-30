// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#pragma once

#include <vector>

namespace DirectX::Etw
{

// from src/etw/DirectX-Direct3D12.man D3D12ResourceHeapType in Direct3D repo
enum ResourceHeapType : uint32_t
{
    ImmutableHeap,     // placed resource
    ImplicitHeap,      // committed resource
    ImplicitResource,  // implicit resource used internally for placed resource backing
    ReservedResource,  // reserved resource
};

// D3D12AllocationInfo
#pragma pack(push, 1)
struct VirtualAddressInfos
{
    uint32_t _padding;
    uint32_t physicalAdapterIndex;
    uint64_t startAddress;
    uint64_t endAddress;
};

struct KMTInfos
{
    uint32_t physicalAdapterIndex;
    uint32_t kmAllocation;
    uint64_t offset;
    uint64_t size;
};
#pragma pack(pop)

struct AllocationInfo
{
    uint64_t device;
    uint64_t object;
    std::vector<VirtualAddressInfos> virtualAddressInfos;
    std::vector<KMTInfos> kmtInfos;
};


} // namespace DirectX::Etw