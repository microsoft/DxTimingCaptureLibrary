// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#pragma once

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <map>
#include <string>
#include <tuple>
#include <vector>

#include <DxTimingCaptureLibrary/EtwDispatcher.h>
#include <DxTimingCaptureLibrary/D3D12EtwEventStructs.h>
#include <DxTimingCaptureLibrary/MarkerOp.h>

#include "CsvTableWriter.h"

using namespace DirectX::Etw;

namespace EtlProcessor
{
    struct CommandListInstance
    {
        std::vector<RuntimeMarker> RuntimeMarkers;
    };

    // Reconstructs GPU submissions for one process by correlating D3D12 command
    // list, submission, and dxgkrnl history-buffer events, then emits their
    // ToP/EoP timings as a CSV table.
    class HistoryBufferProcessor
    {
        unsigned long m_processId;
        DxgkContexts m_dxgkContexts;

        struct PerThreadData
        {
            std::vector<CommandListInstanceId> CurrentCommandListInstances;
        };

        std::map<uint32_t, PerThreadData> m_perThreadData;
        std::map<uint64_t, uint64_t> m_d3d12DeviceUmVersions;
        std::map<uint64_t, CommandListInstanceId> m_commandListToId;
        std::map<CommandListInstanceId, CommandListInstance> m_commandListInstances;
        std::vector<CommandBufferSubmission> m_submissions;

    public:
        HistoryBufferProcessor(unsigned long processId)
            : m_processId(processId)
        {
        }

        static inline std::wstring HighPartToString(uint64_t value)
        {
            return std::to_wstring((uint32_t)(value >> 32));
        }

        static inline std::wstring LowPartToString(uint64_t value)
        {
            return std::to_wstring((uint32_t)(value & 0xFFFFFFFF));
        }

        void OutputSubmissionsAsCSVTable(bool noHighLowParts)
        {
            std::vector<EventTime> eventTimes;

            CsvTableWriter tableWriter(std::wcout);

            if (noHighLowParts)
            {
                tableWriter.AddRow({ L"CommandList", L"Submission", L"MarkerOp", L"ToP", L"EoP", L"Submission Start", L"Submission End" });
            }
            else
            {
                tableWriter.AddRow({ L"CommandList", L"Submission", L"MarkerOp", L"ToP(LowPart)", L"ToP(HighPart)", L"EoP(LowPart)", L"EoP(HighPart)", L"Submission Start(LowPart)", L"Submission Start(HighPart)", L"Submission End(LowPart)", L"Submission End(HighPart)" });
            }

            for (auto const& submission : m_submissions)
            {
                uint32_t commandListIndex = 0;

                uint32_t index = 0;
                HistoryBufferEntry dmaEntry{};

                for (auto i = 0u; i < submission.HistoryBuffer.size(); i++)
                {
                    std::vector<std::wstring> row;

                    auto& entry = submission.HistoryBuffer[i];
                    if (i == 0)
                    {
                        // The first entry holds the start and end times for the whole
                        // submission; keep it so we can attach those to each operation.
                        dmaEntry = entry;
                        continue;
                    }

                    auto commandListInfo = submission.CommandListInstanceIdAndApiSequenceNumbers[commandListIndex];
                    auto commandListInstanceId = commandListInfo.CommandListInstanceId;
                    auto& commandListInstance = m_commandListInstances[commandListInstanceId];

                    uint64_t device = std::get<0>(commandListInstanceId);
                    uint64_t sequenceNumber = std::get<1>(commandListInstanceId);

                    row.push_back(std::to_wstring(device) + L"-" + std::to_wstring(sequenceNumber));
                    row.push_back(std::to_wstring(submission.SubmitSequence));

                    // ApiSequenceNumbers are 1-based (they carry one plus the current
                    // API sequence number), but RuntimeMarkers is 0-based, so subtract
                    // one to index into it.
                    auto markerOp = commandListInstance.RuntimeMarkers[commandListInfo.ApiSequenceNumbers[index] - 1].MarkerOp;
                    row.push_back(ToString(markerOp));

                    if (noHighLowParts)
                    {
                        // Operation Start/End
                        row.push_back(std::to_wstring(entry.ToP));
                        row.push_back(std::to_wstring(entry.EoP));

                        // Submission Start/End
                        row.push_back(std::to_wstring(dmaEntry.ToP));
                        row.push_back(std::to_wstring(dmaEntry.EoP));
                    }
                    else
                    {
                        // Operation Start/End
                        row.push_back(LowPartToString(entry.ToP));
                        row.push_back(HighPartToString(entry.ToP));
                        row.push_back(LowPartToString(entry.EoP));
                        row.push_back(HighPartToString(entry.EoP));

                        // Submission Start/End
                        row.push_back(LowPartToString(dmaEntry.ToP));
                        row.push_back(HighPartToString(dmaEntry.ToP));
                        row.push_back(LowPartToString(dmaEntry.EoP));
                        row.push_back(HighPartToString(dmaEntry.EoP));
                    }
                    tableWriter.AddRow(row);

                    index++;
                    if (index == commandListInfo.ApiSequenceNumbers.size())
                    {
                        commandListIndex++;
                        index = 0;
                    }
                }
            }
        }

    private:
        friend class EtwDispatcher<HistoryBufferProcessor>;

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
            if (!IsThisProcess(record))
            {
                return;
            }

            UpdateD3D12Device(args.device, args.umDeviceVersion);
        }

        template <>
        void OnD3D12Event_Stop(EVENT_RECORD* record, D3D12DeviceArgs args)
        {
            if (!IsThisProcess(record))
            {
                return;
            }

            UpdateD3D12Device(args.device, args.umDeviceVersion);
        }

        template <>
        void OnD3D12Event_DCStart(EVENT_RECORD* record, D3D12DeviceArgs args)
        {
            if (!IsThisProcess(record))
            {
                return;
            }

            UpdateD3D12Device(args.device, args.umDeviceVersion);
        }

        template <>
        void OnD3D12Event_Info(EVENT_RECORD* record, D3D12CommandListArgs args)
        {
            if (!IsThisProcess(record))
            {
                return;
            }

            StartNewCommandList(args.commandList, args.device, args.sequenceNumber);
        }

        template <>
        void OnD3D12Event_Start(EVENT_RECORD* record, D3D12CommandListArgs args)
        {
            if (!IsThisProcess(record))
            {
                return;
            }

            StartNewCommandList(args.commandList, args.device, args.sequenceNumber);
        }

        template <>
        void OnD3D12Event_Info(EVENT_RECORD* record, D3D12RuntimeMarkerDataArgs args)
        {
            if (!IsThisProcess(record))
            {
                return;
            }

            // Corresponds to SBatchedMarker
            struct Marker
            {
                uint32_t QPCLow;
                uint8_t ThreadIDOrdinal;
                EMarkerOp MarkerAPI;
                uint16_t Reserved; // padding
            };

            auto* markers = reinterpret_cast<Marker*>(args.data);
            auto markerCount = args.dataSize / sizeof(Marker);

            auto commandListInstanceId = m_commandListToId[args.commandList];
            auto& commandListInstance = m_commandListInstances[commandListInstanceId];

            commandListInstance.RuntimeMarkers.reserve(commandListInstance.RuntimeMarkers.size() + markerCount);

            for (auto i = 0u; i < markerCount; ++i)
            {
                RuntimeMarker m{};
                m.ApiSequenceNumber = args.firstApiSequenceNumber + i;
                m.MarkerOp = markers[i].MarkerAPI;

                commandListInstance.RuntimeMarkers.push_back(m);
            }
        }

        template <>
        void OnD3D12Event_Info(EVENT_RECORD* record, D3D12CommandBufferSubmissionArgs args)
        {
            if (!IsThisProcess(record))
            {
                return;
            }

            bool commandListSupplied = (args.commandList != 0);

            // If a command list was supplied, we use it to look up the instance, otherwise we just use the 'currently executing'
            // command list by accessing the first element in the CurrentCommandListInstances vector.
            CommandListInstanceId commandListInstanceId;
            if (!commandListSupplied)
            {
                auto& perThreadData = GetPerThreadData(record);
                if (perThreadData.CurrentCommandListInstances.empty())
                {
                    std::wcout << L"No command list was supplied, but there are no current command lists.  Skipping submission." << std::endl;
                    return;
                }
                commandListInstanceId = perThreadData.CurrentCommandListInstances.front();
            }
            else
            {
                // Ignore submissions for untracked command lists.
                auto it = m_commandListToId.find(args.commandList);
                if (it == m_commandListToId.end())
                {
                    std::wcout << L"Unknown command list 0x" << std::hex << args.commandList << L" reported for command buffer submission. Skipping submission." << std::endl;
                    return;
                }

                commandListInstanceId = it->second;
            }

            for (auto i = 0u; i < args.contextCount; ++i)
            {
                HandleCommandBufferSubmission(
                    commandListInstanceId,
                    args.contexts[i],
                    args.submitCommandCbSequence,
                    args.loopIteration,
                    args.firstApiSequenceNumberHigh,
                    args.completedApiSequenceNumberSize,
                    args.completedApiSequenceNumbers);
            }
        }

        void HandleCommandBufferSubmission(
            CommandListInstanceId commandListInstanceId,
            uint32_t contextUmdHandle,
            uint32_t submitCommandCbSequence,
            uint32_t loopIteration,
            uint32_t firstApiSequenceNumberHigh,
            uint32_t completedApiSequenceNumberSize,
            uint32_t* completedApiSequenceNumbers)
        {
            auto context = m_dxgkContexts.TryGetContext(m_processId, contextUmdHandle);
            if (!context)
            {
                return;
            }

            // Command buffer submissions are split up into multiple events,
            // that we have to stitch back together.  The runtime emits the
            // events before it actually submits the command buffer, so we don't
            // need to worry about getting history buffers for this submission
            // interleaved with these events.
            // Each submission event indicates the command list the api sequence numbers
            // belong to. Because more than one command list can be contained in
            // a Command buffer submission we might get interleaved events for
            // different command lists.
            // Code below uses a LastCommandListInstanceId to decide if api sequence numbers
            // are to be appended or a new tracking entry needs to be created.

            auto submission = std::ranges::find_if(m_submissions,
                [&](auto const& s)
            {
                return s.Context == context && s.SubmitSequence == submitCommandCbSequence;
            });

            if (submission == m_submissions.end())
            {
                // This is the first event for this submission
                CommandBufferSubmission s;
                s.Context = *context;
                s.SubmitSequence = submitCommandCbSequence;
                s.LastLoopIteration = loopIteration;
                submission = m_submissions.insert(submission, std::move(s));
            }
            else
            {
                assert(submission->Context == context);
                assert(submission->SubmitSequence == submitCommandCbSequence);

                // The LoopIteration field is emitted to try and spot when
                // events are missed.  We don't think this can ever happen.
                assert(loopIteration > submission->LastLoopIteration);
                submission->LastLoopIteration = loopIteration;
            }

            // We cannot assume command lists are always executed back to back.
            // If a different command list is detected a new entry will be added.
            // The order of the CommandListInstanceIdAndApiSequenceNumbers is important
            // to maintain as this is how we ensure correct mapping of the history
            // buffer entries to operations.
            if (submission->LastCommandListInstanceId != commandListInstanceId)
            {
                submission->CommandListInstanceIdAndApiSequenceNumbers.push_back({ commandListInstanceId });
            }
            submission->LastCommandListInstanceId = commandListInstanceId;

            auto commandListInfo = &submission->CommandListInstanceIdAndApiSequenceNumbers.back();
            auto& apiSequenceNumbers = commandListInfo->ApiSequenceNumbers;
            apiSequenceNumbers.reserve(apiSequenceNumbers.size() + completedApiSequenceNumberSize);

            for (auto i = 0u; i < completedApiSequenceNumberSize; ++i)
            {
                // We assume firstApiSequenceNumberHigh is always 0.  If it ever
                // isn't 0 then all sorts of questions need to be answered (eg why
                // are there billions of commands in the submission? what happens
                // if the low part rolls over?)
                assert(firstApiSequenceNumberHigh == 0);

                apiSequenceNumbers.push_back(static_cast<uint64_t>(completedApiSequenceNumbers[i]));
            }
        }

        template <>
        void OnD3D12Event_Start(EVENT_RECORD* record, D3D12ExecuteCommandListArgs args)
        {
            if (!IsThisProcess(record))
            {
                return;
            }

            auto& instances = GetPerThreadData(record).CurrentCommandListInstances;
            instances.clear();
            instances.push_back(m_commandListToId[args.commandList]);
        }

        template <>
        void OnD3D12Event_Stop(EVENT_RECORD* record, D3D12CommandListArgs args)
        {
            if (!IsThisProcess(record))
            {
                return;
            }

            GetPerThreadData(record).CurrentCommandListInstances.clear();
        }

        template <>
        void OnD3D12Event_Start(EVENT_RECORD* record, D3D12ExecuteCommandListsArgs args)
        {
            if (!IsThisProcess(record))
            {
                return;
            }

            auto& instances = GetPerThreadData(record).CurrentCommandListInstances;
            instances.clear();
            for (auto commandList : args.commandLists)
            {
                instances.push_back(m_commandListToId[commandList]);
            }
        }

        template <>
        void OnD3D12Event_Stop(EVENT_RECORD* record, D3D12ExecuteCommandListsArgs args)
        {
            if (!IsThisProcess(record))
            {
                return;
            }

            GetPerThreadData(record).CurrentCommandListInstances.clear();
        }

        template <>
        void OnD3D12Event_Info(EVENT_RECORD* record, D3D12HistoryBufferCompletionArgs args)
        {
            // HW scheduling uses a HW Queue instead of the broadcast context.
            // Everything else is the same so reuse the existing History buffer handler.
            HandleWriteHistoryBuffer(
                record,
                args.hwQueueHandle,
                args.renderCbSequence,
                args.precision,
                args.historyBufferSize,
                args.historyBuffer);
        }

        template <>
        void OnDxgkEvent_Info(EVENT_RECORD* record, DxgkHistoryBufferArgs args)
        {
            HandleWriteHistoryBuffer(
                record,
                args.context,
                args.renderCbSequence,
                args.precision,
                args.historyBufferSize,
                args.historyBuffer);
        }

        void HandleWriteHistoryBuffer(
            EVENT_RECORD* record,
            uint64_t context,
            uint32_t renderCbSequence,
            uint32_t precision,
            uint32_t historyBufferSize,
            uint8_t* historyBuffer)
        {
            // TODO: we probably should not assume that this device supports ToP
            // and EoP...but for now we do.

            auto submission = std::ranges::find_if(m_submissions,
                [&](auto const& s) { return s.Context == context && s.SubmitSequence == renderCbSequence; });

            if (submission == m_submissions.end())
            {
                return;
            }

            uint64_t cpuClock = record->EventHeader.TimeStamp.QuadPart;

            // The timestamps might be 32 or 64 bit
            if (precision <= 32)
            {
                uint32_t* timestamps = reinterpret_cast<uint32_t*>(historyBuffer);
                auto numTimestamps = historyBufferSize / sizeof(uint32_t);
                auto numPairs = numTimestamps / 2;

                for (auto i = 0u; i < numPairs; ++i)
                {
                    submission->HistoryBuffer.push_back({ timestamps[i * 2], timestamps[i * 2 + 1], cpuClock });
                }
            }
            else
            {
                // On top of that, the precision field may indicate that the top
                // bits aren't used.
                uint64_t mask;
                if (precision == 64)
                {
                    mask = 0xFFFFFFFFFFFFFFFF;
                }
                else
                {
                    mask = (1ULL << precision) - 1;
                }

                uint64_t* timestamps = reinterpret_cast<uint64_t*>(historyBuffer);
                auto numTimestamps = historyBufferSize / sizeof(uint64_t);
                auto numPairs = numTimestamps / 2;

                for (auto i = 0u; i < numPairs; ++i)
                {
                    submission->HistoryBuffer.push_back({ timestamps[i * 2] & mask, timestamps[i * 2 + 1] & mask, cpuClock });
                }
            }

            // Validate the overall submission timestamp values
            if (!submission->HistoryBuffer.empty() && submission->HistoryBuffer[0].EoP < submission->HistoryBuffer[0].ToP)
            {
                std::wcout << L"Invalid Start/End Submission timestamp detected. EoP timestamp for overall submission is smaller than ToP timestamp!" << std::endl;
            }
        }

        template <>
        void OnDxgkEvent_Start(EVENT_RECORD* record, DxgkHwQueueArgs args)
        {
            m_dxgkContexts.UpdateHwQueue(args.parentDxgContext, args.hwQueueHandle, args.hwQueue);
        }

        template <>
        void OnDxgkEvent_Stop(EVENT_RECORD* record, DxgkHwQueueArgs args)
        {
            m_dxgkContexts.UpdateHwQueue(args.parentDxgContext, args.hwQueueHandle, args.hwQueue);
        }

        template <>
        void OnDxgkEvent_DCStart(EVENT_RECORD* record, DxgkHwQueueArgs args)
        {
            m_dxgkContexts.UpdateHwQueue(args.parentDxgContext, args.hwQueueHandle, args.hwQueue);
        }

        template <>
        void OnDxgkEvent_Start(EVENT_RECORD* record, DxgkContextArgs args)
        {
            m_dxgkContexts.UpdateContext(args.context, args.contextHandle, args.device, args.nodeOrdinal, args.engineAffinity);
        }

        template <>
        void OnDxgkEvent_Stop(EVENT_RECORD* record, DxgkContextArgs args)
        {
            m_dxgkContexts.UpdateContext(args.context, args.contextHandle, args.device, args.nodeOrdinal, args.engineAffinity);
        }

        template <>
        void OnDxgkEvent_DCStart(EVENT_RECORD* record, DxgkContextArgs args)
        {
            m_dxgkContexts.UpdateContext(args.context, args.contextHandle, args.device, args.nodeOrdinal, args.engineAffinity);
        }

        template <>
        void OnDxgkEvent_Start(EVENT_RECORD* record, DxgkDeviceArgs args)
        {
            m_dxgkContexts.UpdateDevice(args.device, args.dxgAdapter, args.processId, args.thunkHandle);
        }

        template <>
        void OnDxgkEvent_Stop(EVENT_RECORD* record, DxgkDeviceArgs args)
        {
            m_dxgkContexts.UpdateDevice(args.device, args.dxgAdapter, args.processId, args.thunkHandle);
        }

        template <>
        void OnDxgkEvent_DCStart(EVENT_RECORD* record, DxgkDeviceArgs args)
        {
            m_dxgkContexts.UpdateDevice(args.device, args.dxgAdapter, args.processId, args.thunkHandle);
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

        bool IsThisProcess(EVENT_RECORD* record)
        {
            return record->EventHeader.ProcessId == m_processId;
        }

        void UpdateD3D12Device(uint64_t device, uint64_t umDeviceVersion)
        {
            m_d3d12DeviceUmVersions[device] = umDeviceVersion;
        }

        void StartNewCommandList(uint64_t commandList, uint64_t device, uint64_t sequenceNumber)
        {
            // The same command list object (as identified by 'commandList') may
            // be reused.  Each time it is reset it is assigned a new
            // 'sequenceNumber'.  This is unique per device, and we call that a
            // "CommandListInstance".

            m_commandListToId[commandList] = CommandListInstanceId{ device, sequenceNumber };
        }

        PerThreadData& GetPerThreadData(EVENT_RECORD* record)
        {
            return m_perThreadData[record->EventHeader.ThreadId];
        }
    };
}
