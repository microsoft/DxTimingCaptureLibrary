// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#pragma once

#include <algorithm>
#include <format>
#include <vector>
#include <optional>
#include <iostream>
#include <string>
#include <tuple>

#pragma warning(push)
#pragma warning(disable : 4369)
#include "MarkerOp.h"
#pragma warning(pop)

#include <DxTimingCaptureLibrary/EtwDiagnostics.h>
#include <DxTimingCaptureLibrary/EtwException.h>
#include <DxTimingCaptureLibrary/TicksToNanoseconds.h>

namespace DirectX::Etw
{
    inline uint32_t GetHighestBitPos(uint32_t value)
    {
        uint32_t highestPosition = 0;
        while (value >>= 1)
        {
            ++highestPosition;
        }
        return highestPosition;
    }

    struct HistoryBufferEntry
    {
        uint64_t ToP;
        uint64_t EoP;

        // This tells us the CPU time that we obtained this data.  We use this
        // to look up the clock calibration data.
        uint64_t CpuClockTicks;
    };

    struct RuntimeMarker
    {
        uint64_t ApiSequenceNumber;
        EMarkerOp MarkerOp;
    };

    struct EventTime
    {
        uint32_t EventId;
        uint64_t TopOfPipeTimestampNs;
        uint64_t EndOfPipeTimestampNs;
    };

    struct CalibratedClockEntry
    {
        uint64_t CpuClockTicks;
        uint64_t GpuClockTicks;
        uint64_t GpuFrequency;
    };

    class CalibratedClock
    {
        std::vector<CalibratedClockEntry> m_entries;

    public:
        void AddCalibration(uint64_t gpuFrequency, uint64_t gpuClockTicks, uint64_t cpuClockTicks, DiagnosticsSink* diagnosticsSink)
        {
            // The runtime used DdiCalibrateGpuClock to query the clock values.
            // The gpuFrequency and gpuClock values are populated from the GPU.
            // The cpuClock value is documented as being populated by
            // KeQueryPerformanceCounter.
            //
            // We need this because at various points we get GPU timestamps, but
            // we don't know the GPU frequency at that timestamp and so we need
            // to be able to look that up.  Note: in real life, we don't ever
            // expect to see the GPU frequency change on modern GPUs.
            //
            // We could also use this to try and convert to CPU time (which is
            // something that we do in timing captures because we want to
            // present CPU and (possibly multi-) GPU events on the same
            // timeline.  We don't need to do this here.

            // We expect AddCalibration to always be called with increasing CPU
            // and GPU time (since clocks can't ever run backwards)
            if (!m_entries.empty() && m_entries.back().CpuClockTicks > cpuClockTicks)
            {
                diagnosticsSink->OnDiagnostic(
                    DiagnosticSeverity::Error,
                    DiagnosticCode::ClockCalibrationCpuWentBackwards,
                    std::format(
                        L"CalibratedClock Error: Reported CpuClockTick ({}) is smaller than last reported CpuClockTick ({}).",
                        cpuClockTicks,
                        m_entries.back().CpuClockTicks));
                return;
            }

            if (!m_entries.empty() && m_entries.back().GpuClockTicks > gpuClockTicks)
            {
                diagnosticsSink->OnDiagnostic(
                    DiagnosticSeverity::Error,
                    DiagnosticCode::ClockCalibrationGpuWentBackwards,
                    std::format(
                        L"CalibratedClock Error: Reported GpuClockTick ({}) is smaller than last reported GpuClockTick ({}).",
                        gpuClockTicks,
                        m_entries.back().GpuClockTicks));
                return;
            }

            m_entries.push_back({ cpuClockTicks, gpuClockTicks, gpuFrequency });
        }

        uint64_t GpuTicksToNanoseconds(uint64_t gpuClockTicks) const
        {
            auto& entry = GetEntry(gpuClockTicks);
            return TicksToNanoseconds(gpuClockTicks, entry.GpuFrequency);
        }

        uint64_t GpuTicksToQpc(uint64_t gpuClockTicks, uint64_t qpcFrequency) const
        {
            auto& entry = GetEntry(gpuClockTicks);
            if (entry.GpuFrequency == 0)
            {
                Errors::ThrowToolException(E_UNEXPECTED, L"Clock calibration reported a zero GPU frequency");
            }

            // Signed, because the timestamp may sit either side of the anchor.
            int64_t gpuDelta = static_cast<int64_t>(gpuClockTicks) - static_cast<int64_t>(entry.GpuClockTicks);
            int64_t qpcDelta = static_cast<int64_t>(
                (static_cast<double>(gpuDelta) * static_cast<double>(qpcFrequency)) / static_cast<double>(entry.GpuFrequency));

            return static_cast<uint64_t>(static_cast<int64_t>(entry.CpuClockTicks) + qpcDelta);
        }

        uint64_t GetCalibratedGpuTicks(uint64_t gpuClockTicks)
        {
            return GetEntry(gpuClockTicks).GpuClockTicks;
        }

        CalibratedClockEntry const& GetEntry(uint64_t gpuClockTicks) const
        {
            if (m_entries.empty())
            {
                // We can't do anything if we don't have any calibration data!
                Errors::ThrowToolException(E_UNEXPECTED, L"Unexpectedly trying to calculate clock time before receiving any clock calibration data");
            }

            auto it = std::ranges::lower_bound(m_entries, gpuClockTicks, {}, &CalibratedClockEntry::GpuClockTicks);
            if (it == m_entries.end())
            {
                // Just use the last entry
                --it;
            }

            return *it;
        }

        bool IsEmpty() const
        {
            return m_entries.empty();
        }
    };

    struct DxgkContext
    {
        uint64_t Adapter;
        uint32_t NodeOrdinal;
        uint32_t EngineOrdinal;
        CalibratedClock Clock;
    };

    class DxgkContexts
    {
        struct DxgkDevice
        {
            uint64_t Device;
            uint64_t Adapter;
            uint32_t ProcessId;
            uint32_t KmDevice;

            bool operator< (uint64_t device) const
            {
                return Device < device;
            }
        };

        std::vector<DxgkDevice> m_devices;

    public:

        struct UmContext
        {
            uint32_t ProcessId;
            uint64_t Context;
            uint32_t ContextHandle;
            uint64_t Adapter;
            uint32_t NodeOrdinal;
            uint32_t EngineOrdinal;
            uint32_t EngineAffinity;

            bool operator< (uint64_t context) const
            {
                return Context < context;
            }
        };

    private:

        std::vector<UmContext> m_umContexts;

        struct HwQueue
        {
            UmContext ParentUmContext;
            uint64_t QueueHandle;
            uint64_t Queue;

            bool operator< (uint32_t queueHandle) const
            {
                return QueueHandle < queueHandle;
            }
        };

        std::vector<HwQueue> m_hwQueues;

        std::vector<DxgkContext> m_dxgkContexts;

    public:

        std::optional<uint64_t> GetDxgAdapterFromKmDevice(uint32_t kmDevice)
        {
            auto it = std::ranges::find_if(m_devices, [&](auto& d) { return d.KmDevice == kmDevice; });
            if (it != m_devices.end())
            {
                return it->Adapter;
            }
            return std::nullopt;
        }

        void UpdateDevice(
            uint64_t device,
            uint64_t adapter,
            uint64_t rawProcessId,
            uint32_t kmDevice)
        {
            // processId is logged as a uint64, but it's really a uint32
            assert((rawProcessId & 0xFFFFFFFF00000000) == 0);
            uint32_t processId = static_cast<uint32_t>(rawProcessId);

            auto it = std::ranges::lower_bound(m_devices, device, {}, &DxgkDevice::Device);
            if (it == m_devices.end() || it->Device != device)
            {
                m_devices.insert(it, { device, adapter, processId, kmDevice });
            }
            else
            {
                // This device might have been removed, and new one created at
                // the same address, so update the values.  We don't bother
                // tracking destruction separately since we only use this for
                // looking things up.
                it->Adapter = adapter;
                it->ProcessId = processId;
                it->KmDevice = kmDevice;
            }
        }

        void UpdateHwQueue(
            uint64_t parentContext,
            uint64_t hwQueueHandle,
            uint64_t hwQueue)
        {
            // Ignore HW Queue handles that are 0 because these are internal handles
            // created by the kernel.
            if (hwQueueHandle == 0)
                return;

            auto it = std::ranges::lower_bound(m_umContexts, parentContext, {}, &UmContext::Context);
            if (it != m_umContexts.end() && it->Context == parentContext)
            {
                HwQueue newHwQueue = {};
                newHwQueue.ParentUmContext = *it;
                newHwQueue.QueueHandle = hwQueueHandle;
                newHwQueue.Queue = hwQueue;

                m_hwQueues.push_back(newHwQueue);
            }
        }

        void UpdateContext(
            uint64_t context,
            uint64_t rawContextHandle,
            uint64_t device,
            uint32_t nodeOrdinal,
            uint32_t engineAffinity)
        {
            uint32_t engineOrdinal = GetHighestBitPos(engineAffinity);

            // contextHandle is logged as uint64, but it's really a
            // D3DKMT_HANDLE, which is a uint32
            assert((rawContextHandle & 0xFFFFFFFF00000000) == 0);
            uint32_t contextHandle = static_cast<uint32_t>(rawContextHandle);

            // We can't tell just by looking at the context which process or
            // adapter it belongs to.  But we can get this from the device...
            auto const* dxgkDevice = TryGetDevice(device);
            if (!dxgkDevice)
                return;

            auto processId = dxgkDevice->ProcessId;
            auto adapter = dxgkDevice->Adapter;

            auto it = std::ranges::lower_bound(m_umContexts, context, {}, &UmContext::Context);

            if (it == m_umContexts.end() || it->Context != context)
            {
                m_umContexts.insert(it, { processId, context, contextHandle, adapter, nodeOrdinal, engineOrdinal, engineAffinity });
            }
            else
            {
                // This context might have been removed - and a new one may have
                // been created at the same address.  We don't bother tracking
                // destruction separately.
                it->ProcessId = processId;
                it->ContextHandle = contextHandle;
                it->Adapter = adapter;
                it->NodeOrdinal = nodeOrdinal;
                it->EngineOrdinal = engineOrdinal;
                it->EngineAffinity = engineAffinity;
            }
        }

        uint64_t GetContext(uint32_t processId, uint32_t contextHandle) const
        {
            auto it = std::ranges::find_if(m_umContexts, [&](auto& c) { return c.ProcessId == processId && c.ContextHandle == contextHandle; });
            if (it == m_umContexts.end())
            {
                assert(false);
                Errors::ThrowToolException(E_UNEXPECTED);
            }
            return it->Context;
        }

        std::optional<uint64_t> TryGetContext(uint32_t processId, uint32_t handle) const
        {
            if (auto umContext = TryGetUmContext(processId, handle))
                return umContext->Context;

            if (auto hwQueue = TryGetHwQueue(processId, handle))
                return hwQueue->QueueHandle;

            return std::nullopt;
        }

        UmContext const* TryGetUmContext(uint32_t processId, uint32_t contextHandle) const
        {
            auto it = std::ranges::find_if(m_umContexts, [&](auto& c) { return c.ProcessId == processId && c.ContextHandle == contextHandle; });
            if (it != m_umContexts.end())
                return &*it;
            return nullptr;
        }

        HwQueue const* TryGetHwQueue(uint32_t processId, uint32_t hwQueueHandle) const
        {
            auto it = std::ranges::find_if(m_hwQueues, [&](auto& h) { return h.ParentUmContext.ProcessId == processId && h.QueueHandle == hwQueueHandle; });
            if (it != m_hwQueues.end())
                return &*it;
            return nullptr;
        }

        // Matches on the DXG object pointer, not the OS handle TryGetHwQueue above uses.
        HwQueue const* TryGetHwQueueByQueueObject(uint64_t hwQueue) const
        {
            auto it = std::find_if(m_hwQueues.rbegin(), m_hwQueues.rend(), [&](auto& h) { return h.Queue == hwQueue; });
            if (it != m_hwQueues.rend())
                return &*it;
            return nullptr;
        }

        void AddClockCalibration(
            uint64_t adapter,
            uint32_t nodeOrdinal,
            uint32_t engineOrdinal,
            uint64_t gpuFrequency,
            uint64_t gpuClock,
            uint64_t cpuClock,
            DiagnosticsSink* diagnosticsSink)
        {
            auto& context = GetOrCreateDxgkContext(adapter, nodeOrdinal, engineOrdinal);
            context.Clock.AddCalibration(gpuFrequency, gpuClock, cpuClock, diagnosticsSink);
        }

        CalibratedClock const& GetCalibratedClock(uint64_t context) const
        {
            return GetDxgContextFromContext(context).Clock;
        }

        // The context can be a broadcast context or a HW Queue.
        // HW Queues are used if HW secheduling is being used.
        DxgkContext& GetOrCreateDxgContextFromContext(uint64_t context)
        {
            auto umContext = GetUmContextForCalibratedClock(context);
            return GetOrCreateDxgkContext(umContext.Adapter, umContext.NodeOrdinal, umContext.EngineOrdinal);
        }

        UmContext const* TryGetUmContextForTimingContext(uint64_t context) const
        {
            if (auto umc = TryGetUmContextForCalibratedClock(context))
                return umc;

            if (auto hwq = TryGetHwQueueForCalibratedClock(context))
                return &hwq->ParentUmContext;

            return nullptr;
        }

    private:

        // The context can be a broadcast context or a HW Queue.
        // HW Queues are used if HW secheduling is being used.
        DxgkContext const& GetDxgContextFromContext(uint64_t context) const
        {
            auto umContext = GetUmContextForCalibratedClock(context);
            return GetDxgkContext(umContext.Adapter, umContext.NodeOrdinal, umContext.EngineOrdinal);
        }

        HwQueue const* TryGetHwQueueForCalibratedClock(uint64_t hwQueueHandle) const
        {
            auto it = std::ranges::find_if(m_hwQueues, [&](auto& h) { return h.QueueHandle == hwQueueHandle; });
            if (it != m_hwQueues.end())
                return &*it;
            return nullptr;
        }

        UmContext GetUmContextForCalibratedClock(uint64_t context) const
        {
            if (auto umc = TryGetUmContextForCalibratedClock(context))
                return *umc;

            if (auto hwq = TryGetHwQueueForCalibratedClock(context))
                return hwq->ParentUmContext;

            Errors::ThrowToolException(E_UNEXPECTED);
        }

        UmContext const* TryGetUmContextForCalibratedClock(uint64_t context) const
        {
            auto it = std::ranges::find_if(m_umContexts, [&](auto& c) { return c.Context == context; });
            if (it != m_umContexts.end())
                return &*it;
            return nullptr;
        }

        DxgkDevice const* TryGetDevice(uint64_t device) const
        {
            auto it = std::ranges::lower_bound(m_devices, device, {}, &DxgkDevice::Device);
            if (it == m_devices.end() || it->Device != device)
            {
                return nullptr;
            }

            return &*it;
        }

        DxgkContext const& GetDxgkContext(uint64_t adapter, uint32_t nodeOrdinal, uint32_t engineOrdinal) const
        {
            auto it = std::ranges::find_if(m_dxgkContexts,
                [&](auto& c)
            {
                return (c.Adapter == adapter
                    && c.NodeOrdinal == nodeOrdinal
                    && c.EngineOrdinal == engineOrdinal);
            });

            if (it == m_dxgkContexts.end())
            {
                Errors::ThrowToolException(E_UNEXPECTED);
            }

            return *it;
        }

        DxgkContext& GetOrCreateDxgkContext(uint64_t adapter, uint32_t nodeOrdinal, uint32_t engineOrdinal)
        {
            auto it = std::ranges::find_if(m_dxgkContexts,
                [&](auto& c)
            {
                return (c.Adapter == adapter
                    && c.NodeOrdinal == nodeOrdinal
                    && c.EngineOrdinal == engineOrdinal);
            });

            if (it == m_dxgkContexts.end())
            {
                it = m_dxgkContexts.insert(it, { adapter, nodeOrdinal, engineOrdinal });
            }

            return *it;
        }
    };

    // These are identified by the device and sequence number
    using CommandListInstanceId = std::tuple<uint64_t, uint64_t>;
    using CommandQueueInstanceId = std::tuple<uint64_t, uint64_t>;

    struct CommandListIdAndApiSequenceNumbers
    {
        Etw::CommandListInstanceId CommandListInstanceId;
        std::vector<uint64_t> ApiSequenceNumbers;
    };

    struct CommandBufferSubmission
    {
        uint64_t Context;
        uint32_t SubmitSequence;

        CommandListInstanceId LastCommandListInstanceId;
        std::vector<Etw::CommandListIdAndApiSequenceNumbers> CommandListInstanceIdAndApiSequenceNumbers;

        uint32_t LastLoopIteration;

        std::vector<HistoryBufferEntry> HistoryBuffer;

        std::optional<HistoryBufferEntry> GetHistoryBufferEntry(CommandListInstanceId commandList, uint64_t apiSequenceNumber) const
        {
            // The first HistoryBuffer entry contains the begin/end for the
            // entire submission for a command list, so we ignore that one
            size_t index = 1;
            for (auto const& entry : CommandListInstanceIdAndApiSequenceNumbers)
            {
                if (entry.CommandListInstanceId == commandList)
                {
                    for (auto sequenceNumber : entry.ApiSequenceNumbers)
                    {
                        if (sequenceNumber == apiSequenceNumber)
                        {
                            assert(index < HistoryBuffer.size());
                            return HistoryBuffer[index];
                        }
                        index++;
                    }
                }
                else
                {
                    index += entry.ApiSequenceNumbers.size();
                }
            }

            return std::nullopt;
        }
    };
} // namespace DirectX::Etw
