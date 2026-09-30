// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#pragma once

#include "MemoryCounterWriter.h"
#include "MonitorTracker.h"

namespace DirectX::Etw
{

class AdapterTracker
{
    using AdapterSegmentId = uint32_t;
    struct AdapterSegmentInfo
    {
        D3DKMT_MEMORY_SEGMENT_GROUP MemorySegmentGroup;
    };

    struct PerAdapterInfo
    {
        AdapterId AdapterId;
        std::wstring PnpDeviceId;
        std::wstring Description;
        std::vector<HardwareCommandQueueId> EngineNodeOrdinalToHardwareQueueDatabaseId;
        uint64_t MemoryCounterWriterIndex = 0;

        std::unordered_map<AdapterSegmentId, AdapterSegmentInfo> SegmentInfo;
    };

    std::map<uint64_t, PerAdapterInfo> m_adapterLuidToAdapterInfo;
    std::map<uint64_t, uint64_t> m_dxgkAdapterToAdapterLuid;
    std::map<uint64_t, std::set<MonitorIdentifier>> m_dxgkAdapterToMonitorIds;
    std::vector<std::shared_ptr<MemoryCounterWriter>> m_memoryCounterWriters;

    MonitorTracker m_monitorTracker;

public:
    void ReportLastMemoryCounterValues(int64_t timestamp, PixCounterCallbacks* callbacks)
    {
        for (auto& writer : m_memoryCounterWriters)
        {
            writer->ReportLastCounterValues(timestamp, callbacks);
        }
    }

    std::optional<AdapterId> GetAdapterId(uint64_t dxgkAdapter) const
    {
        auto it = m_dxgkAdapterToAdapterLuid.find(dxgkAdapter);
        if (it == m_dxgkAdapterToAdapterLuid.end())
        {
            return std::nullopt;
        }

        auto itInfo = m_adapterLuidToAdapterInfo.find(it->second);
        if (itInfo == m_adapterLuidToAdapterInfo.end())
        {
            return std::nullopt;
        }

        return itInfo->second.AdapterId;
    }

    MemoryCounterWriter* GetMemoryCounterWriter(uint64_t dxgkAdapter) const
    {
        auto it = m_dxgkAdapterToAdapterLuid.find(dxgkAdapter);
        if (it == m_dxgkAdapterToAdapterLuid.end())
        {
            return nullptr;
        }

        auto itInfo = m_adapterLuidToAdapterInfo.find(it->second);
        if (itInfo == m_adapterLuidToAdapterInfo.end())
        {
            return nullptr;
        }

        return m_memoryCounterWriters[itInfo->second.MemoryCounterWriterIndex].get();
    }

    std::optional<HardwareCommandQueueId> GetHardwareCommandQueueId(uint64_t dxgkAdapter, size_t engineNodeOrdinal) const
    {
        auto it = m_dxgkAdapterToAdapterLuid.find(dxgkAdapter);
        if (it == m_dxgkAdapterToAdapterLuid.end())
        {
            return std::nullopt;
        }

        auto itInfo = m_adapterLuidToAdapterInfo.find(it->second);
        if (itInfo == m_adapterLuidToAdapterInfo.end())
        {
            return std::nullopt;
        }

        size_t requiredNodes = engineNodeOrdinal + 1;
        if (itInfo->second.EngineNodeOrdinalToHardwareQueueDatabaseId.size() < requiredNodes)
        {
            return std::nullopt;
        }

        return itInfo->second.EngineNodeOrdinalToHardwareQueueDatabaseId[engineNodeOrdinal];
    }

    void UpdateDxgkAdapter(uint64_t dxgkAdapter, uint64_t adapterLuid, wchar_t const* pnpDeviceId, DxgkObjectCallbacks* dxgkObjectCallbacks, PixCounterCallbacks* counterCalbacks)
    {
        m_dxgkAdapterToAdapterLuid[dxgkAdapter] = adapterLuid;

        auto itInfo = m_adapterLuidToAdapterInfo.find(adapterLuid);
        if (itInfo != m_adapterLuidToAdapterInfo.end())
        {
            // We have seen this adapter before.  Ensure that the database has the current name by updating it now.
            // If no description is provided, we use the PnPID as the name.
            // This matches GPU View behavior.
            auto adapterName = itInfo->second.Description.empty() ? pnpDeviceId : itInfo->second.Description;
            ThrowFailure(dxgkObjectCallbacks->OnHardwareAdapterName(itInfo->second.AdapterId.Value, adapterName.c_str()));
        }
        else
        {
            // This is the first time we have seen this dxgkadapter + adapter luuid combination,
            // so define a new adapter now.
            uint64_t adapterId = 0;
            ThrowFailure(dxgkObjectCallbacks->OnHardwareAdapter(&adapterId));

            PerAdapterInfo info = {};
            info.AdapterId = AdapterId(adapterId);
            info.PnpDeviceId = pnpDeviceId;
            info.MemoryCounterWriterIndex = m_memoryCounterWriters.size();
            m_memoryCounterWriters.push_back(std::make_shared<MemoryCounterWriter>(info.AdapterId, counterCalbacks));

            m_adapterLuidToAdapterInfo[adapterLuid] = std::move(info);
        }
    }

    void UpdateDxgiAdapterHardwareQueue(uint64_t dxgkAdapter, uint32_t nodeOrdinal, DXGK_ENGINE_TYPE engineType, std::wstring_view friendlyName, DxgkObjectCallbacks* dxgkObjectCallbacks)
    {
        auto it = m_dxgkAdapterToAdapterLuid.find(dxgkAdapter);
        if (it != m_dxgkAdapterToAdapterLuid.end())
        {
            auto itInfo = m_adapterLuidToAdapterInfo.find(it->second);
            if (itInfo != m_adapterLuidToAdapterInfo.end())
            {
                std::wstring engineName(friendlyName);

                // If a friendly name is not provided assign one based on the engine type.
                if (engineName.empty())
                {
                    static wchar_t const* EngineTypeStrings[DXGK_ENGINE_TYPE_MAX] =
                    {
                        L"OTHER",
                        L"3D",
                        L"VIDEO_DECODE",
                        L"VIDEO_ENCODE",
                        L"VIDEO_PROCESSING",
                        L"SCENE_ASSEMBLY",
                        L"COPY",
                        L"OVERLAY",
                        L"CRYPTO"
                    };

                    // If an engine type value falls outside of this enumeration, 'OTHER' will be used.
                    if (engineType >= DXGK_ENGINE_TYPE_MAX)
                        engineType = DXGK_ENGINE_TYPE_OTHER;
                    engineName = EngineTypeStrings[engineType];
                }

                uint64_t hardwareQueueId = 0;
                ThrowFailure(dxgkObjectCallbacks->OnHardwareCommandQueue(itInfo->second.AdapterId.Value, &hardwareQueueId));
                ThrowFailure(dxgkObjectCallbacks->OnHardwareCommandQueueName(hardwareQueueId, engineName.c_str()));

                size_t requiredNodes = nodeOrdinal + 1;
                if (itInfo->second.EngineNodeOrdinalToHardwareQueueDatabaseId.size() < requiredNodes)
                {
                    itInfo->second.EngineNodeOrdinalToHardwareQueueDatabaseId.resize(requiredNodes);
                }
                itInfo->second.EngineNodeOrdinalToHardwareQueueDatabaseId[nodeOrdinal] = HardwareCommandQueueId(hardwareQueueId);
            }
        }
    }

    void UpdateDeviceDescription(std::wstring_view deviceId, std::wstring_view deviceDescription, DxgkObjectCallbacks* dxgkObjectCallbacks, MonitorEventCallbacks* monitorEventCallbacks, PixCounterCallbacks* counterCallbacks)
    {
        constexpr auto displayRoot = L"DISPLAY\\";
        if (deviceId.find(displayRoot) == 0)
        {
            UpdateDisplayDescription(deviceId, deviceDescription, dxgkObjectCallbacks);
        }
        else
        {
            UpdateAdapterDescription(deviceId, deviceDescription, dxgkObjectCallbacks, monitorEventCallbacks, counterCallbacks);
        }
    }

    void AddVsyncEvent(uint32_t vidPnTargetId, uint64_t dxgkAdapter, uint64_t timestampNs, MonitorEventCallbacks* monitorEventCallbacks)
    {
        if (vidPnTargetId == 0)
        {
            // Early out from invalid events
            return;
        }

        auto monitorIdentifier = MonitorIdentifier(vidPnTargetId, dxgkAdapter);

        auto adapterDescription = TryFindAdapterDescription(dxgkAdapter, monitorIdentifier);
        auto monitorId = m_monitorTracker.SetAdapterDescription(monitorEventCallbacks, monitorIdentifier, adapterDescription);

        // color is ARGB
        ThrowFailure(monitorEventCallbacks->OnVSync(monitorId.Value, timestampNs));
    }

    void UpdateSegment(uint64_t dxgAdapter, AdapterSegmentId segmentId, D3DKMT_MEMORY_SEGMENT_GROUP memorySegmentGroup)
    {
        auto adapterInfo = TryGetPerAdapterInfo(dxgAdapter);
        if (adapterInfo)
        {
            adapterInfo->SegmentInfo[segmentId].MemorySegmentGroup = memorySegmentGroup;
        }
    }

    std::optional<AdapterSegmentInfo> TryGetSegmentInfo(uint64_t dxgAdapter, AdapterSegmentId segmentId)
    {
        const auto adapterInfo = TryGetPerAdapterInfo(dxgAdapter);
        if (adapterInfo == nullptr || adapterInfo->SegmentInfo.count(segmentId) == 0)
        {
            return std::nullopt;
        }
        else
        {
            return adapterInfo->SegmentInfo.at(segmentId);
        }
    }

private:
    std::wstring TryFindAdapterDescription(uint64_t dxgkAdapter, MonitorIdentifier monitorId)
    {
        auto dxgkAdapterLUTIt = m_dxgkAdapterToMonitorIds.find(dxgkAdapter);
        if (dxgkAdapterLUTIt == m_dxgkAdapterToMonitorIds.end() || dxgkAdapterLUTIt->second.find(monitorId) == dxgkAdapterLUTIt->second.end())
        {
            // If we just created this monitor, or we first learned about this monitor elsewhere (i.e. PNP rundown),
            // we need to link relevant adapter information
            m_dxgkAdapterToMonitorIds[dxgkAdapter].insert(monitorId); // Add an entry to the LUT

            auto adapterLuidIt = m_dxgkAdapterToAdapterLuid.find(dxgkAdapter);
            if (adapterLuidIt != m_dxgkAdapterToAdapterLuid.end())
            {
                auto adapterInfoIt = m_adapterLuidToAdapterInfo.find(adapterLuidIt->second);
                if (adapterInfoIt != m_adapterLuidToAdapterInfo.end())
                {
                    auto const& adapterInfo = adapterInfoIt->second;
                    return adapterInfo.Description;
                }
            }
        }

        return {};
    }

    void UpdateAdapterDescription(std::wstring_view deviceId, std::wstring_view deviceDescription, DxgkObjectCallbacks* dxgkObjectCallbacks, MonitorEventCallbacks* monitorEventCallbacks, PixCounterCallbacks* counterCallbacks)
    {
        // Walk the adapters list and assign the device description to the one that
        // matches the deviceId.
        for (auto& info : m_adapterLuidToAdapterInfo)
        {
            // Search for a matching substring in the full PnP device Id to match the description to the
            // adapter.
            if (deviceId.find(info.second.PnpDeviceId) != std::wstring_view::npos)
            {
                info.second.Description = deviceDescription;
                ThrowFailure(dxgkObjectCallbacks->OnHardwareAdapterName(info.second.AdapterId.Value, info.second.Description.c_str()));
                m_memoryCounterWriters[info.second.MemoryCounterWriterIndex]->UpdateGroupName(info.second.Description.c_str(), counterCallbacks);

                // Update any monitors associated with this adapter
                auto dxgkAdapterIt = std::find_if(m_dxgkAdapterToAdapterLuid.begin(), m_dxgkAdapterToAdapterLuid.end(),
                    [&](auto const& id)
                    {
                        return id.second == info.first;
                    });
                assert(dxgkAdapterIt != m_dxgkAdapterToAdapterLuid.end());

                for (auto monitorId : m_dxgkAdapterToMonitorIds[dxgkAdapterIt->first])
                {
                    m_monitorTracker.SetAdapterDescription(monitorEventCallbacks, monitorId, info.second.Description);
                }
            }
        }
    }

    void UpdateDisplayDescription(std::wstring_view deviceId, std::wstring_view deviceDescription, DxgkObjectCallbacks* dxgkObjectCallbacks)
    {
        // deviceId looks something like this:
        // L"DISPLAY\\DEL4084\\8&1171F0ED&0&UID256"

        // TODO: figure out how to match this up to lanes we logged for the vsyncs
    }

    PerAdapterInfo* TryGetPerAdapterInfo(uint64_t dxgAdapter)
    {
        const auto luidIt = m_dxgkAdapterToAdapterLuid.find(dxgAdapter);
        if (luidIt != m_dxgkAdapterToAdapterLuid.end())
        {
            const auto adapterInfoIt = m_adapterLuidToAdapterInfo.find(luidIt->second);
            if (adapterInfoIt != m_adapterLuidToAdapterInfo.end())
            {
                return &adapterInfoIt->second;
            }
        }

        return nullptr;
    }
};

} // namespace DirectX::Etw