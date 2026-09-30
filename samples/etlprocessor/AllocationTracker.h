// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#pragma once

#include <cstdint>
#include <format>
#include <iostream>
#include <set>
#include <string>

#include <DxTimingCaptureLibrary/EtwDispatcher.h>

using namespace DirectX::Etw;

namespace EtlProcessor
{
    // Tracks a specific class of dxgkrnl adapter allocations (those tagged with
    // the 0x4000 flag) by pairing their Start and Stop events, and reports how
    // many were still outstanding when the trace ended.  It demonstrates
    // correlating allocation lifetimes rather than being a complete leak report.
    class AllocationTracker
    {
    public:
        AllocationTracker() = default;

        ~AllocationTracker()
        {
            std::cout << std::format("{} allocations remaining\n", m_pendingAllocations.size());
        }

    private:
        friend class EtwDispatcher<AllocationTracker>;

        std::set<uint64_t> m_pendingAllocations;

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

        template<>
        void OnDxgkEvent_Start(EVENT_RECORD* record, DxgkAdapterAllocationArgs args)
        {
            // Only the 0x4000 allocation class is tracked here.
            if (args.flags & 0x4000)
            {
                m_pendingAllocations.insert(args.hDxgGlobalAlloc);
                std::cout << std::format("+ {:016x} {:08x}\n", args.hDxgGlobalAlloc, args.flags);
            }
        }

        template<>
        void OnDxgkEvent_Stop(EVENT_RECORD* record, DxgkAdapterAllocationArgs args)
        {
            if (m_pendingAllocations.count(args.hDxgGlobalAlloc) > 0)
            {
                std::cout << std::format("- {:016x} (match)\n", args.hDxgGlobalAlloc);
                m_pendingAllocations.erase(args.hDxgGlobalAlloc);
            }
        }

        void OnPnpDeviceDescription(
            EVENT_RECORD* record,
            std::wstring_view deviceId,
            std::wstring_view deviceDescription)
        {
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
    };
}
