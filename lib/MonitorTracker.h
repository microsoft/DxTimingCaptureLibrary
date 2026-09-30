// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#pragma once

namespace DirectX::Etw
{

class MonitorTracker
{
    struct PerMonitorInfo
    {
        MonitorId MonitorId;
        std::wstring Description;
        std::wstring AdapterDescription;
    };

    std::map<MonitorIdentifier, PerMonitorInfo> m_info;

public:
    MonitorId SetMonitorDescription(MonitorEventCallbacks* monitorEventCallbacks, MonitorIdentifier id, std::wstring const& monitorDescription)
    {
        auto it = m_info.find(id);
        if (it == m_info.end())
        {
            // This must be new!
            it = CreateNew(monitorEventCallbacks, id, monitorDescription, {});
        }
        else
        {
            if (!monitorDescription.empty() && it->second.Description != monitorDescription)
            {
                it->second.Description = monitorDescription;
                Update(monitorEventCallbacks, it->second);
            }
        }

        return it->second.MonitorId;
    }

    MonitorId SetAdapterDescription(MonitorEventCallbacks* monitorEventCallbacks, MonitorIdentifier id, std::wstring const& adapterDescription)
    {
        auto it = m_info.find(id);
        if (it == m_info.end())
        {
            // This must be new!
            it = CreateNew(monitorEventCallbacks, id, {}, adapterDescription);
        }
        else
        {
            if (!adapterDescription.empty() && it->second.AdapterDescription != adapterDescription)
            {
                it->second.AdapterDescription = adapterDescription;
                Update(monitorEventCallbacks, it->second);
            }
        }

        return it->second.MonitorId;
    }

private:
    std::map<MonitorIdentifier, PerMonitorInfo>::iterator CreateNew(MonitorEventCallbacks* monitorEventCallbacks, MonitorIdentifier id, std::wstring const& description, std::wstring const& adapterDescription)
    {
        static int monitorNumber = 0;

        PerMonitorInfo info;
        info.Description = description.empty() ? std::wstring(L"Monitor #{}") + std::to_wstring(++ monitorNumber) : description;
        info.AdapterDescription = adapterDescription.empty() ? std::to_wstring(id.second) : adapterDescription;

        ThrowFailure(monitorEventCallbacks->OnMonitor(
            info.Description.c_str(),
            info.AdapterDescription.c_str(),
            &info.MonitorId.Value));

        return m_info.emplace(id, std::move(info)).first;
    }

    void Update(MonitorEventCallbacks* monitorEventCallbacks, PerMonitorInfo const& info)
    {
        ThrowFailure(monitorEventCallbacks->OnMonitorUpdate(
            info.MonitorId.Value,
            info.Description.c_str(),
            info.AdapterDescription.c_str()));
    }
};

} // namespace DirectX::Etw
