// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#pragma once

namespace DirectX::Etw
{

class MemoryCounterWriter
{
public:
    MemoryCounterWriter(AdapterId AdapterId, PixCounterCallbacks* callbacks)
    {
        // Add a GPU Memory counter group for this adapter for memory counters like commit totals etc.
        std::wstringstream groupName;
        groupName << L"GPU Memory (Adapter #" << AdapterId.Value << L")";
        ThrowFailure(callbacks->OnPixCounterGroup(0, groupName.str().c_str(), L"", &m_memoryCountersGroupId));
    }

    enum CounterType
    {
        LocalBudget = 0,
        LocalCommitment,
        LocalUsage,
        NonLocalBudget,
        NonLocalCommitment,
        NonLocalUsage,
        DemotedMinPriority,
        DemotedLowPriority,
        DemotedNormalPriority,
        DemotedHighPriority,
        DemotedMaxPriority,
        LocalToNonLocalPagingActivity,
        NonLocalToLocalPagingActivity,
        TotalMemoryCounterTypes
    };

    void ReportCounterValue(
        uint32_t processId,
        CounterType counterType,
        int64_t timestamp,
        uint64_t value,
        PixCounterCallbacks* callbacks)
    {
        if (m_memoryCounterDescs[counterType].CounterId == 0)
        {
            StoreNewCounter(counterType, processId, callbacks);
        }

        m_memoryCounterDescs[counterType].LastValueReported = value;
        StoreCounterValueInMegaBytes(m_memoryCounterDescs[counterType].CounterId, timestamp, value, callbacks);
    }

    void ReportDemotedCommitmentCounterValue(
        uint32_t processId,
        uint8_t priorityClass,
        int64_t timestamp,
        uint64_t value,
        PixCounterCallbacks* callbacks)
    {
        CounterType priorityClasses[] = {
            DemotedMinPriority,
            DemotedLowPriority,
            DemotedNormalPriority,
            DemotedHighPriority,
            DemotedMaxPriority
        };
        ReportCounterValue(processId, priorityClasses[priorityClass], timestamp, value, callbacks);
    }

    void ReportLastCounterValues(int64_t timestamp, PixCounterCallbacks* callbacks)
    {
        std::vector<PixCounterCallbacks::CounterDataPoint> dataPoints;
        for (auto& desc : m_memoryCounterDescs)
        {
            if (desc.CounterId != 0)
            {
                dataPoints.push_back({ desc.CounterId, timestamp, ToMegaBytes(desc.LastValueReported) });
            }
        }

        if (!dataPoints.empty())
        {
            ThrowFailure(callbacks->OnPixCounterData(dataPoints.data(), (UINT32)dataPoints.size()));
        }
    }

    void UpdateGroupName(const wchar_t* adapterName, PixCounterCallbacks* callbacks)
    {
        //
        // TODO: Uncomment the following lines to update the GPU Memory group with the real adapter
        //       name when the populator interface is updated.
        //

        // Update the GPU Memory counter group name for this adapter with the
        // real adapter name.
        //std::wstringstream groupName;
        //groupName << L"GPU Memory (" << adapterName << L")";
        //ThrowFailure(populator->UpdatePixCounterGroup(m_memoryCountersGroupId, groupName.str().c_str()));
    }

private:
    uint64_t m_memoryCountersGroupId = 0;

    struct CounterDesc
    {
        PCWSTR Name;
        PCWSTR Description;
        PCWSTR Units;
        uint64_t CounterId;
        uint64_t LastValueReported;
    };

    CounterDesc m_memoryCounterDescs[TotalMemoryCounterTypes] = {
        {L"Local Budget",         L"OS-provided budget for memory placed in the local segment on this adapter.", L"MB", 0, 0},
        {L"Local Resident",       L"Amount of memory that is actually placed in local memory segments.", L"MB", 0, 0},
        {L"Local Usage",          L"Amount of memory that the application asked to be placed in local memory segments.", L"MB", 0, 0},
        {L"Non-Local Budget",     L"OS-provided budget for memory placed in the non-local segment on this adapter.", L"MB", 0, 0},
        {L"Non-Local Resident",   L"Amount of memory that is actually placed in non-local memory segments.", L"MB", 0, 0},
        {L"Non-Local Usage",      L"Amount of memory that the application asked to be placed in non-local memory segments.", L"MB", 0, 0},
        {L"Minimum Priority",     L"Amount of minimum priority memory that was preferred to be placed in local memory, but could not be placed there.", L"MB", 0, 0},
        {L"Low Priority",         L"Amount of low priority memory that was preferred to be placed in local memory, but could not be placed there.", L"MB", 0, 0},
        {L"Normal Priority",      L"Amount of normal priority memory that was preferred to be placed in local memory, but could not be placed there.", L"MB", 0, 0},
        {L"High Priority",        L"Amount of high priority memory that was preferred to be placed in local memory, but could not be placed there.", L"MB", 0, 0},
        {L"Maximum Priority",     L"Amount of maximum priority memory that was preferred to be placed in local memory, but could not be placed there.", L"MB", 0, 0},
        {L"Local to Non-Local Paging", L"Amount of memory that was moved from local memory into non-local memory due to GPU memory pressure.", L"MB", 0, 0},
        {L"Non-Local to Local Paging", L"Amount of memory that was moved from non-local memory to local memory due to GPU memory pressure.", L"MB", 0, 0}
    };

    // Some counters are linked to each other to represent a budget and usage pair.
    // When both counters are reported, the budget counter is linked to the usage counter.
    // See TryStoreCounterBudget.
    struct CounterBudgetLink
    {
        CounterType UsageCounterType;
        CounterType BudgetCounterType;
    };

    const CounterBudgetLink m_budgetLinks[2] = {
        { LocalUsage, LocalBudget },
        { NonLocalUsage, NonLocalBudget },
    };

    void StoreNewCounter(CounterType counterType, uint32_t processId, PixCounterCallbacks* callbacks)
    {
        assert(m_memoryCounterDescs[counterType].CounterId == 0);

        // Note that Infinity is chosen as the max value
        // because the graph rendering in PIX recognizes it
        // as a constant indicating that auto scaling to the
        // maximum reported value is required.
        ThrowFailure(callbacks->OnPixCounterInfo(
            m_memoryCountersGroupId,
            processId,
            m_memoryCounterDescs[counterType].Name,
            m_memoryCounterDescs[counterType].Description,
            m_memoryCounterDescs[counterType].Units,
            CounterFlags::None,
            0,
            std::numeric_limits<double>::infinity(),
            &m_memoryCounterDescs[counterType].CounterId
        ));

        // If applicable, attempt to link budget and usage counters.
        TryStoreCounterBudget(counterType, callbacks);
    }

    void TryStoreCounterBudget(CounterType counterType, PixCounterCallbacks* callbacks)
    {
        assert(m_memoryCounterDescs[counterType].CounterId != 0);

        for (const auto link : m_budgetLinks)
        {
            std::optional<CounterType> linkedCounterType;
            if (counterType == link.UsageCounterType)
            {
                linkedCounterType = link.BudgetCounterType;
            }
            else if (counterType == link.BudgetCounterType)
            {
                linkedCounterType = link.UsageCounterType;
            }

            if (linkedCounterType.has_value())
            {
                if (m_memoryCounterDescs[linkedCounterType.value()].CounterId != 0)
                {
                    ThrowFailure(callbacks->OnPixCounterBudget(
                        m_memoryCounterDescs[link.UsageCounterType].CounterId,
                        m_memoryCounterDescs[link.BudgetCounterType].CounterId,
                        CounterBudgetType::Maximum,
                        CounterBudgetSource::System
                    ));
                }

                // We assume there's only one valid pair so no need to iterate over the rest
                // of the links.
                break;
            }
        }
    }

    double ToMegaBytes(uint64_t valueInBytes)
    {
        return ((double)valueInBytes / 1000000.0f); /*MB conversion*/
    }

    void StoreCounterValueInMegaBytes(uint64_t counterId, int64_t timestamp, uint64_t valueInBytes, PixCounterCallbacks* callbacks)
    {
        if (counterId == 0)
            return;
        PixCounterCallbacks::CounterDataPoint dataPoint = {counterId, timestamp, ToMegaBytes(valueInBytes)};
        ThrowFailure(callbacks->OnPixCounterData(&dataPoint, 1));
    }
};

} // namespace DirectX::Etw