// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#pragma once

namespace DirectX::Etw
{

struct RuntimeMarkerDataEntry
{
    EMarkerOp MarkerOp;
    ApiMarkerEventId ApiMarkerEventId;
    uint64_t TimestampNs;
    uint64_t ApiSequenceNumber;
    uint32_t ThreadId;
    size_t WinPixEventIndex;
};

struct WinPixEventEntry
{
    WinPixEventId PixEventId;
    PixEventType EventType;
    uint64_t TimeStamp;
};

struct GpuWorkEvent
{
    UINT64 HardwareCommandQueueId;
    UINT64 ApiCommandQueueId;
    UINT64 ApiMarkerId;
    UINT32 CommandListId; // Identifies the command list for an execution
    INT64 BeginTimestamp;
    INT64 EndTimestamp;
};

class PixMarkerTracker
{
    std::vector<size_t> m_events;
    std::vector<std::pair<uint32_t, uint32_t>> m_threadToEventTotals;
public:

    void Add(RuntimeMarkerDataEntry marker, size_t markerIndex)
    {
        m_events.push_back(markerIndex);
        auto it = std::find_if(m_threadToEventTotals.begin(), m_threadToEventTotals.end(), [&](auto& e) {return e.first == marker.ThreadId; });
        if (it != m_threadToEventTotals.end())
            it->second++;
        else
            m_threadToEventTotals.emplace_back(marker.ThreadId, 1);
    }

    auto begin() const { return m_threadToEventTotals.begin(); }
    auto end() const { return m_threadToEventTotals.end(); }

    auto empty() const { return m_events.empty(); }
    const size_t operator[](size_t index) const { return m_events[index]; }
    size_t Size() const { return m_events.size(); }

    void Clear()
    {
        m_events.clear();
        m_threadToEventTotals.clear();
    }
};

// This class manages PIX events for both CommandList and CommandQueues
// separately.  CommandList events are stored per-context/per-thread.
// CommandQueue events are stored per-context only and are sorted by
// time.
class WinPixEventStore
{
    struct PerThreadData
    {
        std::unordered_map<WinPixEventContextId, std::queue<WinPixEventEntry>> Events;

        void AddEvent(WinPixEventContextId contextId, WinPixEventEntry entry)
        {
            Events[contextId].push(std::move(entry));
        }
    };

    struct PerQueueData
    {
        std::vector<WinPixEventEntry> Events; // sorted by timestamp

        void AddEvent(WinPixEventEntry entry)
        {
            // CommandQueue events must be inserted in sorted order because they can come in on any thread
            // and any order.
            auto it = std::upper_bound(Events.begin(), Events.end(), entry, [](auto& lhs, auto& rhs)
                {
                    return lhs.TimeStamp > rhs.TimeStamp;
                });
            Events.insert(it, std::move(entry));
        }
    };

    std::map<uint32_t, PerThreadData> m_perThreadData;
    std::map<WinPixEventContextId, PerQueueData> m_perQueueData;

public:

    bool HasEnoughEvents(WinPixEventContextId contextId, PixMarkerTracker* tracker)
    {
        for (auto& requiredTotals : *tracker)
        {
            auto threadId = requiredTotals.first;
            auto totalEvents = requiredTotals.second;

            if (m_perThreadData[threadId].Events[contextId].size() < totalEvents)
                return false;
        }

        return true;
    }

    void AddCommandQueuePixEvent(WinPixEventContextId contextId, WinPixEventEntry entry)
    {
        m_perQueueData[contextId].AddEvent(std::move(entry));
    }

    void AddPixEvent(uint32_t threadId, WinPixEventContextId contextId, WinPixEventEntry entry)
    {
        m_perThreadData[threadId].AddEvent(contextId, std::move(entry));
    }

    std::optional<WinPixEventEntry> TakeCommandQueueEventBeforeTimestamp(WinPixEventContextId contextId, uint64_t timestamp)
    {
        auto& pixEvents = m_perQueueData[contextId].Events;
        if (pixEvents.empty())
            return std::nullopt;

        if (pixEvents.back().TimeStamp > timestamp)
            return std::nullopt;

        WinPixEventEntry entry = pixEvents.back();
        pixEvents.pop_back();

        return entry;
    }

    std::optional<WinPixEventEntry> TakeCommandListEvent(WinPixEventContextId contextId, PixEventType eventType, uint32_t threadId, uint64_t timestamp)
    {
        auto& perThreadData = m_perThreadData[threadId];
        auto& pixEvents = perThreadData.Events[contextId];

        if (pixEvents.empty())
        {
            return std::nullopt;
        }

        if (pixEvents.front().TimeStamp > timestamp)
        {
            return std::nullopt;
        }

        // Grab the next event. This might not be the best event as there can be data in the event store
        // from command lists that we are not tracking. To narrow it down we seek through the events
        // and find the closest event in time.
        WinPixEventEntry entry = pixEvents.front();
        pixEvents.pop();

        bool eventChosen = true;
        if (!pixEvents.empty() && pixEvents.front().TimeStamp < timestamp)
        {
            eventChosen = false;
            while (!pixEvents.empty())
            {
                auto tempEntry = pixEvents.front();
                pixEvents.pop();

                if (pixEvents.empty())
                {
                    entry = tempEntry;
                    eventChosen = true;
                    break;
                }

                // If the next entry is larger so take the temp entry
                // as the choice.
                if (pixEvents.front().TimeStamp > timestamp)
                {
                    entry = tempEntry;
                    eventChosen = true;
                    break;
                }

                // If the next entry is equal and the event type does not match,
                // take the temp entry as the choice.
                if (pixEvents.front().TimeStamp == timestamp && pixEvents.front().EventType != eventType)
                {
                    entry = tempEntry;
                    eventChosen = true;
                    break;
                }
            }

            if (!eventChosen)
            {
                return std::nullopt;
            }
        }

        if (entry.EventType != eventType)
        {
            return std::nullopt;
        }

        return entry;
    }
};

struct StartExecutionTime
{
    uint32_t ThreadId = 0;
    uint64_t CpuSubmitTimestamp = 0;
    std::wstring CommandListName;
};

struct CommandQueueInstance
{
    CommandQueueInstanceId Id;
    ApiCommandQueueId ApiCommandQueueId;
    WinPixEventContextId PixEventContextId;
    D3D12_COMMAND_LIST_TYPE CommandListType;
    uint64_t CreationTimestamp;
    std::wstring Name;
    bool IsDestroyed;
};

struct CommandListInstance
{
    CommandListInstanceId Id;
    std::vector<RuntimeMarkerDataEntry> RuntimeMarkers;
    size_t RuntimeMarkerProcessed = 0;
    uint64_t FirstTopOfPipeGpuTimestampNs = 0;
    std::vector<GpuEvent> PixGpuEvents;
    WinPixEventContextId PixEventContextId;
    std::wstring Name;
    PixMarkerTracker ExpectedWinPixEvents;
    std::vector<WinPixEventEntry> WinPixEvents;
    std::queue<StartExecutionTime> ExecutionStarts;
    std::queue<uint64_t> TopOfPipeGpuStartTimes;
    size_t WinPixEventMarkerProcessed = 0;
    bool Deleted = false;
    bool IgnoreSubmissions = false;
    std::vector<GpuWorkEvent> GpuWorkEvents;

    struct EventTimingInfo {
        uint64_t minTOP;
        uint64_t maxEOP;
        size_t beginEventIndex;
    };
    std::vector<EventTimingInfo> EventTimings;

    void PrepareForNextExecution()
    {
        PixGpuEvents.clear();
        GpuWorkEvents.clear();
        FirstTopOfPipeGpuTimestampNs = 0;
        RuntimeMarkerProcessed = 0;
        WinPixEventMarkerProcessed = 0;
        if (!ExecutionStarts.empty())
        {
            ExecutionStarts.pop();
        }
    }

    void FinalizeEventIds(PixEventCallbacks* pixEventCallbacks)
    {
        if (WinPixEvents.empty())
            return;

        std::vector<uint64_t> eventIds;
        eventIds.resize(WinPixEvents.size());
        for (auto const& pixEvent : WinPixEvents)
        {
            eventIds.push_back(pixEvent.PixEventId.Value);
        }

        ThrowFailure(pixEventCallbacks->FinalizeGpuEventsForCpuEventIds(eventIds.data(), (uint32_t)eventIds.size()));
    }

    bool IsFullyProcessed()
    {
        assert(RuntimeMarkerProcessed <= RuntimeMarkers.size());
        return (RuntimeMarkerProcessed == RuntimeMarkers.size());
    }

    bool CanBeDeleted()
    {
        return (Deleted && (ExecutionStarts.empty() || IsInternalPresentCommandList()));
    }

    bool IsInternalPresentCommandList()
    {
        // The internal present CommandList payloads contain 3 runtime marker operations.
        // BeginSubmission, Present, EndSubmission.
        // A good way to filter out these submissions is to look for the Present op
        // at index 1 (the middle).
        if (RuntimeMarkers.size() == 3 && RuntimeMarkers[1].MarkerOp == e_MOp_Present)
        {
            return true;
        }
        return false;
    }

    void ProcessCommandQueueEvents(CommandQueueInstance const* commandQueue, WinPixEventStore* eventStore)
    {
        if (ExecutionStarts.empty())
            return;

        // Add all events that occured before execution of the command list
        auto timeStampBeforeExecution = ExecutionStarts.front().CpuSubmitTimestamp;
        std::vector<GpuEvent> events;
        int index = 0;
        while (auto winPixEvent = eventStore->TakeCommandQueueEventBeforeTimestamp(commandQueue->PixEventContextId, timeStampBeforeExecution))
        {
            GpuEvent gpuEvent = {};
            gpuEvent.ApiCommandQueueId = commandQueue->ApiCommandQueueId.Value;
            gpuEvent.CpuContextId = winPixEvent->PixEventId.Value;
            gpuEvent.Type = winPixEvent->EventType;
            gpuEvent.Timestamp = FirstTopOfPipeGpuTimestampNs;

            PixGpuEvents.insert(PixGpuEvents.begin() + index, gpuEvent);
            index++;
        }
    }

    static PixEventType ToPixEventType(EMarkerOp op)
    {
        switch (op)
        {
        case e_MOp_BeginEvent:
            return PixEventType::Begin;
        case e_MOp_EndEvent:
            return PixEventType::End;
        case e_MOp_SetMarker:
            return PixEventType::Marker;
        }

        assert(false);
        return PixEventType::Marker;
    }

    bool MatchPixEventsToMarkersFromEventStore(WinPixEventStore* eventStore)
    {
        // Check to see if there are enough events in the event store
        // to process this command list.
        if (!eventStore->HasEnoughEvents(PixEventContextId, &ExpectedWinPixEvents))
            return false;

        // Gather all pix events for the command list
        for (size_t i = WinPixEventMarkerProcessed; i < ExpectedWinPixEvents.Size(); i++)
        {
            size_t markerIndex = ExpectedWinPixEvents[i];
            auto runtimeMarker = RuntimeMarkers[markerIndex];
            auto winPixEvent = eventStore->TakeCommandListEvent(
                PixEventContextId,
                ToPixEventType(runtimeMarker.MarkerOp),
                runtimeMarker.ThreadId,
                runtimeMarker.TimestampNs);

            if (winPixEvent && winPixEvent->EventType == ToPixEventType(runtimeMarker.MarkerOp))
            {
                WinPixEvents.push_back(*winPixEvent);
                WinPixEventMarkerProcessed++;
            }
            else
            {
                if (!WinPixEvents.empty())
                {
                    // At least one event has been successfully matched so it is assumed there
                    // are more coming.
                    return false;
                }

                // No matching pix event was found in the event store so clear
                // expectations about receiving them and proceed to
                // write out gpu events without PIX markers.
                // This can occur when ETW is turned on while a game is running.
                // Blocks of PIX markers might contain fragments from a previous operation.
                ClearExpectedPixEventTrackingData();
                WinPixEventMarkerProcessed = 0;
                return true;
            }
        }
        return (ExpectedWinPixEvents.Size() == WinPixEvents.size());
    }

    void MakePixEventsFromMarkers(uint32_t processId, PixEventCallbacks* pixEventCallbacks)
    {
        // Create CPU events from the runtime marker data and write them in batches by thread
        // to maintain order
        struct EventBatchEntry
        {
            uint32_t ThreadId;
            size_t FirstIndex;
            size_t Total;
        };

        size_t totalEvents = ExpectedWinPixEvents.Size();
        std::vector<PixCpuEvent> events(totalEvents);
        std::vector<uint64_t> eventIds(totalEvents);
        std::vector<EventBatchEntry> batches;
        uint32_t lastThreadId = 0;

        for (size_t i = 0; i < totalEvents; i++)
        {
            size_t markerIndex = ExpectedWinPixEvents[i];
            auto runtimeMarker = RuntimeMarkers[markerIndex];

            PixCpuEvent& event = events[i];
            event.Timestamp = runtimeMarker.TimestampNs;
            event.Name = L"unknown PIX event string";
            event.Color = 0;
            event.Type = ToPixEventType(runtimeMarker.MarkerOp);
            event.HasContext = TRUE;

            if (runtimeMarker.ThreadId != lastThreadId)
            {
                batches.push_back({ runtimeMarker.ThreadId, i, 1 });
            }
            else
            {
                batches.back().Total++;
            }

            lastThreadId = runtimeMarker.ThreadId;
        }

        for (auto& batch : batches)
        {
            ThrowFailure(pixEventCallbacks->OnPixEvents(processId, batch.ThreadId, &events[batch.FirstIndex], &eventIds[batch.FirstIndex], (UINT32)batch.Total));

            for (size_t i = batch.FirstIndex; i < batch.FirstIndex + batch.Total; i++)
            {
                WinPixEventEntry winPixEvent;
                winPixEvent.EventType = events[i].Type;
                winPixEvent.PixEventId = WinPixEventId(eventIds[i]);
                winPixEvent.TimeStamp = events[i].Timestamp;
                WinPixEvents.push_back(std::move(winPixEvent));
            }
        }

        assert(WinPixEvents.size() == ExpectedWinPixEvents.Size());
        WinPixEventMarkerProcessed = WinPixEvents.size();
    }

    void ClearExpectedPixEventTrackingData()
    {
        ExpectedWinPixEvents.Clear();
        WinPixEvents.clear();
    }

    bool MatchPixEventsToMarkers(uint32_t processId, WinPixEventStore* eventStore, int64_t lastTimestampNs)
    {
        if (ExpectedWinPixEvents.empty())
            return true;

        if (ExpectedWinPixEvents.Size() == WinPixEvents.size())
            return true;

        // A command list indicates that it is expecting PIX events to be matched
        // by the presence of SetMarker/BeginEvent or EndEvent op codes.
        //
        // The majority of the time these op codes get recorded in the command
        // list via calls to WinPixEventRuntime's PIXSetMarker/PIXBeginEvent, etc.
        // PIX events will be eventually reported and collected into this decoder's
        // PIX event store.
        //
        // Some applications might have been compiled using the old school <pix.h>
        // method of recording PIX events.  We do not support this event logging
        // anymore, but we do have a fallback mechanism which will allow GPU timings
        // to be reported on the timeline.
        //
        // The fallback mechanism removes all assumptions that there are
        // any PIX events and writes out just the gpu events.

        // First check the PIX event store to see if there is a matched event
        // for this command list.  If ALL events are matched 'true' will be returned
        // and WinPixEvents will be populated.
        // If SOME events are matched, 'false' will be returned and WinPixEvents will
        // contain SOME events.
        uint64_t duration = lastTimestampNs - ExecutionStarts.front().CpuSubmitTimestamp;
        constexpr uint64_t MatchFailureTimeoutNs = 10'000'000'000; // 10 seconds
        bool result = MatchPixEventsToMarkersFromEventStore(eventStore);

        // If we fail to match ANY events the fallback mechanism is a considered
        // option and will be used if the command list has been hanging out
        // without ANY events for a period of time.
        if (!result && WinPixEvents.empty())
        {
            if (duration >= MatchFailureTimeoutNs)
            {
                ClearExpectedPixEventTrackingData();
                return true;
            }
        }

        return result;
    }
};

struct CommandBufferSubmissionEntry
{
    uint64_t Context = 0;
    uint32_t SubmitSequence = 0;

    CommandListInstanceId LastCommandListId;
    std::vector<DirectX::Etw::CommandListIdAndApiSequenceNumbers> CommandListIdAndApiSequenceNumbers;

    std::vector<GpuWorkEvent> CompletedCommandListsGpuWorkEvents;
    std::vector<GpuEvent> CompletedCommandListsPixGpuEvents;
    std::vector<uint64_t> CompletedCommandListWinPixEventIds;
    std::vector<CommandListInstance> CompletedCommandListsToDelete;

    // Friendly names (ID3D12Object::SetName) for command lists in this submission, keyed by the same
    // 32-bit command list id that is reported to StoreGpuWork2 as commandListIndexInExecution.
    std::vector<std::pair<uint32_t, std::wstring>> CompletedCommandListNames;

    CommandQueueInstanceId CommandQueueId;

    uint32_t LastLoopIteration = 0;

    std::vector<HistoryBufferEntry> HistoryBuffer;
    size_t ExpectedHistoryBuffers = 1; // Default to 1 to include TOP/EOP timestamp pair that appears at the top of every history buffer set
    uint64_t TimestampMask = 0;

    uint64_t CalculateGpuTimestampNs(const CalibratedClockEntry& entry, const TimestampConverter* timestampConverter, uint64_t timestamp)
    {
        uint64_t gpuTicks = entry.GpuClockTicks & TimestampMask;
        uint64_t gpuTimestampNs = TicksToNanoseconds(gpuTicks, entry.GpuFrequency);
        uint64_t cpuTimestampNs = timestampConverter->ConvertClockToTimeStamp(entry.CpuClockTicks);
        uint64_t finalTimestampNs = TicksToNanoseconds(timestamp, entry.GpuFrequency);
        return (cpuTimestampNs + (finalTimestampNs - gpuTimestampNs));
    }

    bool IsPixEvent(RuntimeMarkerDataEntry entry)
    {
        return (entry.MarkerOp == e_MOp_BeginEvent ||
            entry.MarkerOp == e_MOp_SetMarker ||
            entry.MarkerOp == e_MOp_EndEvent);
    }

    void HandlePixEvent(RuntimeMarkerDataEntry entry, uint64_t topNs, uint64_t eopNs, CommandListInstance* commandList, uint64_t apiCommandQueueId, DiagnosticsSink* diagnosticsSink)
    {
        if (commandList->WinPixEvents.empty())
            return;

        GpuEvent gpuEvent = {};
        gpuEvent.ApiCommandQueueId = apiCommandQueueId;
        gpuEvent.CpuContextId = commandList->WinPixEvents[entry.WinPixEventIndex].PixEventId.Value;

        switch (entry.MarkerOp)
        {
        case e_MOp_BeginEvent:
        {
            gpuEvent.Type = PixEventType::Begin;
            gpuEvent.Timestamp = topNs;

            // We process GPU events in the order they were submitted to the cmdlist (order of the history buffers),
            // but the timestamps in the history buffers can be re-ordered, changing what appears inside the PIX events.
            // However since the GPU events come in submission order we can track the min TOP and max EOP of the work that occurs
            // between the Begin and End events in a stack. When we see the End event we can pop the info off the stack and update
            // all the inner event timestamps.

            // Start tracking a new Begin/End pair
            commandList->EventTimings.push_back({ (uint64_t)gpuEvent.Timestamp, eopNs, commandList->PixGpuEvents.size() });
        }
        break;
        case e_MOp_EndEvent:
        {
            gpuEvent.Type = PixEventType::End;
            gpuEvent.Timestamp = eopNs;

            // If there was any GPU work that occurred between this End event and its corresponding Begin event,
            // we need to make sure the two events contain all the GPU work that occurred between them.
            if (!commandList->EventTimings.empty())
            {
                const auto& innerTimings = commandList->EventTimings.back();
                commandList->EventTimings.pop_back();

                gpuEvent.Timestamp = std::max((uint64_t)gpuEvent.Timestamp, innerTimings.maxEOP);

                // Update the corresponding Begin event. If any work occurred before the Begin event's timestamp,
                // the timestamp will be updated to the earliest work's ToP
                auto& beginEvent = commandList->PixGpuEvents[innerTimings.beginEventIndex];
                beginEvent.Timestamp = std::min((uint64_t)beginEvent.Timestamp, innerTimings.minTOP);
                assert(beginEvent.Timestamp <= gpuEvent.Timestamp);

                // Update the inner Begin and End events.
                // The Begin event timestamps should occur no earlier than the enclosing Begin event.
                // The End event timestamps should occur no later than the enclosing End event.
                // Don't modify any PixEventType::Marker events, as we want to show these as reported by the GPU.
                // This may mean a Marker that should appear inside a particular event doesn't, but there isn't much
                // we can do about that.
                for (size_t i = innerTimings.beginEventIndex + 1; i < commandList->PixGpuEvents.size(); ++i)
                {
                    auto& innerEvent = commandList->PixGpuEvents[i];
                    if (innerEvent.Type == PixEventType::Begin)
                    {
                        innerEvent.Timestamp = std::max(innerEvent.Timestamp, beginEvent.Timestamp);
                    }
                    else if (innerEvent.Type == PixEventType::End)
                    {
                        innerEvent.Timestamp = std::min(innerEvent.Timestamp, gpuEvent.Timestamp);
                    }
                }

                // Update parent event with all of this event's timing info.
                // In other words, expand the parent event time range to include the current event.
                if (!commandList->EventTimings.empty())
                {
                    auto& timings = commandList->EventTimings.back();
                    timings.minTOP = std::min(timings.minTOP, innerTimings.minTOP);
                    timings.maxEOP = std::max(timings.maxEOP, innerTimings.maxEOP);
                }
            }
            else
            {
                // we still want to save the PixGpuEvent even if it could look wrong
                diagnosticsSink->OnDiagnostic(
                    DiagnosticSeverity::Warning,
                    DiagnosticCode::GpuEndEventWithoutBeginEvent,
                    L"GPU EndEvent encountered with no matching BeginEvent.");
            }
        }
        break;
        case e_MOp_SetMarker:
        {
            gpuEvent.Type = PixEventType::Marker;
            gpuEvent.Timestamp = eopNs;

            if (!commandList->EventTimings.empty())
            {
                auto& timings = commandList->EventTimings.back();
                timings.minTOP = std::min(timings.minTOP, topNs);
                timings.maxEOP = std::max(timings.maxEOP, eopNs);
            }
        }
        break;

        // No PixGpuEvents for these markers, but we need to track GPU work
        case e_MOp_BeginSubmission:
        case e_MOp_EndSubmission:
            return;
        default:
        {
            // GPU work
            if (!commandList->EventTimings.empty())
            {
                auto& timings = commandList->EventTimings.back();
                timings.minTOP = std::min(timings.minTOP, topNs);
                timings.maxEOP = std::max(timings.maxEOP, eopNs);
            }
        }
        return;
        }

        commandList->PixGpuEvents.push_back(gpuEvent);
    }

    void HandleGpuWork(CommandListInstance* commandList, UINT64 hardwareCommandQueueId, UINT64 apiCommandQueueId, UINT64 apiMarkerId, INT64 beginTimestamp, INT64 endTimestamp)
    {
        uint32_t commandListId = static_cast<uint32_t>(std::get<1>(commandList->Id)); // casting a 64-bit sequence number to 32-bit id should be fine as this is mainly used to uniquely identify a command list in 32-bit terms
        GpuWorkEvent gpuEvent = { hardwareCommandQueueId, apiCommandQueueId, apiMarkerId, commandListId, beginTimestamp, endTimestamp };
        commandList->GpuWorkEvents.push_back(std::move(gpuEvent));
    }

    static void EraseCommandListIdTrackingData(
        std::map<uint64_t, std::vector<std::pair<bool, CommandListInstanceId>>>* commandListToIds,
        CommandListInstance* commandList)
    {
        auto itIds = commandListToIds->find(commandList->PixEventContextId.Value);
        if (itIds != commandListToIds->end())
        {
            auto itInst = std::find_if(itIds->second.begin(), itIds->second.end(), [&](auto& i) { return i.second == commandList->Id; });
            if (itInst != itIds->second.end())
            {
                itIds->second.erase(itInst);
            }
        }
    }

    bool TryWriteGpuEvents(
        TimestampConverter const* timestampConverter,
        AdapterTracker* adapters,
        DxgkContexts* dxgContexts,
        WinPixEventStore* eventStore,
        uint32_t processId,
        std::map<CommandListInstanceId, CommandListInstance>* commandLists,
        std::map<CommandQueueInstanceId, CommandQueueInstance>* commandQueues,
        std::map<uint64_t, std::vector<std::pair<bool, CommandListInstanceId>>>* commandListToIds,
        GpuTimingsCallbacks* gpuTimingsCallbacks,
        PixEventCallbacks* pixEventCallbacks,
        bool ignoreMissingPixEvents,
        DiagnosticsSink* diagnosticsSink)
    {
        // Validate that the command queue is known before we proceed to process
        // the submission. If any command queue is not known for this submission we can
        // ignore the  entire submission.
        //
        // Note that we do allow destroyed command queues here. There are real-world
        // scenarios where submissions come in after the command queue destruction event,
        // particularly when the submissions come in close to process shutdown. If we
        // ignore destroyed command queues here then we would be missing timing data
        // close to the process's destruction.
        auto itCommandQueueEntry = commandQueues->find(CommandQueueId);
        if (itCommandQueueEntry == commandQueues->end())
        {
            diagnosticsSink->OnDiagnostic(
                DiagnosticSeverity::Warning,
                DiagnosticCode::CommandListExecutionMissingQueue,
                L"Could not find a matching D3D12 command queue for a command list execution. Some GPU timing data will be missing.");
            return true;
        }

        auto& commandQueue = itCommandQueueEntry->second;

        {
            // Validate that all command lists for this submission are known.
            // If any command list is not known for this submission we can ignore the
            // entire submission.
            // This can happen if we recieve submissions for untracked command lists
            // reported during rundown/capture state or if a bundle command list
            // could not be fully resolved.
            std::vector<CommandListInstance*> validCommandLists;
            validCommandLists.reserve(CommandListIdAndApiSequenceNumbers.size());
            bool ignoreSubmission = false;
            for (auto& commandListInfo : CommandListIdAndApiSequenceNumbers)
            {
                auto it = commandLists->find(commandListInfo.CommandListInstanceId);
                if (it != commandLists->end())
                {
                    validCommandLists.push_back(&(it->second));
                    if (it->second.IgnoreSubmissions)
                    {
                        ignoreSubmission = true;
                    }
                }
                else
                {
                    // A command list was not found in the tracking data.
                    ignoreSubmission = true;
                }
            }

            // If this submission is to be ignored, we need to ensure that any commandlists
            // contained get properly handled
            if (ignoreSubmission)
            {
                // Sort the elements by context id which is the D3D identifier
                std::sort(validCommandLists.begin(), validCommandLists.end(),
                    [](auto& a, auto& b) { return a->PixEventContextId < b->PixEventContextId; });

                // Remove any elements with the same context id
                validCommandLists.erase(
                    std::unique(validCommandLists.begin(), validCommandLists.end(),
                        [](auto& a, auto& b) { return a->PixEventContextId == b->PixEventContextId; }),
                    validCommandLists.end());

                for (auto& commandList : validCommandLists)
                {
                    commandList->PrepareForNextExecution();
                    if (commandList->CanBeDeleted())
                    {
                        commandList->FinalizeEventIds(pixEventCallbacks);
                        EraseCommandListIdTrackingData(commandListToIds, commandList);
                        commandLists->erase(commandList->Id);
                    }
                }

                return true;
            }
        }

        // History buffers come in chunks so we need to make sure we have collected
        // all of them for every command list in this submission before continuing.
        if (HistoryBuffer.size() != ExpectedHistoryBuffers)
            return false;

        // Pix events come in blocks so we need to make sure we have collected
        // all of them for every command list tracked in this submission before continuing.
        for (auto const& commandListInfo : CommandListIdAndApiSequenceNumbers)
        {
            auto& commandList = commandLists->at(commandListInfo.CommandListInstanceId);
            if (ignoreMissingPixEvents)
            {
                commandList.ClearExpectedPixEventTrackingData();
            }
            else
            {
                // Can we match up Pix events to marker data?
                if (!commandList.MatchPixEventsToMarkers(processId, eventStore, timestampConverter->GetLastEventTimeStamp()))
                {
                    return false;
                }
            }
        }

        // We cannot process this submission until we have enough information to map the executing context
        // to a hardware command queue.
        auto& dxgContext = dxgContexts->GetOrCreateDxgContextFromContext(Context);
        std::optional<HardwareCommandQueueId> hardwareCommandQueueId = adapters->GetHardwareCommandQueueId(dxgContext.Adapter, dxgContext.EngineOrdinal);
        if (!hardwareCommandQueueId.has_value())
        {
            return false;
        }

        // We cannot process this submission if there is no calibration data
        auto const& clock = dxgContexts->GetCalibratedClock(Context);
        if (clock.IsEmpty())
        {
            return false;
        }

        // A submission contains a history buffer that can span multiple command lists.
        // This is handled below by accessing a ranged start/end section per command list
        size_t historyBufferStart = 1; // skip first entry as that is the overall ToP/EoP timestamps

        StartExecutionTime startExecutionTime = { 0, UINT64_MAX };
        for (auto const& commandListInfo : CommandListIdAndApiSequenceNumbers)
        {
            auto& commandList = commandLists->at(commandListInfo.CommandListInstanceId);

            // All commandlists will have the same start execution time as they are considered to be
            // executed together in an ExecuteCommandLists call.
            if (commandList.ExecutionStarts.front().CpuSubmitTimestamp < startExecutionTime.CpuSubmitTimestamp)
            {
                startExecutionTime = commandList.ExecutionStarts.front();
            }
            assert(startExecutionTime.CpuSubmitTimestamp > 0);

            // We do not want to process internal 'Present' submissions at all.  Present operations are performed
            // by the D3D12 runtime via an internal command list added to the app's command queue.
            // These command lists or may not have History buffers. This is because some present
            // operations cause GPU work and some do not.
            // According to with the D3D12 team there is no way to determine if a submission
            // for a 'Present' will ever get History buffers so we ignore all of them.
            // Presents and Vsyncs will be represented using a different ETW event set.
            if (commandList.IsInternalPresentCommandList())
            {
                // Report that the submission has been handled and immediately delete the
                // command list from the tracking list.  The D3D12 runtime will never execute
                // this command list multiple times without resetting it in between.
                commandLists->erase(commandList.Id);
                return true;
            }

            // Skip the first entry in the history buffer as it does not belong
            // to a command list operation.
            size_t apiSequenceNumberIndex = 0;
            size_t const historyBufferEnd = historyBufferStart + commandListInfo.ApiSequenceNumbers.size();

#define DEBUG_HISTORY_BUFFER 0
#if DEBUG_HISTORY_BUFFER
            {
                const auto& historyBufferEntry = HistoryBuffer[0];
                const auto& entry = clock.GetEntry(historyBufferEntry.EoP);

                uint64_t topNs = CalculateGpuTimestampNs(entry, timestampConverter, historyBufferEntry.ToP);
                uint64_t eopNs = CalculateGpuTimestampNs(entry, timestampConverter, historyBufferEntry.EoP);
                str::print(std::wcout, L"{} HistoryBuffer[{}]: topNs={}ns, eopNs={}ns, \n",
                    std::get<1>(commandListInfo.CommandListInstanceId), 0, topNs, eopNs);
            }
#endif

            for (size_t i = historyBufferStart; i < historyBufferEnd; ++i)
            {
                const auto& historyBufferEntry = HistoryBuffer[i];
                const auto& entry = clock.GetEntry(historyBufferEntry.EoP);

                uint64_t topNs = CalculateGpuTimestampNs(entry, timestampConverter, historyBufferEntry.ToP);
                uint64_t eopNs = CalculateGpuTimestampNs(entry, timestampConverter, historyBufferEntry.EoP);

                if (eopNs < topNs)
                {
                    diagnosticsSink->OnDiagnostic(
                        DiagnosticSeverity::Error,
                        DiagnosticCode::TimestampEndBeforeStart,
                        std::format(
                            L"Timestamp calculation error! - EOP timestamp occurs before TOP timestamp. "
                            L"Calculated values: TOP={}ns, EOP={}ns. HistoryBuffer values: TOP={}, EOP={}.",
                            topNs,
                            eopNs,
                            historyBufferEntry.ToP,
                            historyBufferEntry.EoP));
                }

                // Api sequence numbers are 1-based.  They need to be changed to 0-based
                // so they can be used to index into the RuntimeMarkers vector to obtain
                // the marker entry to process.
                size_t runtimeMarkerIndex = commandListInfo.ApiSequenceNumbers[apiSequenceNumberIndex] - 1;
                apiSequenceNumberIndex++;

                const auto& runtimeMarkerEntry = commandList.RuntimeMarkers[runtimeMarkerIndex];

#if DEBUG_HISTORY_BUFFER
                {
                    str::print(std::wcout, L"{} HistoryBuffer[{}]: topNs={}ns, eopNs={}ns, runtimeMarkerIndex={}, runtimeMarkerEntry.TimestampNs={}ns, runtimeMarkerEntry.MarkerOp={}, runtimeMarkerEntry.WinPixEventIndex={}\n",
                        std::get<1>(commandListInfo.CommandListInstanceId), i, topNs, eopNs, runtimeMarkerIndex, runtimeMarkerEntry.TimestampNs, ToString(runtimeMarkerEntry.MarkerOp), runtimeMarkerEntry.WinPixEventIndex);
                }
#endif

                // Capture the first TOP timestamp after BeginSubmission for use in placing command queue events
                // and obtaining the first previous EOP timestamp.
                if (commandList.FirstTopOfPipeGpuTimestampNs == 0 && runtimeMarkerEntry.MarkerOp != e_MOp_BeginSubmission)
                {
                    commandList.FirstTopOfPipeGpuTimestampNs = topNs;
                }

                // HandlePixEvent needs to see all GPU events so it can track GPU work timestamps.
                HandlePixEvent(runtimeMarkerEntry,
                    topNs,
                    eopNs,
                    &commandList,
                    commandQueue.ApiCommandQueueId.Value,
                    diagnosticsSink);

                if (runtimeMarkerEntry.MarkerOp != e_MOp_BeginSubmission && runtimeMarkerEntry.MarkerOp != e_MOp_EndSubmission)
                {
                    HandleGpuWork(&commandList,
                        hardwareCommandQueueId->Value,
                        commandQueue.ApiCommandQueueId.Value,
                        runtimeMarkerEntry.ApiMarkerEventId.Value,
                        topNs,
                        eopNs);
                }

                // The runtime marker has been processed so increment the tracking
                // index value on the command list.  This Marker index value is used as
                // a cursor to indicate the current marker being processed.
                // This needs to stay with the command list as a command list can
                // span across multiple submissions.
                commandList.RuntimeMarkerProcessed++;
                assert(commandList.RuntimeMarkerProcessed <= commandList.RuntimeMarkers.size());
            }

            // If the command list is fully processed, the submission is considered completed and we can now
            // generate events.
            if (commandList.IsFullyProcessed())
            {
                // Command list events have already been processed.  ProcessCommandQueueEvents( ) assumes this as it
                // inserts Begin/SetMarker queue events before the command list events and End queue events after
                // the command list events.  This ensure proper Command Queue bracketing behaviors.
                commandList.ProcessCommandQueueEvents(&commandQueue, eventStore);

                if (!commandList.PixGpuEvents.empty())
                {
                    // Command list events can overlap with other command lists if executed together in a single
                    // ExecuteCommandLists call.  To maintain PIX event hierarchy, we shift any overlapping
                    // events to appear immediately after the previous command list.
                    if (!CompletedCommandListsPixGpuEvents.empty())
                    {
                        auto lastTimeStamp = CompletedCommandListsPixGpuEvents.back().Timestamp;
                        for (auto& event : commandList.PixGpuEvents)
                        {
                            if (event.Timestamp < lastTimeStamp)
                                event.Timestamp = lastTimeStamp;
                        }
                    }

                    // Ensure events within the commandlist do not overlap. Do this by shifting each successive event after its predecessor.
                    for (size_t i = 1; i < commandList.PixGpuEvents.size(); ++i)
                    {
                        commandList.PixGpuEvents[i].Timestamp = std::max(commandList.PixGpuEvents[i].Timestamp, commandList.PixGpuEvents[i - 1].Timestamp);
                    }

                    CompletedCommandListsPixGpuEvents.insert(CompletedCommandListsPixGpuEvents.end(), commandList.PixGpuEvents.begin(), commandList.PixGpuEvents.end());
                    commandList.PixGpuEvents.clear();
                }

                if (!commandList.GpuWorkEvents.empty())
                {
                    CompletedCommandListsGpuWorkEvents.insert(CompletedCommandListsGpuWorkEvents.end(), commandList.GpuWorkEvents.begin(), commandList.GpuWorkEvents.end());
                    commandList.GpuWorkEvents.clear();

                    std::wstring capturedCommandListName = commandList.ExecutionStarts.front().CommandListName;
                    if (!capturedCommandListName.empty())
                    {
                        uint32_t commandListId = static_cast<uint32_t>(std::get<1>(commandList.Id));
                        CompletedCommandListNames.push_back(std::make_pair(commandListId, std::move(capturedCommandListName)));
                    }
                }

                // A command list can be reused and executed again. We ensure a clean state
                // for the command list by clearing any state tracked for a single execution.
                commandList.PrepareForNextExecution();

                // If the command list can be deleted do that now as this instance is no longer needed.
                if (commandList.CanBeDeleted())
                {
                    // Copy the ids created for PIX events.  These ids need to be finalized AFTER all events
                    // are written to free up memory in the database tracking logic.
                    if (!commandList.WinPixEvents.empty())
                    {
                        size_t currentSize = CompletedCommandListWinPixEventIds.size();
                        CompletedCommandListWinPixEventIds.resize(currentSize + commandList.WinPixEvents.size());
                        for (auto const& pixEvent : commandList.WinPixEvents)
                        {
                            CompletedCommandListWinPixEventIds.push_back(pixEvent.PixEventId.Value);
                        }
                    }

                    CompletedCommandListsToDelete.push_back(commandList);
                }
            }

            historyBufferStart = historyBufferEnd;
        }

        // Write out all command list events for this submission in sorted order as
        // Command list events can be reordered during execution and appear interleaved
        // with other command list events.
        if (!CompletedCommandListsPixGpuEvents.empty())
        {
            // Stable sort is preferred to ensure that any events with identical timestamps
            // maintain their original order.  This is important when Begin and End events.
            std::stable_sort(CompletedCommandListsPixGpuEvents.begin(), CompletedCommandListsPixGpuEvents.end(), [](const auto& lhs, const auto& rhs) { return lhs.Timestamp < rhs.Timestamp; });
            ThrowFailure(pixEventCallbacks->OnPixGpuEvents(CompletedCommandListsPixGpuEvents.data(), (UINT32)CompletedCommandListsPixGpuEvents.size()));
        }

        if (!CompletedCommandListsGpuWorkEvents.empty())
        {
            uint64_t executionId = 0;
            ThrowFailure(gpuTimingsCallbacks->OnGpuExecutionBegin(processId, startExecutionTime.ThreadId, commandQueue.ApiCommandQueueId.Value, (INT64)startExecutionTime.CpuSubmitTimestamp, 0, &executionId));

            std::stable_sort(CompletedCommandListsGpuWorkEvents.begin(), CompletedCommandListsGpuWorkEvents.end(), [](const auto& lhs, const auto& rhs) { return lhs.BeginTimestamp < rhs.BeginTimestamp; });
            for (auto const& gpuEvent : CompletedCommandListsGpuWorkEvents)
            {
                ThrowFailure(gpuTimingsCallbacks->OnGpuWork(
                    gpuEvent.HardwareCommandQueueId,
                    executionId,
                    gpuEvent.CommandListId,
                    gpuEvent.ApiMarkerId,
                    gpuEvent.BeginTimestamp,
                    gpuEvent.EndTimestamp));
            }

            for (auto const& [commandListId, commandListName] : CompletedCommandListNames)
            {
                ThrowFailure(gpuTimingsCallbacks->OnCommandListName(executionId, commandListId, commandListName.c_str()));
            }

            ThrowFailure(gpuTimingsCallbacks->OnGpuExecutionComplete(executionId));
        }

        // Finalize any ids for deleted command lists to free up memory managed by the database.
        if (!CompletedCommandListWinPixEventIds.empty())
        {
            ThrowFailure(pixEventCallbacks->FinalizeGpuEventsForCpuEventIds(CompletedCommandListWinPixEventIds.data(), (uint32_t)CompletedCommandListWinPixEventIds.size()));
        }

        // Delete any commandlist instances
        for (auto& commandListToDelete : CompletedCommandListsToDelete)
        {
            EraseCommandListIdTrackingData(commandListToIds, &commandListToDelete);
            commandLists->erase(commandListToDelete.Id);
        }


        return true;
    }
};

class PerProcessData
{
    struct PerThreadData
    {
        uint32_t ThreadId;
        std::vector<CommandListInstanceId> CurrentCommandListInstanceIds;
        CommandQueueInstanceId CurrentCommandQueueInstanceId;

        struct CompilationTimingEntry
        {
            uint64_t StartTime;
            std::optional<uint64_t> StopTime;
            std::optional<uint64_t> ObjectId;
            std::optional<D3D12CacheStatisticsArgs> CacheStats;
        };
        // Note: we're making the assumption that once a start compilation event is received
        // we'll insert the CompilationTimingEntry before the next start compilation event.
        // Notably this insertion is dependent on the associated API Object insertion (because we need the object id given to us by the storage layer).
        std::unordered_map<ApiObjectType, CompilationTimingEntry> CompilationTimingEntries;

        void StartCompilationEvent(uint64_t timestamp, uint32_t processId, ApiObjectType objectType, DiagnosticsSink* diagnosticsSink)
        {
            const auto it = CompilationTimingEntries.find(objectType);
            if (it != CompilationTimingEntries.end())
            {
                diagnosticsSink->OnDiagnostic(
                    DiagnosticSeverity::Warning,
                    DiagnosticCode::PipelineStateCompilationStartWithoutObjectId,
                    std::format(
                        L"Never received API Object Id for previous Pipeline State Compilation Event. "
                        L"Dropping the event (object type {}, process {}, thread {}, start time {}ns, stop time {}).",
                        (uint64_t)objectType,
                        processId,
                        ThreadId,
                        it->second.StartTime,
                        it->second.StopTime.has_value() ? std::format(L"{}ns", it->second.StopTime.value()) : L"<unknown>"));
                CompilationTimingEntries.erase(objectType);
            }

            CompilationTimingEntries[objectType] = { timestamp, std::nullopt, std::nullopt };
        }

        void StopCompilationEvent(uint64_t timestamp, uint32_t processId, ApiObjectType objectType, PipelineStateEventCallbacks* pipelineStateEventCallbacks, DiagnosticsSink* diagnosticsSink)
        {
            auto it = CompilationTimingEntries.find(objectType);
            if (it != CompilationTimingEntries.end())
            {
                auto& entry = it->second;
                entry.StopTime = timestamp;

                TryInsertCompilationTimingEntry(processId, objectType, pipelineStateEventCallbacks);
            }
            else
            {
                diagnosticsSink->OnDiagnostic(
                    DiagnosticSeverity::Warning,
                    DiagnosticCode::PipelineStateCompilationEndWithoutStart,
                    std::format(
                        L"Received a compilation ended event without corresponding start event "
                        L"(object type {}, process {}, thread {}, time {}ns).",
                        (uint64_t)objectType,
                        processId,
                        ThreadId,
                        timestamp));
            }
        }

        void HandleApiObjectInserted(uint32_t processId, D3D12ObjectProcessor::ApiObjectInsertedEventArgs args, PipelineStateEventCallbacks* pipelineStateEventCallbacks)
        {
            auto entryIt = this->CompilationTimingEntries.find(args.ObjectType);
            if (entryIt != this->CompilationTimingEntries.end())
            {
                auto& entry = entryIt->second;
                entry.ObjectId = args.ObjectId;

                this->TryInsertCompilationTimingEntry(processId, args.ObjectType, pipelineStateEventCallbacks);
            }
        }

        // A cache statistics event comes in between the start and stop compilation events.
        // Since each thread can only compile one API object at a time, we can associate the statistics event with the current compilation timing entry.
        void CaptureCompilationCacheStatistics(D3D12CacheStatisticsArgs cacheStats)
        {
            assert(CompilationTimingEntries.size() == 1);
            if (CompilationTimingEntries.size() == 1)
            {
                auto& entry = CompilationTimingEntries.begin()->second;
                assert(entry.CacheStats.has_value() == false);
                entry.CacheStats = cacheStats;
            }
        }

    private:
        void TryInsertCompilationTimingEntry(uint32_t processId, ApiObjectType key, PipelineStateEventCallbacks* pipelineStateEventCallbacks, bool insertWithoutObjectId = false)
        {
            const auto entry = CompilationTimingEntries.find(key);
            if (entry != CompilationTimingEntries.end()
                && entry->second.StopTime.has_value()
                && (insertWithoutObjectId || entry->second.ObjectId.has_value()))
            {
                ThrowFailure(
                    pipelineStateEventCallbacks->OnPsoCompilation(
                        processId,
                        this->ThreadId,
                        entry->second.ObjectId.value(),
                        entry->second.StartTime,
                        entry->second.StopTime.value())
                );

                if (entry->second.CacheStats.has_value())
                {
                    const auto& cacheStats = entry->second.CacheStats.value();
                    (void)cacheStats;

                    // TODO There is no Storage API to store these properties.
                    // Ideally we'll have a way to attach arbitrary properties to an event.
                    // This will require modifying StoreThreadAPIObjectEvent to return an event id
                    // and adding a new API to attach properties to an event via eventId.
                    // 
                    // struct EventProperty
                    //{
                    //     const wchar_t* Name;
                    //     const wchar_t* Description;
                    //     uint32_t Value;
                    // };
                    // const std::vector <EventProperty> eventProperties = {
                    //     { L"NumRequiredLookups", L"Number of separate driver objects that make up the overall PSO/SO.",
                    //     cacheStats.numRequiredLookups }, { L"NumRequiredHitsInPSDB", L"Number of driver objects that were
                    //     precompiled (found in the PSDB).", cacheStats.numRequiredHitsInPSDB }, {
                    //     L"NumRequiredHitsInDynamicCache", L"Number of driver objects that were cached from a previous run of
                    //     the application.", cacheStats.numRequiredHitsInDynamicCache }, { L"NumIgnoredHits", L"Number of driver
                    //     objects that were cached but the driver ignored.", cacheStats.numIgnoredHits }, {
                    //     L"NumOptionalLookups", L"Additional cache lookup requests for optimized variants of driver objects
                    //     which may or may not be expected to exist.", cacheStats.numOptionalLookups }, {
                    //     L"NumOptionalHitsInPSDB", L"The precompiled shader portion of NumOptionalLookups.",
                    //     cacheStats.numOptionalHitsInPSDB }, { L"NumOptionalHitsInDynamicCache", L"The dynamic cache portion of
                    //     NumOptionalLookups.", cacheStats.numOptionalHitsInDynamicCache }, { L"NumDynamicCacheStores", L"Number
                    //     of driver objects the driver stored in the dynamic cache.", cacheStats.numDynamicCacheStores }
                    // };

                    // for (const auto& prop : eventProperties)
                    //{
                    //     ThrowFailure(populator->StoreThreadAPIObjectEventProperty(
                    //         eventId,
                    //         prop.Name,
                    //         prop.Value,
                    //         prop.Description
                    //     ));
                    // }
                }

                CompilationTimingEntries.erase(entry);
            }
        }
    };

    uint32_t m_processId;
    DiagnosticsSink* m_diagnosticsSink;
    std::map<uint32_t, PerThreadData> m_perThreadData;
    std::map<uint64_t, uint64_t> m_d3d12DeviceUmVersions;
    std::map<uint64_t, uint32_t> m_d3d12DeviceToKmDevices;

    AllocationTracker m_allocationTracker;

    D3D12ObjectProcessor m_apiObjectProcessor{ m_allocationTracker };

    // key is thunk allocation handle
    std::unordered_map<uint64_t, ResidencyOperation> m_deferredResidencyOperation;

    std::map<uint64_t, std::vector<std::pair<bool /*flag to indicate deleted*/, CommandListInstanceId>>> m_commandListToIds;
    std::map<CommandListInstanceId, CommandListInstance> m_commandListInstances;

    // Friendly names (ID3D12Object::SetName) keyed by D3D12 command list object pointer. Tracked per object so
    // that names persist across command list resets, which produce new CommandListInstance entries.
    std::map<uint64_t, std::wstring> m_commandListNames;

    std::vector<CommandQueueInstanceId> m_pendingCommandQueueInstances;

    std::map<uint64_t, CommandQueueInstanceId> m_commandQueueToId;
    std::map<CommandQueueInstanceId, CommandQueueInstance> m_commandQueueInstances;
    uint64_t m_commandQueueSequenceNumber;

    std::vector<CommandBufferSubmissionEntry> m_submissions;

    WinPixEventStore m_pixEventStore;

    struct DemotedAllocationEntry
    {
        uint64_t timestamp;
        uint64_t dxgAdapter;
        uint64_t allocationGlobalHandle;
        uint32_t preferredSegmentId;
        uint32_t actualSegmentId;
    };

    // dxgAdapter to entries
    std::unordered_map<uint64_t, std::vector<DemotedAllocationEntry>> m_deferredDemotedAllocationEntries;

    struct MigrateAllocationEntry
    {
        uint64_t startTime, stopTime;
        uint32_t status;
    };

    // It shouldn't be possible for more than one migration to be in-progress at the same time, but we'll
    // use a map to avoid any potential issues anyways.
    std::unordered_map<uint64_t, MigrateAllocationEntry> m_migrateAllocationEntries;

public:
    PerProcessData(uint32_t processId, PipelineStateEventCallbacks* pipelineStateEventCallbacks, DiagnosticsSink* diagnosticsSink)
        : m_processId(processId)
        , m_diagnosticsSink(diagnosticsSink)
        , m_commandQueueSequenceNumber(1)
    {
        m_apiObjectProcessor.OnApiObjectInserted = [=](auto args)
            {
                this->HandleApiObjectInserted(args, pipelineStateEventCallbacks);
            };
    }

    void AddAllocationInfos(
        uint64_t device,
        uint64_t object,
        uint32_t numVirtualAddressInfos,
        const VirtualAddressInfos* virtualAddressInfos,
        uint32_t numKMTInfos,
        const KMTInfos* kmtInfos,
        uint64_t timestamp,
        ResidencyEventCallbacks* residencyCallbacks,
        ApiObjectCallbacks* apiObjectCallbacks)
    {
        // We're only interested in AllocationInfos that have virtualAddressInfos
        if (numVirtualAddressInfos > 0)
        {
            AllocationTracker::Handle allocationInfoHandle;
            {
                AllocationInfo info{};
                info.device = device;
                info.object = object;
                info.virtualAddressInfos.reserve(numVirtualAddressInfos);
                info.virtualAddressInfos.insert(info.virtualAddressInfos.begin(), virtualAddressInfos, virtualAddressInfos + numVirtualAddressInfos);
                info.kmtInfos.reserve(numKMTInfos);
                info.kmtInfos.insert(info.kmtInfos.begin(), kmtInfos, kmtInfos + numKMTInfos);

                allocationInfoHandle = m_allocationTracker.AddAllocationInfo(timestamp, std::move(info));
            }

            m_apiObjectProcessor.AddAllocationInfo(allocationInfoHandle, timestamp, apiObjectCallbacks);

            // Process any deferred residence operations
            // Bug 40407969: AllocationInfo possibly missing kernel allocation information for large allocations (>4GB)
            if (numKMTInfos > 0)
            {
                uint32_t thunkAllocation = kmtInfos->kmAllocation;
                const auto residencyIt = m_deferredResidencyOperation.find(thunkAllocation);
                if (residencyIt != m_deferredResidencyOperation.end())
                {
                    const auto objectInfo = m_apiObjectProcessor.TryGetOwningApiObjectInfoByObjectAddress(object);
                    if (objectInfo.has_value())
                    {
                        auto& residencyData = residencyIt->second;
                        residencyData.ObjectId = objectInfo->Id;
                        residencyData.ObjectType = objectInfo->Type;
                        ThrowFailure(residencyCallbacks->OnResidencyOperation(&residencyData));

                        m_deferredResidencyOperation.erase(thunkAllocation);
                    }
                }
            }
        }
    }

    void AddPixEvent(uint32_t threadId, WinPixEventContextId contextId, WinPixEventEntry entry)
    {
        // If the context belongs to a command queue, route it to the queue specific store.
        auto it = m_commandQueueToId.find(contextId.Value);
        if (it != m_commandQueueToId.end())
        {
            m_pixEventStore.AddCommandQueuePixEvent(contextId, std::move(entry));
            return;
        }

        m_pixEventStore.AddPixEvent(threadId, contextId, std::move(entry));
    }

    void ProcessPendingSubmissions(TimestampConverter const* timestampConverter, AdapterTracker* adapters, DxgkContexts* dxgContexts, GpuTimingsCallbacks* gpuTimingsCallbacks, PixEventCallbacks* pixEventCallbacks, bool endOfTrace, DiagnosticsSink* diagnosticsSink)
    {
        // Iterate over all submissions currently tracked and ask each one to try and write out
        // its gpu event payload.  Submissions must be processed in order. If a submission is
        // not ready the loop exits early.
        //
        // There is no later retry at the end of the trace, so keep going past a
        // submission that never became ready rather than discarding the ones behind it.
        size_t processedSubmissions = 0;
        size_t unfinishedSubmissions = 0;
        for (auto& submission : m_submissions)
        {
            bool processed = submission.TryWriteGpuEvents(timestampConverter, adapters, dxgContexts, &m_pixEventStore, m_processId, &m_commandListInstances, &m_commandQueueInstances, &m_commandListToIds, gpuTimingsCallbacks, pixEventCallbacks, endOfTrace, diagnosticsSink);
            if (processed)
            {
                processedSubmissions++;
            }
            else if (endOfTrace)
            {
                unfinishedSubmissions++;
            }
            else
            {
                // The submission was not ready to process, so exit the loop.
                break;
            }
        }

        if (endOfTrace)
        {
            if (unfinishedSubmissions != 0)
            {
                diagnosticsSink->OnDiagnostic(
                    DiagnosticSeverity::Warning,
                    DiagnosticCode::CommandListExecutionsUnfinishedAtEndOfTrace,
                    L"Command list executions were still waiting on events that never arrived when the trace ended. Their GPU timing data is missing.");
            }

            m_submissions.clear();
            return;
        }

        // Erase all processed submissions in one go
        m_submissions.erase(m_submissions.begin(), m_submissions.begin() + processedSubmissions);
    }

    void ProcessDeferredEntries(ApiObjectCallbacks* callbacks)
    {
        m_apiObjectProcessor.ProcessDeferredEntries(callbacks);
    }

    void UpdateD3D12Device(uint64_t device, uint32_t kmDevice, uint64_t umDeviceVersion)
    {
        m_d3d12DeviceUmVersions[device] = umDeviceVersion;
        m_d3d12DeviceToKmDevices[device] = kmDevice;
    }

    void StartNewD3D12Device(
        uint64_t device,
        uint32_t featureLevel,
        uint32_t kmAdapter,
        uint64_t umAdapter,
        uint64_t umAdapterVersion,
        uint32_t kmDevice,
        uint64_t umDeviceVersion,
        uint64_t timestamp,
        uint32_t threadId,
        ApiObjectCallbacks* callbacks)
    {
        DeviceInfo deviceData{ static_cast<D3D_FEATURE_LEVEL>(featureLevel), kmAdapter, umAdapter, umAdapterVersion, kmDevice, umDeviceVersion };
        D3D12ObjectProcessor::DeviceEntry deviceEntry;
        deviceEntry.Timestamp = timestamp;
        deviceEntry.ObjectAddress = device;
        deviceEntry.ThreadId = threadId;
        deviceEntry.Data = deviceData;
        m_apiObjectProcessor.AddDevice(deviceEntry, callbacks);
    }

    void DestroyD3D12Device(
        uint64_t device,
        uint32_t kmDevice,
        uint64_t umDeviceVersion,
        uint64_t timestamp,
        ApiObjectCallbacks* callbacks)
    {
        m_apiObjectProcessor.DestroyApiObject(ApiObjectType::Device, device, timestamp, callbacks);
    }

    void UpdateName(uint64_t timestamp, uint64_t d3d12Object, std::string_view name, DxgkObjectCallbacks* dxgkObjectCallbacks, ApiObjectCallbacks* apiObjectCallbacks)
    {
        std::wstring_convert<std::codecvt_utf8<wchar_t>> converter;

        UpdateNameWide(timestamp, d3d12Object, converter.from_bytes(std::string(name)), dxgkObjectCallbacks, apiObjectCallbacks);
    }

    void UpdateNameWide(uint64_t timestamp, uint64_t d3d12Object, std::wstring_view name, DxgkObjectCallbacks* dxgkObjectCallbacks, ApiObjectCallbacks* apiObjectCallbacks)
    {
        auto it = m_commandQueueToId.find(d3d12Object);
        if (it != m_commandQueueToId.end())
        {
            auto& instance = m_commandQueueInstances[it->second];
            instance.Name = name;
            if (instance.ApiCommandQueueId.Value == 0)
            {
                // The command queue has not yet been written to the database so return
                // early as it will be written at a later time.
                return;
            }

            ThrowFailure(dxgkObjectCallbacks->OnApiCommandQueueName(instance.ApiCommandQueueId.Value, instance.Name.c_str()));
            return;
        }

        auto commandListIdsIt = m_commandListToIds.find(d3d12Object);
        if (commandListIdsIt != m_commandListToIds.end())
        {
            // Track the name per command list object so it survives resets, and apply it to any currently
            // tracked instances. The name is captured when an execution begins and written to storage when gpu work is completed.
            m_commandListNames[d3d12Object] = name;
            for (auto const& [deleted, commandListInstanceId] : commandListIdsIt->second)
            {
                auto instanceIt = m_commandListInstances.find(commandListInstanceId);
                if (instanceIt != m_commandListInstances.end())
                {
                    instanceIt->second.Name = name;
                }
            }
            return;
        }

        m_apiObjectProcessor.UpdateName(d3d12Object, name, timestamp, apiObjectCallbacks);
    }

    void MarkCommandListInstancesForDeletion(
        uint64_t commandList,
        PixEventCallbacks* pixEventCallbacks,
        ApiObjectCallbacks* apiObjectCallbacks)
    {
        // Grab and mark all command list instances associated with D3D12 commandList as deleted.
        // Any id that is already marked as delete is skipped.
        auto& commandListIds = m_commandListToIds[commandList];
        auto idIter = commandListIds.begin();
        while (idIter != commandListIds.end())
        {
            // First check if we have already marked the command list as deleted by looking at the
            // flag associated with just id. This is an optimization that helps us avoid
            // a map<> lookup for the actual command list instance.
            if (!idIter->first)
            {
                // The command list id indicates that the command list is not currently marked as deleted.
                // Find the command list instance in the tracking map so it can be properly marked.
                auto it = m_commandListInstances.find(idIter->second);
                if (it == m_commandListInstances.end())
                {
                    // The id is not tracking an instance so erase it now.
                    idIter = commandListIds.erase(idIter);
                }
                else
                {
                    // Mark both the command list id tracking data and the command list instance
                    idIter->first = true;
                    it->second.Deleted = true;

                    // If the command list is not in-flight delete it immediately.
                    // This ensures that we catch command lists that were created,
                    // but never executed.
                    if (it->second.CanBeDeleted())
                    {
                        it->second.FinalizeEventIds(pixEventCallbacks);
                        m_commandListInstances.erase(it);
                        idIter = commandListIds.erase(idIter);
                    }
                    else
                    {
                        ++idIter;
                    }
                }
            }
            else
            {
                ++idIter;
            }
        }
    }

    void StartNewCommandList(uint64_t commandList, uint64_t device, uint64_t sequenceNumber, bool resetUsed, PixEventCallbacks* pixEventCallbacks, ApiObjectCallbacks* apiObjectCallbacks)
    {
        // The same command list object (as identified by 'commandList') may
        // be reused.  Each time it is reset it is assigned a new
        // 'sequenceNumber'.  This is unique per device, and we call that a
        // "CommandListInstance".

        // An invalid sequence number value will be sent by the d3d runtime during a capture
        // state operation for command lists that have an unknown state.
        // These command lists can be ignored.
        constexpr uint64_t InvalidSequenceNumber = UINT64_MAX;
        if (sequenceNumber == InvalidSequenceNumber)
            return;

        auto pixEventContextId = WinPixEventContextId(commandList);

        if (resetUsed)
        {
            // Reset being called on a command list indicates that the command list
            // will be rebuilt and any instances previously built should be marked
            // as deleted as they can no longer be executed once completed.
            MarkCommandListInstancesForDeletion(
                commandList,
                pixEventCallbacks,
                apiObjectCallbacks);
        }

        auto commandListInstanceId = CommandListInstanceId{ device, sequenceNumber };
        auto& commandListInstance = m_commandListInstances[commandListInstanceId];
        commandListInstance.Id = commandListInstanceId;
        commandListInstance.PixEventContextId = pixEventContextId;

        // Carry forward any name previously assigned to this command list object so reused command lists keep
        // their friendly name across resets.
        auto nameIt = m_commandListNames.find(commandList);
        if (nameIt != m_commandListNames.end())
        {
            commandListInstance.Name = nameIt->second;
        }

        m_commandListToIds[commandList].push_back(std::make_pair(false, commandListInstanceId));
    }

    void DestroyCommandList(uint64_t commandList, uint64_t device, uint64_t sequenceNumber, PixEventCallbacks* pixEventCallbacks, ApiObjectCallbacks* apiObjectCallbacks)
    {
        // Mark all command list instances associated with D3D12 commandList as deleted.
        MarkCommandListInstancesForDeletion(
            commandList,
            pixEventCallbacks,
            apiObjectCallbacks);

        // Remove all tracking data if there are no more associated command list instances.
        auto commandListIdsIt = m_commandListToIds.find(commandList);
        if (commandListIdsIt != m_commandListToIds.end() && commandListIdsIt->second.empty())
        {
            m_commandListToIds.erase(commandList);
            m_commandListNames.erase(commandList);
        }
    }

    void StartExecuteCommandList(EVENT_RECORD* record, uint64_t timeStamp, uint64_t commandQueue, uint64_t commandList)
    {
        // Ignore executions for untracked commandlists because they will not
        // have any runtime marker data which makes associating them with
        // submissions and history buffer data impossible.
        if (m_commandListToIds[commandList].empty())
            return;

        // Ignore executions for untracked command queues too.  We do not have enough
        // information here to start tracking the command queue.  It is expected that
        // command queues not created during the capture will be reported during the
        // capture state operation which is always invoked when a capture begins.
        auto cmdQueue = m_commandQueueToId.find(commandQueue);
        if (cmdQueue == m_commandQueueToId.end())
            return;

        auto& perThreadData = GetPerThreadData(record);
        auto commandListInstanceId = m_commandListToIds[commandList].back().second;
        perThreadData.CurrentCommandListInstanceIds.clear();
        perThreadData.CurrentCommandListInstanceIds.push_back(commandListInstanceId);
        perThreadData.CurrentCommandQueueInstanceId = cmdQueue->second;
        auto& commandListInstance = m_commandListInstances[commandListInstanceId];

        // It has been observed in some capture sessions that an empty command list
        // gets executed.  If this is the case, we should not proceed as there isn't
        // any marker data to process.  By not recording a StartExecutionTime the command
        // list will appear as not used and will be deleted because it will never be considered
        // in-flight.
        // TODO: Investigate if this is really the case and if SDK Layers emits a warning
        //       about this.
        if (commandListInstance.RuntimeMarkers.empty())
            return;

        commandListInstance.ExecutionStarts.push({ perThreadData.ThreadId, timeStamp, commandListInstance.Name });
    }

    void StopExecuteCommandList(EVENT_RECORD* record)
    {
        auto& perThreadData = GetPerThreadData(record);
        perThreadData.CurrentCommandListInstanceIds.clear();
        perThreadData.CurrentCommandQueueInstanceId = {};
    }

    void StartExecuteCommandLists(EVENT_RECORD* record, uint64_t timeStamp, uint64_t commandQueue, PointerRange commandLists)
    {
        // Ignore executions for untracked command queues.  We do not have enough
        // information here to start tracking the command queue.  It is expected that
        // command queues not created during the capture will be reported during the
        // capture state operation which is always invoked when a capture begins.
        auto cmdQueue = m_commandQueueToId.find(commandQueue);
        if (cmdQueue == m_commandQueueToId.end())
            return;

        auto& perThreadData = GetPerThreadData(record);
        perThreadData.CurrentCommandListInstanceIds.clear();
        perThreadData.CurrentCommandQueueInstanceId = cmdQueue->second;

        for (auto commandList : commandLists)
        {
            // Only track command list executions for known commandlists because they should
            // contain runtime marker data which makes associating them with submissions and
            // history buffer data possible.
            if (!m_commandListToIds[commandList].empty())
            {
                auto commandListInstanceId = m_commandListToIds[commandList].back().second;
                auto& commandListInstance = m_commandListInstances[commandListInstanceId];
                perThreadData.CurrentCommandListInstanceIds.push_back(commandListInstanceId);

                // It has been observed in some capture sessions that a known command list
                // gets executed that doesn't contain marker data.  If this is the case,
                // we should not record a StartExecutionTime.
                // The command list will appear as not used and will be deleted because it will
                // never be considered in-flight.
                if (!commandListInstance.RuntimeMarkers.empty())
                {
                    commandListInstance.ExecutionStarts.push({ perThreadData.ThreadId, timeStamp, commandListInstance.Name });
                }
            }
        }
    }

    void StopExecuteCommandLists(EVENT_RECORD* record, PointerRange commandLists)
    {
        // TODO: Mark the submission that is being tracked for these command lists as ready to process

        auto& perThreadData = GetPerThreadData(record);
        perThreadData.CurrentCommandListInstanceIds.clear();
        perThreadData.CurrentCommandQueueInstanceId = {};
    }

    std::optional<AdapterId> GetAdapterIdFromD3D12Device(AdapterTracker* adapters, DxgkContexts* dxgContexts, uint64_t device)
    {
        auto it = m_d3d12DeviceToKmDevices.find(device);
        if (it != m_d3d12DeviceToKmDevices.end())
        {
            auto dxgAdapter = dxgContexts->GetDxgAdapterFromKmDevice(it->second);
            if (dxgAdapter.has_value())
                return adapters->GetAdapterId(*dxgAdapter);
        }
        return std::nullopt;
    }

    static const wchar_t* CommandListTypeToString(D3D12_COMMAND_LIST_TYPE type)
    {
        switch (type)
        {
        case D3D12_COMMAND_LIST_TYPE_DIRECT: return L"Direct";
        case D3D12_COMMAND_LIST_TYPE_BUNDLE: return L"Bundle";
        case D3D12_COMMAND_LIST_TYPE_COMPUTE: return L"Compute";
        case D3D12_COMMAND_LIST_TYPE_COPY: return L"Copy";
        case D3D12_COMMAND_LIST_TYPE_VIDEO_DECODE: return L"Video Decode";
        case D3D12_COMMAND_LIST_TYPE_VIDEO_PROCESS: return L"Video Process";
        case D3D12_COMMAND_LIST_TYPE_VIDEO_ENCODE: return L"Video Encode";
        case D3D12_COMMAND_LIST_TYPE_NONE: return L"Software";  // This is the same terminology the D3D runtime uses

        default:
            return L"Unknown";
        }
    }

    void DefinePendingCommandQueues(AdapterTracker* adapters, DxgkContexts* dxgContexts, DxgkObjectCallbacks* dxgkObjectCallbacks)
    {
        auto it = m_pendingCommandQueueInstances.begin();
        while (it != m_pendingCommandQueueInstances.end())
        {
            // The D3D device identifier is actually part of the command queue instance id.
            // A CommandQueueInstanceId is composed of a device + sequence number.
            // We can use that device along with the context information to obtain what
            // specific adapter it belongs to.
            uint64_t device = std::get<0>(*it);
            auto adapterId = GetAdapterIdFromD3D12Device(adapters, dxgContexts, device);
            if (adapterId.has_value())
            {
                auto& commandQueueInstance = m_commandQueueInstances[*it];
                uint64_t apiQueueId = 0;
                ThrowFailure(dxgkObjectCallbacks->OnApiCommandQueue(m_processId, adapterId->Value, CommandListTypeToString(commandQueueInstance.CommandListType), commandQueueInstance.CreationTimestamp, &apiQueueId));
                commandQueueInstance.ApiCommandQueueId = ApiCommandQueueId(apiQueueId);

                // If this command queue was is named ensure that the database gets it now.
                if (!commandQueueInstance.Name.empty())
                {
                    ThrowFailure(dxgkObjectCallbacks->OnApiCommandQueueName(commandQueueInstance.ApiCommandQueueId.Value, commandQueueInstance.Name.c_str()));
                }

                it = m_pendingCommandQueueInstances.erase(it);
            }
            else
            {
                // No adapter was found so leave it in the pending list to be processed when more adapter info
                // is reported.
                ++it;
            }
        }
    }

    void StartNewCommandQueue(uint64_t commandQueue, uint64_t device, D3D12_COMMAND_LIST_TYPE commandListType, uint64_t creationTimeNs, AdapterTracker* adapters, DxgkContexts* dxgContexts, DxgkObjectCallbacks* dxgkObjectCallbacks)
    {
        // The same command queue object (as identified by 'commandQueue') may
        // be reused.  Each time it is destroyed and created it is assigned a new
        // sequence number.  This is unique per device and we call that a
        // "CommandQueueInstance". The sequence number is generated by this
        // tracking system unlike command lists where the D3D12 runtime generates it.

        auto it = m_commandQueueToId.find(commandQueue);

        // If the existing command queue is destroyed then let's pretend that it doesn't
        // exist, so that we'll create a new one in its place below
        if (it == m_commandQueueToId.end() || m_commandQueueInstances[it->second].IsDestroyed)
        {
            auto commandQueueInstanceId = CommandQueueInstanceId{ device, m_commandQueueSequenceNumber++ };
            auto& commandQueueInstance = m_commandQueueInstances[commandQueueInstanceId];
            commandQueueInstance.Id = commandQueueInstanceId;
            commandQueueInstance.PixEventContextId = WinPixEventContextId(commandQueue);
            commandQueueInstance.CreationTimestamp = creationTimeNs;
            commandQueueInstance.CommandListType = commandListType;
            commandQueueInstance.IsDestroyed = false;
            m_commandQueueToId[commandQueue] = commandQueueInstanceId;

            // Attempt to find the adapter id for this command queue and define an api queue
            // in the database to represent it.
            auto adapterId = GetAdapterIdFromD3D12Device(adapters, dxgContexts, device);
            if (adapterId.has_value())
            {
                uint64_t id = 0;
                ThrowFailure(dxgkObjectCallbacks->OnApiCommandQueue(m_processId, adapterId->Value, CommandListTypeToString(commandQueueInstance.CommandListType), commandQueueInstance.CreationTimestamp, &id));
                commandQueueInstance.ApiCommandQueueId = ApiCommandQueueId(id);
            }
            else
            {
                // Adapter id was not found because the adapter data has not yet been populated.  This occurs
                // when a command queue is reported during capture state at the beginning of the capture.
                // The order of the D3D12 rundown reported events interleave with the DX kernel adapter events
                // so it is possible to recieve a command queue BEFORE the adapter that it belongs to.
                // For this case we store away the command queue instance id to be later defined in the database
                // when the adapter information is ready.
                m_pendingCommandQueueInstances.push_back(commandQueueInstanceId);
            }
        }
    }

    void DestroyCommandQueue(uint64_t commandQueue, uint64_t device, uint64_t destructionTimeNs, DxgkObjectCallbacks* dxgkObjectCallbacks)
    {
        auto it = m_commandQueueToId.find(commandQueue);
        if (it != m_commandQueueToId.end())
        {
            auto commandQueueInstanceId = it->second;
            auto& commandQueueInstance = m_commandQueueInstances[commandQueueInstanceId];
            ThrowFailure(dxgkObjectCallbacks->OnApiCommandQueueEnd(commandQueueInstance.ApiCommandQueueId.Value, destructionTimeNs));

            commandQueueInstance.IsDestroyed = true;
        }
    }

    void StartNewD3D12Heap(
        uint64_t device,
        uint64_t heap,
        uint64_t sizeInBytes,
        uint64_t alignment,
        uint32_t type,
        uint32_t cpuPageProperty,
        uint32_t memoryPoolPreference,
        uint32_t creationNodeMask,
        uint32_t visibleNodeMask,
        uint32_t flags,
        uint64_t conjoinedResource,
        uint64_t kmAllocation,
        uint64_t timestamp,
        uint32_t processId,
        uint32_t threadId,
        ApiObjectCallbacks* callbacks)
    {
        D3D12ObjectProcessor::HeapEntry heapEntry;
        heapEntry.Timestamp = timestamp;
        heapEntry.ObjectAddress = heap;
        heapEntry.ProcessId = processId;
        heapEntry.ThreadId = threadId;
        heapEntry.ConjoinedResource = conjoinedResource;

        heapEntry.Desc.SizeInBytes = sizeInBytes;
        heapEntry.Placement.GpuVirtualSize = sizeInBytes;
        heapEntry.Desc.Alignment = alignment;

        heapEntry.Desc.Properties.Type = static_cast<D3D12_HEAP_TYPE>(type);
        heapEntry.Desc.Properties.CPUPageProperty = static_cast<D3D12_CPU_PAGE_PROPERTY>(cpuPageProperty);
        heapEntry.Desc.Properties.MemoryPoolPreference = static_cast<D3D12_MEMORY_POOL>(memoryPoolPreference);
        heapEntry.Desc.Properties.CreationNodeMask = creationNodeMask;
        heapEntry.Desc.Properties.VisibleNodeMask = visibleNodeMask;
        heapEntry.Desc.Flags = static_cast<D3D12_HEAP_FLAGS>(flags);

        m_apiObjectProcessor.AddHeap(heapEntry, device, callbacks);
    }

    void DestroyD3D12Heap(
        uint64_t heap,
        uint64_t timestamp,
        ApiObjectCallbacks* callbacks)
    {
        m_apiObjectProcessor.DestroyApiObject(ApiObjectType::Heap, heap, timestamp, callbacks);
    }

    void StartNewD3D12Resource(
        uint64_t device,
        uint64_t resource,
        uint64_t umResource,
        uint32_t dimension,
        uint64_t width,
        uint32_t height,
        uint64_t depth,
        uint32_t mipLevels,
        uint32_t arraySize,
        uint32_t planeCount,
        uint32_t format,
        uint32_t sampleCount,
        uint32_t sampleQuality,
        uint32_t layout,
        uint32_t flags,
        uint32_t heapType,
        uint64_t heap,
        uint64_t immutableHeapOffset,
        uint64_t placedAlignment,
        uint64_t placedSize,
        uint32_t numTilesForResource,
        uint32_t numPackedMips,
        uint32_t numTilesForPackedMips,
        uint64_t immutableBuffer,
        uint64_t immutableBufferOffset,
        uint64_t timestamp,
        uint32_t processId,
        uint32_t threadId,
        ApiObjectCallbacks* callbacks)
    {

        D3D12ObjectProcessor::ResourceEntry entry;
        entry.Timestamp = timestamp;
        entry.ObjectAddress = resource;
        entry.ProcessId = processId;
        entry.ThreadId = threadId;
        entry.ResourceHeapType = (ResourceHeapType)heapType;

        entry.ResourceHeapData.HeapObjectAddress = heap;
        entry.ResourceHeapData.ImmutableHeapOffset = immutableHeapOffset;
        entry.ResourceHeapData.PlacedAlignment = placedAlignment;
        entry.ResourceHeapData.PlacedSize = placedSize;
        entry.ResourceHeapData.NumTilesForResource = numTilesForResource;
        entry.ResourceHeapData.NumPackedMips = numPackedMips;
        entry.ResourceHeapData.NumTilesForPackedMips = numTilesForPackedMips;

        D3D12_RESOURCE_DESC& desc = entry.Desc;
        desc.Dimension = static_cast<D3D12_RESOURCE_DIMENSION>(dimension);
        desc.Alignment = D3D12ObjectProcessor::ResourceEntry::InvalidValue; // determined from associated heap
        desc.Width = width;
        desc.Height = height;
        // If 3D texture then use depth, otherwise the resource may be an array
        // https://docs.microsoft.com/en-us/windows/win32/api/d3d12/ne-d3d12-d3d12_resource_dimension
        desc.DepthOrArraySize = static_cast<UINT16>(dimension == 4 ? depth : arraySize);
        desc.MipLevels = static_cast<UINT16>(mipLevels);
        desc.Format = static_cast<DXGI_FORMAT>(format);
        desc.SampleDesc = { sampleCount, sampleQuality };
        desc.Layout = static_cast<D3D12_TEXTURE_LAYOUT>(layout);
        desc.Flags = static_cast<D3D12_RESOURCE_FLAGS>(flags);

        entry.Placement.GpuVirtualAddress = D3D12ObjectProcessor::ResourceEntry::InvalidValue; // determined from associated heap
        entry.Placement.GpuVirtualSize = D3D12ObjectProcessor::ResourceEntry::InvalidValue; // determined from associated heap

        m_apiObjectProcessor.AddResource(entry, device, callbacks);
    }

    void DestroyD3D12Resource(
        uint64_t resource,
        uint64_t timestamp,
        ApiObjectCallbacks* callbacks)
    {
        m_apiObjectProcessor.DestroyApiObject(ApiObjectType::Resource, resource, timestamp, callbacks);
    }

    void StartNewD3D12GraphicsPipelineState(const D3D12GraphicsPipelineStateArgs& data, uint64_t timestamp, uint32_t processId, uint32_t threadId, ApiObjectCallbacks* callbacks)
    {
        D3D12ObjectProcessor::PipelineStateEntry entry;
        entry.Timestamp = timestamp;
        entry.ObjectAddress = data.object;
        entry.ProcessId = processId;
        entry.ThreadId = threadId;

        m_apiObjectProcessor.AddPipelineState(std::move(entry), data.device, callbacks);
    }

    void DestroyD3D12GraphicsPipelineState(const D3D12GraphicsPipelineStateArgs& data, uint64_t timestamp, ApiObjectCallbacks* callbacks)
    {
        m_apiObjectProcessor.DestroyApiObject(ApiObjectType::PipelineState, data.object, timestamp, callbacks);
    }

    void StartNewD3D12StateObject(const D3D12StateObjectArgs& data, uint64_t timestamp, uint32_t processId, uint32_t threadId, ApiObjectCallbacks* callbacks)
    {
        D3D12ObjectProcessor::StateObjectEntry entry;
        entry.Timestamp = timestamp;
        entry.ObjectAddress = data.object;
        entry.ProcessId = processId;
        entry.ThreadId = threadId;

        m_apiObjectProcessor.AddStateObject(std::move(entry), data.device, callbacks);
    }

    void DestroyD3D12StateObject(const D3D12StateObjectArgs& data, uint64_t timestamp, ApiObjectCallbacks* callbacks)
    {
        m_apiObjectProcessor.DestroyApiObject(ApiObjectType::StateObject, data.object, timestamp, callbacks);
    }

    void StartNewD3D12CommandAllocator(const D3D12CommandAllocatorArgs& data, uint64_t timestamp, uint32_t processId, uint32_t threadId, ApiObjectCallbacks* callbacks)
    {
        D3D12ObjectProcessor::CommandAllocatorEntry entry;
        entry.Timestamp = timestamp;
        entry.ObjectAddress = data.object;
        entry.ProcessId = processId;
        entry.ThreadId = threadId;
        entry.CommandListType = data.commandListType;

        m_apiObjectProcessor.AddCommandAllocator(std::move(entry), data.device, callbacks);
    }

    void DestroyD3D12CommandAllocator(const D3D12CommandAllocatorArgs& data, uint64_t timestamp, ApiObjectCallbacks* callbacks)
    {
        m_apiObjectProcessor.DestroyApiObject(ApiObjectType::CommandAllocator, data.object, timestamp, callbacks);
    }

    void StartNewD3D12DescriptorHeap(const D3D12DescriptorHeapArgs& data, uint64_t timestamp, uint32_t processId, uint32_t threadId, ApiObjectCallbacks* callbacks)
    {
        D3D12ObjectProcessor::DescriptorHeapEntry entry;
        entry.Timestamp = timestamp;
        entry.ObjectAddress = data.object;
        entry.ProcessId = processId;
        entry.ThreadId = threadId;
        entry.DescriptorHeapType = data.descriptorHeapType;
        entry.NumDescriptors = data.numDescriptors;
        entry.DescriptorHeapFlags = data.descriptorHeapFlags;
        entry.NodeMask = data.nodeMask;

        m_apiObjectProcessor.AddDescriptorHeap(std::move(entry), data.device, callbacks);
    }

    void DestroyD3D12DescriptorHeap(const D3D12DescriptorHeapArgs& data, uint64_t timestamp, ApiObjectCallbacks* callbacks)
    {
        m_apiObjectProcessor.DestroyApiObject(ApiObjectType::DescriptorHeap, data.object, timestamp, callbacks);
    }

    void StartNewD3D12MetaCommand(const D3D12MetaCommandArgs& data, uint64_t timestamp, uint32_t processId, uint32_t threadId, ApiObjectCallbacks* callbacks)
    {
        D3D12ObjectProcessor::MetaCommandEntry entry;
        entry.Timestamp = timestamp;
        entry.ObjectAddress = data.object;
        entry.ProcessId = processId;
        entry.ThreadId = threadId;
        entry.CommandId = data.commandId;

        m_apiObjectProcessor.AddMetaCommand(std::move(entry), data.device, callbacks);
    }

    void DestroyD3D12MetaCommand(const D3D12MetaCommandArgs& data, uint64_t timestamp, ApiObjectCallbacks* callbacks)
    {
        m_apiObjectProcessor.DestroyApiObject(ApiObjectType::MetaCommand, data.object, timestamp, callbacks);
    }

    void UpdateRuntimeMarkerData(
        const TimestampConverter* timestampConverter,
        GpuTimingsCallbacks* gpuTimingsCallbacks,
        uint32_t processId,
        uint64_t cpuFrequency,
        uint64_t firstApiSequenceNumber,
        uint64_t commandList,
        uint32_t cpuTimeHigh,
        uint8_t threadIdCount,
        uint32_t* threadIds,
        uint32_t dataSize,
        uint8_t* data)
    {
        // Corresponds to SBatchedMarker
        struct Marker
        {
            uint32_t QPCLow;
            uint8_t ThreadIDOrdinal;
            EMarkerOp MarkerAPI;
            uint16_t Reserved; // padding
        };

        auto* markers = reinterpret_cast<Marker*>(data);
        auto markerCount = dataSize / sizeof(Marker);

        // Ignore commandlists we have not tracked any reporting or creation info.
        if (m_commandListToIds[commandList].empty())
            return;

        auto commandListInstanceId = m_commandListToIds[commandList].back().second;
        auto& commandListInstance = m_commandListInstances[commandListInstanceId];

        commandListInstance.RuntimeMarkers.reserve(commandListInstance.RuntimeMarkers.size() + markerCount);

        size_t nextApiSequenceNumber = firstApiSequenceNumber;
        size_t runtimeMarkerIndex = 0;
        for (auto i = 0u; i < markerCount; ++i)
        {
            RuntimeMarkerDataEntry m{};
            m.ApiSequenceNumber = nextApiSequenceNumber++;
            m.MarkerOp = markers[i].MarkerAPI;
            m.ThreadId = threadIds[markers[i].ThreadIDOrdinal];

            ULARGE_INTEGER markerTimeStamp = {};
            markerTimeStamp.LowPart = markers[i].QPCLow;
            markerTimeStamp.HighPart = cpuTimeHigh;
            m.TimestampNs = timestampConverter->ConvertClockToTimeStamp(markerTimeStamp.QuadPart);

            // Always add the runtime marker to the commandlist's marker vector.
            // This ensures we can safely track markers to history buffer entries.
            // The marker's index is also tracked so it can be used as a key into
            // the associated WinPixEvent.
            runtimeMarkerIndex = commandListInstance.RuntimeMarkers.size();
            commandListInstance.RuntimeMarkers.push_back(m);

            // Track Begin/SetMarker/End as expected events as we expect there to be corresponding
            // events reported by the WinPixEventRuntime.
            if (m.MarkerOp == e_MOp_BeginEvent || m.MarkerOp == e_MOp_SetMarker || m.MarkerOp == e_MOp_EndEvent)
            {
                // BeginEvent,EndEvent and SetMarker are indicators that WinPixEventRuntime events
                // are expected to be paired with this command list.  A tracking vector keeps the
                // association of the runtime marker index location with the WinPixEventRuntime emitted event.
                commandListInstance.RuntimeMarkers.back().WinPixEventIndex = commandListInstance.ExpectedWinPixEvents.Size();
                commandListInstance.ExpectedWinPixEvents.Add(commandListInstance.RuntimeMarkers.back(), runtimeMarkerIndex);
            }

            uint64_t apiMarkerId = 0; // default to 0
            // Create api markers for all marker operations except BeginSubmission and EndSubmission.
            if (m.MarkerOp != e_MOp_BeginSubmission && m.MarkerOp != e_MOp_EndSubmission)
            {
                ThrowFailure(gpuTimingsCallbacks->OnApiMarker(processId, m.ThreadId, ToString(m.MarkerOp), m.TimestampNs, &apiMarkerId));
            }
            commandListInstance.RuntimeMarkers.back().ApiMarkerEventId = ApiMarkerEventId(apiMarkerId);

            // If the marker is an ExecuteBundle then we need to find the corresponding command list
            // that represents the bundle and copy all of its RuntimeMarkers to this command list's
            // runtime marker vector.
            // This has to be done because the HistoryBuffer data will also be sent expecting to be
            // matched up to the in-place expanded bundle command list markers.
            if (m.MarkerOp == e_MOp_ExecuteBundle)
            {
                // Bundles are a special case. After an e_MOp_ExecuteBundle operation, the next runtime marker should be reinterpreted
                // as a command list ID for the bundle.
                i++;
                uint64_t bundleCommandList = *reinterpret_cast<uint64_t const*>(&markers[i]);
                if (bundleCommandList != 0)
                {
                    auto it = m_commandListToIds.find(bundleCommandList);
                    if (it != m_commandListToIds.end())
                    {
                        auto const& bundleCommandListInstance = m_commandListInstances.at(it->second.back().second);
                        commandListInstance.RuntimeMarkers.reserve(commandListInstance.RuntimeMarkers.size() + bundleCommandListInstance.RuntimeMarkers.size());
                        for (auto& bundleMarker : bundleCommandListInstance.RuntimeMarkers)
                        {
                            RuntimeMarkerDataEntry b{};
                            b.ApiSequenceNumber = nextApiSequenceNumber++;
                            b.MarkerOp = bundleMarker.MarkerOp;
                            b.ThreadId = bundleMarker.ThreadId;
                            b.ApiMarkerEventId = bundleMarker.ApiMarkerEventId;
                            b.TimestampNs = bundleMarker.TimestampNs;
                            commandListInstance.RuntimeMarkers.push_back(b);
                        }
                    }
                    else
                    {
                        // A bundle was referenced, but it does not appear in our tracking data.
                        // This is most likely because of a race with the D3D12 runtime coming up to
                        // speed with just seeing an ETW provider being enabled.
                        // For this case, as a quick fix, we will mark this command list as 'no good'
                        // and avoid attempting to translate any submissions for it into gpu events.
                        commandListInstance.IgnoreSubmissions = true;
                    }
                }
            }
        }
    }

    void UpdateCommandBufferSubmission(
        EVENT_RECORD* record,
        DxgkContexts* dxgContexts,
        uint64_t commandQueue,
        uint32_t contextCount,
        uint32_t* contexts,
        uint32_t loopIteration,
        uint32_t submitCommandCbSequence,
        uint32_t firstApiSequenceNumberHigh,
        uint32_t completedApiSequenceNumberSize,
        uint32_t* completedApiSequenceNumbers,
        uint64_t commandList)
    {
        auto perThreadData = GetPerThreadData(record);

        // Ignore this submission if command list instance or command queue instance is not set.
        // This keeps submissions that we cannot fully process out of our tracked submissions list.
        auto commandQueueInstanceId = perThreadData.CurrentCommandQueueInstanceId;
        if (perThreadData.CurrentCommandListInstanceIds.empty() || commandQueueInstanceId == CommandQueueInstanceId{})
            return;

        CommandListInstanceId commandListInstanceId;
        if (commandList == 0)
        {
            assert(perThreadData.CurrentCommandListInstanceIds.size() == 1);
            commandListInstanceId = perThreadData.CurrentCommandListInstanceIds[0];
        }
        else
        {
            // Ignore this submission if command list instance is not tracked.
            // This keeps submissions that we cannot fully process out of our tracked submissions list.
            if (m_commandListToIds[commandList].empty())
                return;

            commandListInstanceId = m_commandListToIds[commandList].back().second;
        }

        for (auto i = 0u; i < contextCount; ++i)
        {
            HandleCommandBufferSubmission(
                dxgContexts,
                commandQueueInstanceId,
                commandListInstanceId,
                contexts[i],
                submitCommandCbSequence,
                loopIteration,
                firstApiSequenceNumberHigh,
                completedApiSequenceNumberSize,
                completedApiSequenceNumbers);
        }
    }

    void HandleCommandBufferSubmission(
        DxgkContexts* dxgContexts,
        CommandQueueInstanceId commandQueueInstanceId,
        CommandListInstanceId commandListInstanceId,
        uint32_t contextUmdHandle,
        uint32_t submitCommandCbSequence,
        uint32_t loopIteration,
        uint32_t firstApiSequenceNumberHigh,
        uint32_t completedApiSequenceNumberSize,
        uint32_t* completedApiSequenceNumbers)
    {
        auto context = dxgContexts->TryGetContext(m_processId, contextUmdHandle);
        if (!context)
            return;

        // Command buffer submissions are split up into multiple events,
        // that we have to stitch back together.  The runtime emits the
        // events before it actually submits the command buffer, so we don't
        // need to worry about getting history buffers for this submission
        // interleaved with these events.

        auto submission = std::ranges::find_if(m_submissions,
            [&](auto const& s)
            {
                return s.Context == context && s.SubmitSequence == submitCommandCbSequence;
            });

        if (submission == m_submissions.end())
        {
            // This is the first event for this submission
            CommandBufferSubmissionEntry s = {};
            s.Context = *context;
            s.SubmitSequence = submitCommandCbSequence;
            s.CommandQueueId = commandQueueInstanceId;
            s.LastLoopIteration = loopIteration;

            submission = m_submissions.insert(submission, std::move(s));
        }
        else
        {
            assert(submission->Context == context);
            assert(submission->SubmitSequence == submitCommandCbSequence);
            assert(submission->CommandQueueId == commandQueueInstanceId);

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
        if (submission->LastCommandListId != commandListInstanceId)
        {
            submission->CommandListIdAndApiSequenceNumbers.push_back({ commandListInstanceId });
        }
        submission->LastCommandListId = commandListInstanceId;
        submission->ExpectedHistoryBuffers += completedApiSequenceNumberSize;

        auto commandListInfo = &submission->CommandListIdAndApiSequenceNumbers.back();
        auto& apiSequenceNumbers = commandListInfo->ApiSequenceNumbers;
        apiSequenceNumbers.reserve(apiSequenceNumbers.size() + completedApiSequenceNumberSize);

        for (auto i = 0u; i < completedApiSequenceNumberSize; ++i)
        {
            // ObjectTrackingInfoSource assumes that
            // firstApiSequenceNumberHigh always == 0.  We'll make the same
            // assumption.  If it ever isn't 0 then all sorts of questions
            // need to be answered (eg why are there billions of commands in
            // the submission? what happens if the low part rolls over?)
            assert(firstApiSequenceNumberHigh == 0);

            apiSequenceNumbers.push_back(static_cast<uint64_t>(completedApiSequenceNumbers[i]));
        }
    }

    void UpdateHistoryBuffers(
        DxgkContexts* dxgContexts,
        uint64_t cpuClock,
        uint64_t context,
        uint32_t renderCbSequence,
        uint32_t precision,
        uint32_t historyBufferSize,
        uint8_t* historyBuffer)
    {
        auto submission = std::ranges::find_if(m_submissions,
            [&](auto const& s) { return s.Context == context && s.SubmitSequence == renderCbSequence; });

        if (submission == m_submissions.end())
            return;

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
            if (precision == 64)
                submission->TimestampMask = 0xFFFFFFFFFFFFFFFF;
            else
                submission->TimestampMask = (1ULL << precision) - 1;

            uint64_t* timestamps = reinterpret_cast<uint64_t*>(historyBuffer);
            auto numTimestamps = historyBufferSize / sizeof(uint64_t);
            auto numPairs = numTimestamps / 2;

            for (auto i = 0u; i < numPairs; ++i)
            {
                submission->HistoryBuffer.push_back({ timestamps[i * 2] & submission->TimestampMask, timestamps[i * 2 + 1] & submission->TimestampMask, cpuClock });
            }
        }
    }

    void ReportAdapterMemoryBudgetChange(
        MemoryCounterWriter* memoryCounterWriter,
        uint64_t newBudget,
        D3DKMT_MEMORY_SEGMENT_GROUP memorySegmentGroup,
        INT64 timestamp,
        PixCounterCallbacks* callbacks)
    {
        if (!memoryCounterWriter)
            return;

        switch (memorySegmentGroup)
        {
        case D3DKMT_MEMORY_SEGMENT_GROUP_LOCAL:
            memoryCounterWriter->ReportCounterValue(m_processId, MemoryCounterWriter::CounterType::LocalBudget, timestamp, newBudget, callbacks);
            break;
        case D3DKMT_MEMORY_SEGMENT_GROUP_NON_LOCAL:
            memoryCounterWriter->ReportCounterValue(m_processId, MemoryCounterWriter::CounterType::NonLocalBudget, timestamp, newBudget, callbacks);
            break;
        default:
            break;
        }
    }

    void ReportAdapterMemoryUsageChange(
        MemoryCounterWriter* memoryCounterWriter,
        uint64_t newUsage,
        D3DKMT_MEMORY_SEGMENT_GROUP memorySegmentGroup,
        INT64 timestamp,
        PixCounterCallbacks* callbacks)
    {
        if (!memoryCounterWriter)
            return;

        switch (memorySegmentGroup)
        {
        case D3DKMT_MEMORY_SEGMENT_GROUP_LOCAL:
            memoryCounterWriter->ReportCounterValue(m_processId, MemoryCounterWriter::CounterType::LocalUsage, timestamp, newUsage, callbacks);
            break;
        case D3DKMT_MEMORY_SEGMENT_GROUP_NON_LOCAL:
            memoryCounterWriter->ReportCounterValue(m_processId, MemoryCounterWriter::CounterType::NonLocalUsage, timestamp, newUsage, callbacks);
            break;
        default:
            break;
        }
    }

    void ReportAdapterMemoryCommitmentChange(
        MemoryCounterWriter* memoryCounterWriter,
        uint64_t newCommitment,
        D3DKMT_MEMORY_SEGMENT_GROUP memorySegmentGroup,
        INT64 timestamp,
        PixCounterCallbacks* callbacks)
    {
        if (!memoryCounterWriter)
            return;

        switch (memorySegmentGroup)
        {
        case D3DKMT_MEMORY_SEGMENT_GROUP_LOCAL:
            memoryCounterWriter->ReportCounterValue(m_processId, MemoryCounterWriter::CounterType::LocalCommitment, timestamp, newCommitment, callbacks);
            break;
        case D3DKMT_MEMORY_SEGMENT_GROUP_NON_LOCAL:
            memoryCounterWriter->ReportCounterValue(m_processId, MemoryCounterWriter::CounterType::NonLocalCommitment, timestamp, newCommitment, callbacks);
            break;
        default:
            break;
        }
    }

    void ReportAdapterMemoryDemotedCommitmentChange(
        MemoryCounterWriter* memoryCounterWriter,
        uint64_t newCommitment,
        uint8_t priorityClass,
        INT64 timestamp,
        PixCounterCallbacks* callbacks)
    {
        if (!memoryCounterWriter)
            return;

        memoryCounterWriter->ReportDemotedCommitmentCounterValue(m_processId, priorityClass, timestamp, newCommitment, callbacks);
    }

    void ReportAdapterPagingActivity(
        MemoryCounterWriter* memoryCounterWriter,
        uint64_t transferSize,
        DXGK_MEMORY_TRANSFER_DIRECTION transferDirection,
        INT64 timestamp,
        PixCounterCallbacks* callbacks)
    {
        if (!memoryCounterWriter)
            return;

        switch (transferDirection)
        {
        case DXGK_MEMORY_TRANSFER_LOCAL_TO_SYSTEM:
            memoryCounterWriter->ReportCounterValue(m_processId, MemoryCounterWriter::CounterType::LocalToNonLocalPagingActivity, timestamp, transferSize, callbacks);
            break;
        case DXGK_MEMORY_TRANSFER_SYSTEM_TO_LOCAL:
            memoryCounterWriter->ReportCounterValue(m_processId, MemoryCounterWriter::CounterType::NonLocalToLocalPagingActivity, timestamp, transferSize, callbacks);
            break;
        case DXGK_MEMORY_TRANSFER_LOCAL_TO_LOCAL: // Ignore this as it is not reported during paging
        default:
            break;
        }
    }

    void StartNewDeviceAllocation(
        uint64_t vidMmAlloc,
        uint64_t vidMmGlobalAlloc,
        uint64_t thunkAllocation,
        uint64_t timestamp)
    {
        AllocationTracker::DeviceAllocation allocation;
        allocation.Timestamp = timestamp;
        allocation.VidMmAlloc = vidMmAlloc;
        allocation.VidMmGlobalAlloc = vidMmGlobalAlloc;
        allocation.ThunkAllocation = thunkAllocation;

        m_allocationTracker.AddDeviceAllocation(std::move(allocation));
    }

    void DestroyDeviceAllocation(
        uint64_t vidMmAlloc,
        uint64_t vidMmGlobalAlloc)
    {
        m_allocationTracker.EraseDeviceAllocationByVidMm(vidMmAlloc, vidMmGlobalAlloc);
    }

    void StartNewAdapterAllocation(
        uint64_t dxgAdapter,
        uint32_t preferredSegment,
        uint64_t vidMmGlobalAlloc)
    {
        AllocationTracker::AdapterAllocation allocation;
        allocation.DxgAdapter = dxgAdapter;
        allocation.PreferredSegment = preferredSegment;
        allocation.VidMmGlobalAlloc = vidMmGlobalAlloc;

        m_allocationTracker.AddAdapterAllocation(std::move(allocation));
    }

    void DestroyAdapterAllocation(uint64_t vidMmGlobalAlloc)
    {
        m_allocationTracker.EraseAdapterAllocation(vidMmGlobalAlloc);
    }

    void ReportMakeResident(
        uint64_t vidMmAlloc,
        uint32_t residencyCount,
        uint64_t timestamp,
        ResidencyEventCallbacks* residencyCallbacks)
    {
        InsertResidenceOperation(vidMmAlloc, residencyCount, ResidencyOperationType::MakeResident, timestamp, residencyCallbacks);
    }

    void ReportEvict(
        uint64_t vidMmAlloc,
        uint32_t residencyCount,
        uint64_t timestamp,
        ResidencyEventCallbacks* residencyCallbacks)
    {
        InsertResidenceOperation(vidMmAlloc, residencyCount, ResidencyOperationType::Evict, timestamp, residencyCallbacks);
    }

    void InsertResidenceOperation(uint64_t vidMmAlloc, uint32_t residencyCount, ResidencyOperationType type, uint64_t timestamp, ResidencyEventCallbacks* residencyCallbacks)
    {
        ResidencyOperation data = { 0 };
        data.OperationType = type;
        data.Timestamp = timestamp;
        data.ResidencyCount = residencyCount;

        auto objectInfos = m_apiObjectProcessor.TryGetOwningApiObjectInfos(vidMmAlloc, false);
        if (objectInfos.empty())
        {
            // When creating committed resources the initial MakeResident event will come in before we receive
            // allocation information for the implied heap, but we should have the DeviceAllocation.
            const auto deviceAllocation = m_allocationTracker.TryFindDeviceAllocationByVidMmAlloc(vidMmAlloc);

            // If we can't find the DeviceAllocation then we'll just drop this event.
            // Note that this may happen if we attach to a process after the allocation is made but before the residence operation.
            if (deviceAllocation)
            {
                m_deferredResidencyOperation[deviceAllocation->ThunkAllocation] = data;
            }
        }
        else
        {
            const auto& objectInfo = objectInfos[0];
            data.ObjectId = objectInfo.Id;
            data.ObjectType = objectInfo.Type;
            ThrowFailure(residencyCallbacks->OnResidencyOperation(&data));
        }
    }

    void AddPagingOperation(uint64_t allocationGlobalHandle, DXGK_MEMORY_TRANSFER_DIRECTION transferDirection, uint64_t timestamp, ResidencyEventCallbacks* residencyCallbacks)
    {
        auto objectInfos = m_apiObjectProcessor.TryGetOwningApiObjectInfos(allocationGlobalHandle, true);
        // If we aren't tracking the object then we don't store anything.
        if (objectInfos.empty())
            return;

        for (const auto& objectInfo : objectInfos)
        {
            ResidencyOperation data = { 0 };
            data.ObjectId = objectInfo.Id;
            data.ObjectType = objectInfo.Type;
            data.OperationType = transferDirection == DXGK_MEMORY_TRANSFER_DIRECTION::DXGK_MEMORY_TRANSFER_SYSTEM_TO_LOCAL
                ? ResidencyOperationType::PageIn
                : ResidencyOperationType::PageOut;
            data.Timestamp = timestamp;
            ThrowFailure(residencyCallbacks->OnResidencyOperation(&data));
        }
    }

    void HandleAllocationSegmentInfo(
        AdapterTracker& adapters,
        uint64_t dxgAdapter,
        uint64_t allocationGlobalHandle,
        uint32_t segmentId,
        uint64_t timestamp,
        ResidencyEventCallbacks* residencyCallbacks)
    {
        // Notes:
        // Parameters are not guaranteed to be specific to this process (allocation segment info events originate from system process).
        // allocationGlobalHandle corresponds to vidMmGlobalAlloc (not dxgGlobalAlloc).
        // AdapterAllocations come from process the allocation is for (not the system process).

        // Runs before the adapterAllocation guard below - it only needs the adapter and segment id.
        UpdateResidentSegmentGroup(adapters, dxgAdapter, allocationGlobalHandle, segmentId, timestamp, residencyCallbacks);

        const auto* const adapterAllocation = m_allocationTracker.TryFindAdapterAllocation(dxgAdapter, allocationGlobalHandle);
        if (adapterAllocation == nullptr)
            return;

        // see https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dukmdt/ns-d3dukmdt-_d3dddi_segmentpreference
        D3DDDI_SEGMENTPREFERENCE segmentPreference;
        segmentPreference.Value = adapterAllocation->PreferredSegment;

        DemotedAllocationEntry entry{};
        entry.timestamp = timestamp;
        entry.dxgAdapter = dxgAdapter;
        entry.allocationGlobalHandle = allocationGlobalHandle;
        entry.preferredSegmentId = segmentPreference.SegmentId0;
        entry.actualSegmentId = segmentId;

        if (!TryInsertDemotedAllocation(adapters, entry, residencyCallbacks))
        {
            // Need to defer the insertion until we get the segment info
            m_deferredDemotedAllocationEntries[dxgAdapter].push_back(entry);
        }
    }

    // Records where an allocation now lives and, if it moved, notifies consumers.
    void UpdateResidentSegmentGroup(
        AdapterTracker& adapters,
        uint64_t dxgAdapter,
        uint64_t allocationGlobalHandle,
        uint32_t segmentId,
        uint64_t timestamp,
        ResidencyEventCallbacks* residencyCallbacks)
    {
        const auto segmentInfo = adapters.TryGetSegmentInfo(dxgAdapter, segmentId);
        if (!segmentInfo)
        {
            // Segment's memory type isn't known yet; a later paging event will re-evaluate.
            return;
        }

        // D3DKMT_MEMORY_SEGMENT_GROUP values line up with MemorySegmentGroup.
        const auto group = static_cast<MemorySegmentGroup>(segmentInfo->MemorySegmentGroup);

        const auto previousGroup = m_allocationTracker.TryGetResidentGroup(allocationGlobalHandle);
        if (previousGroup == group)
        {
            return;
        }

        m_allocationTracker.SetResidentGroup(allocationGlobalHandle, group);

        // Find the objects backed by this allocation so we can update their pool.
        const auto owningApiObjectInfos = m_apiObjectProcessor.TryGetOwningApiObjectInfos(allocationGlobalHandle, /*isAllocGlobal*/ true);
        if (owningApiObjectInfos.empty())
        {
            // Plenty of allocations don't map to a tracked object.
            return;
        }

        std::vector<AllocationSegmentGroupChange> changes;
        changes.reserve(owningApiObjectInfos.size());
        for (const auto& owningApiObjectInfo : owningApiObjectInfos)
        {
            AllocationSegmentGroupChange change{};
            change.ObjectId = owningApiObjectInfo.Id;
            change.Timestamp = (INT64)timestamp;
            change.ObjectType = owningApiObjectInfo.Type;
            change.SegmentGroup = group;
            changes.push_back(change);
        }

        ThrowFailure(residencyCallbacks->OnAllocationSegmentGroupChanges(changes.data(), (UINT32)changes.size()));
    }

    bool TryInsertDemotedAllocation(AdapterTracker& adapters, DemotedAllocationEntry entry, ResidencyEventCallbacks* residencyCallbacks)
    {
        const auto& preferredSegmentInfo = adapters.TryGetSegmentInfo(entry.dxgAdapter, entry.preferredSegmentId);
        const auto& actualSegmentInfo = adapters.TryGetSegmentInfo(entry.dxgAdapter, entry.actualSegmentId);

        if (preferredSegmentInfo && actualSegmentInfo)
        {
            if (preferredSegmentInfo->MemorySegmentGroup != actualSegmentInfo->MemorySegmentGroup)
            {
                // TODO should be GetOwningApiObjectInfos (ie all objects in this adapter allocation).
                // The isAllocGlobal flag should be removed and replaced with this new call.
                const auto owningApiObjectInfos = m_apiObjectProcessor.TryGetOwningApiObjectInfos(entry.allocationGlobalHandle, true);
                // not every adapter allocation will correspond to an api object
                std::vector<DemotedAllocation> demotedAllocations;
                if (!owningApiObjectInfos.empty())
                {
                    for (const auto& owningApiObjectInfo : owningApiObjectInfos)
                    {
                        DemotedAllocation data;
                        data.ObjectId = owningApiObjectInfo.Id;
                        data.Timestamp = entry.timestamp;

                        demotedAllocations.push_back(data);
                    }

                    ThrowFailure(residencyCallbacks->OnDemotedAllocations(demotedAllocations.data(), (UINT32)demotedAllocations.size()));
                }
            }

            return true;
        }

        return false;
    }

    void TryInsertDeferredDemotedAllocations(AdapterTracker& adapters, uint64_t dxgAdapter, ResidencyEventCallbacks* residencyCallbacks)
    {
        std::vector<size_t> completedIndices;
        for (size_t i = 0; i < m_deferredDemotedAllocationEntries[dxgAdapter].size(); i++)
        {
            auto& entry = m_deferredDemotedAllocationEntries[dxgAdapter][i];
            if (TryInsertDemotedAllocation(adapters, entry, residencyCallbacks))
            {
                completedIndices.push_back(i);
            }
        }

        // erase from the end first to avoid invalidating any of the indices
        for (size_t i = 1; i <= completedIndices.size(); i++)
        {
            size_t completedIndex = completedIndices[completedIndices.size() - i];
            m_deferredDemotedAllocationEntries[dxgAdapter].erase(m_deferredDemotedAllocationEntries[dxgAdapter].cbegin() + completedIndex);
        }
    }

    void StartMigrateAllocation(
        uint64_t allocationGlobalHandle,
        uint64_t timestamp)
    {
        const auto objectInfos = m_apiObjectProcessor.TryGetOwningApiObjectInfos(allocationGlobalHandle, true);
        // If we aren't tracking the object then we don't store anything.
        if (objectInfos.empty())
            return;

        // For some reason DXGK will fire the event twice when a migration fails. We only want the first occurrence.
        if (m_migrateAllocationEntries.count(allocationGlobalHandle) == 0)
        {
            MigrateAllocationEntry entry;
            entry.startTime = timestamp;
            m_migrateAllocationEntries[allocationGlobalHandle] = std::move(entry);
        }
    }

    void StopMigrateAllocation(
        uint64_t allocationGlobalHandle,
        uint32_t status,
        uint64_t timestamp,
        ResidencyEventCallbacks* residencyCallbacks)
    {
        const auto objectInfos = m_apiObjectProcessor.TryGetOwningApiObjectInfos(allocationGlobalHandle, true);
        // If we aren't tracking the object then we don't store anything.
        if (objectInfos.empty())
            return;

        // It's possible we receive this StopMigrateAllocation without receiving the corresponding Start.
        // If that's the case, just ignore it.
        if (m_migrateAllocationEntries.count(allocationGlobalHandle) > 0)
        {
            auto& entry = m_migrateAllocationEntries.at(allocationGlobalHandle);
            entry.status = status;
            entry.stopTime = timestamp;

            std::vector<AllocationMigration> allocationMigrations;
            
            for (const auto& objectInfo : objectInfos)
            {
                AllocationMigration data;
                data.ObjectId = objectInfo.Id;
                data.StartTime = entry.startTime;
                data.EndTime = entry.stopTime;
                data.Result = entry.status == 0 ? AllocationMigrationResult::Succeeded : AllocationMigrationResult::Failed;

                allocationMigrations.push_back(data);
            }

            ThrowFailure(residencyCallbacks->OnAllocationMigrations(allocationMigrations.data(), (UINT32)allocationMigrations.size()));

            m_migrateAllocationEntries.erase(allocationGlobalHandle);
        }
    }

    void StartCompilationEvent(ApiObjectType objectType, const EVENT_RECORD* record, uint64_t timestamp)
    {
        auto& perThreadData = GetPerThreadData(record);
        perThreadData.StartCompilationEvent(timestamp, m_processId, objectType, m_diagnosticsSink);
    }

    void StopCompilationEvent(ApiObjectType objectType, const EVENT_RECORD* record, uint64_t timestamp, PipelineStateEventCallbacks* pipelineStateEventCallbacks)
    {
        auto& perThreadData = GetPerThreadData(record);
        perThreadData.StopCompilationEvent(timestamp, m_processId, objectType, pipelineStateEventCallbacks, m_diagnosticsSink);
    }

    void CaptureCompilationCacheStatistics(const EVENT_RECORD* record, D3D12CacheStatisticsArgs args)
    {
        auto& perThreadData = GetPerThreadData(record);
        perThreadData.CaptureCompilationCacheStatistics(std::move(args));
    }

private:
    PerThreadData& GetPerThreadData(const EVENT_RECORD* record)
    {
        auto& perThreadData = m_perThreadData[record->EventHeader.ThreadId];
        perThreadData.ThreadId = record->EventHeader.ThreadId;
        return perThreadData;
    }

    void HandleApiObjectInserted(D3D12ObjectProcessor::ApiObjectInsertedEventArgs args, PipelineStateEventCallbacks* pipelineStateEventCallbacks)
    {
        auto perThreadDataIt = m_perThreadData.find(args.ThreadId);
        if (perThreadDataIt != m_perThreadData.end())
        {
            auto& perThreadData = perThreadDataIt->second;

            perThreadData.HandleApiObjectInserted(m_processId, args, pipelineStateEventCallbacks);
        }
    }
};

} // namespace DirectX::Etw