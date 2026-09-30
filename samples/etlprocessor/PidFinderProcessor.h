// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#pragma once

#include <cstdint>
#include <iostream>
#include <map>
#include <string>

#include <DxTimingCaptureLibrary/EtwDispatcher.h>

using namespace DirectX::Etw;

namespace EtlProcessor
{
    // Counts D3D12 device events per process so we can list every process that
    // did GPU work in a trace.
    class PidFinderProcessor
    {
        std::map<unsigned long, uint64_t> m_pidToEventCounts;

    public:
        PidFinderProcessor() = default;

        void OutputPidsWithD3D12Devices()
        {
            std::cout << "Processes that use a D3D12 Device" << std::endl;
            std::cout << "---------------------------------" << std::endl;
            for (auto const& [processId, eventCount] : m_pidToEventCounts)
            {
                std::cout << std::to_string(processId) << std::endl;
            }
        }

        auto begin() const { return m_pidToEventCounts.begin(); }
        auto end() const { return m_pidToEventCounts.end(); }

    private:
        friend class EtwDispatcher<PidFinderProcessor>;

        void SetTraceInfo(EVENT_TRACE_LOGFILE const&)
        {
        }

        template <typename TEventArgs>
        void OnD3D12Event_Start(EVENT_RECORD* record, TEventArgs args)
        {}

        template <typename TEventArgs>
        void OnD3D12Event_Stop(EVENT_RECORD* record, TEventArgs args)
        {}

        template <typename TEventArgs>
        void OnD3D12Event_DCStart(EVENT_RECORD* record, TEventArgs args)
        {}

        template <typename TEventArgs>
        void OnD3D12Event_Info(EVENT_RECORD* record, TEventArgs args)
        {}

        template <typename TEventArgs>
        void OnDxgkEvent_Start(EVENT_RECORD* record, TEventArgs args)
        {}

        template <typename TEventArgs>
        void OnDxgkEvent_Stop(EVENT_RECORD* record, TEventArgs args)
        {}

        template <typename TEventArgs>
        void OnDxgkEvent_DCStart(EVENT_RECORD* record, TEventArgs args)
        {}

        template <typename TEventArgs>
        void OnDxgkEvent_Info(EVENT_RECORD* record, TEventArgs args)
        {}

        template <>
        void OnD3D12Event_Start(EVENT_RECORD* record, D3D12DeviceArgs args)
        {
            m_pidToEventCounts[record->EventHeader.ProcessId]++;
        }

        template <>
        void OnD3D12Event_Stop(EVENT_RECORD* record, D3D12DeviceArgs args)
        {
            m_pidToEventCounts[record->EventHeader.ProcessId]++;
        }

        template <>
        void OnD3D12Event_DCStart(EVENT_RECORD* record, D3D12DeviceArgs args)
        {
            m_pidToEventCounts[record->EventHeader.ProcessId]++;
        }

        void OnEventWritePIXRecordTimingBlock_v1(
            EVENT_RECORD* record,
            uint32_t extraData,
            uint32_t bufferSize,
            uint8_t* buffer)
        {
        }

        void OnEventWritePIXRecordTimingBlock_v2(
            EVENT_RECORD* record,
            uint32_t extraData,
            uint32_t bufferSize,
            uint8_t* buffer)
        {
        }

        void OnEventWritePIXReportCounterData(
            EVENT_RECORD* record,
            float value,
            std::wstring_view name)
        {
        }

        void OnEventWritePIXRecordMemoryAllocationEvent(
            EVENT_RECORD* record,
            UINT16 allocatorId,
            UINT64 baseAddress,
            UINT64 size,
            UINT64 metadata)
        {
        }

        void OnEventWritePIXRecordMemoryFreeEvent(
            EVENT_RECORD* record,
            UINT16 allocatorId,
            UINT64 baseAddress,
            UINT64 size,
            UINT64 metadata)
        {
        }

        void OnPnpDeviceDescription(
            EVENT_RECORD* record,
            std::wstring_view deviceId,
            std::wstring_view deviceDescription)
        {
        }
    };
}
