// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#pragma once

#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <tuple>
#include <vector>

namespace DirectX::Etw
{

enum class GpuPacketSource
{
    HardwareQueue,
    LegacyContext,
};

struct GpuPacketOwner
{
    uint32_t ProcessId;
    uint64_t Adapter;
    uint32_t NodeOrdinal;
    uint32_t EngineAffinity;
};

// An upper bound on time spent on the engine, not a measurement of it: the span
// starts when the packet is handed to the driver, and hardware-scheduled
// completions arrive on a batched DPC.
struct GpuPacketSpan
{
    GpuPacketSource Source;

    uint32_t ProcessId;
    uint64_t Adapter;
    uint32_t NodeOrdinal;
    uint32_t EngineAffinity;

    uint64_t StartQpc;
    uint64_t EndQpc;

    // False when the owning context was never seen, leaving the owner fields
    // meaningless. Paging queues and the scheduler's system contexts always
    // land here.
    bool Attributed;
};

class GpuPacketTracker
{
    struct OpenPacket
    {
        GpuPacketOwner Owner;
        uint64_t StartQpc;
        bool Attributed;
        bool Discarded;
    };

    // Source, handle and fence value - all the events give us to pair on. That is
    // not unique for hardware-scheduled queues, which run two independent progress
    // fence timelines (kernel-mode submissions like presents and paging, and
    // user-mode ones) without saying which a packet used. Keeping a list per key
    // and retiring it in order means a collision costs accuracy on one packet
    // instead of losing the other one entirely.
    using Key = std::tuple<GpuPacketSource, uint64_t, uint64_t>;

    std::map<Key, std::vector<OpenPacket>> m_openPackets;
    size_t m_openPacketCount = 0;

    uint64_t m_lastEventQpc = 0;
    bool m_sawAnySubmission = false;

    // Dropped completions, device resets and contexts torn down mid-flight strand
    // entries forever. Set well above what a healthy machine keeps in flight, so
    // hitting it means something has already gone wrong.
    static constexpr size_t MaxOpenPackets = 64 * 1024;

    static GpuPacketSpan MakeSpan(GpuPacketSource source, OpenPacket const& open, uint64_t endQpc)
    {
        GpuPacketSpan span{};
        span.Source = source;
        span.ProcessId = open.Owner.ProcessId;
        span.Adapter = open.Owner.Adapter;
        span.NodeOrdinal = open.Owner.NodeOrdinal;
        span.EngineAffinity = open.Owner.EngineAffinity;
        span.StartQpc = open.StartQpc;
        span.EndQpc = endQpc;
        span.Attributed = open.Attributed;
        return span;
    }

    void Push(Key const& key, OpenPacket const& packet)
    {
        m_sawAnySubmission = true;

        if (m_openPacketCount >= MaxOpenPackets)
        {
            EvictOldest();
        }

        m_openPackets[key].push_back(packet);
        ++m_openPacketCount;
    }

    // Drops the older half at once, so the sweep runs rarely rather than on every
    // packet once we are sitting at the ceiling.
    void EvictOldest()
    {
        std::vector<uint64_t> startTimes;
        startTimes.reserve(m_openPacketCount);

        for (auto const& [key, openPackets] : m_openPackets)
        {
            for (auto const& open : openPackets)
            {
                startTimes.push_back(open.StartQpc);
            }
        }

        auto const median = startTimes.begin() + startTimes.size() / 2;
        std::ranges::nth_element(startTimes, median);
        uint64_t const cutoff = *median;

        for (auto it = m_openPackets.begin(); it != m_openPackets.end(); )
        {
            m_openPacketCount -= std::erase_if(
                it->second,
                [cutoff](OpenPacket const& open) { return open.StartQpc < cutoff; });

            it = it->second.empty() ? m_openPackets.erase(it) : std::next(it);
        }
    }

public:
    // Call for every packet event, even ones that open nothing, so the
    // end-of-trace timestamp covers the whole trace.
    void NoteEventTimestamp(uint64_t eventQpc)
    {
        m_lastEventQpc = (std::max)(m_lastEventQpc, eventQpc);
    }

    // A nullopt owner records the packet as unattributed - it still occupies the
    // GPU, we just cannot say for whom.
    void Open(
        GpuPacketSource source,
        uint64_t handle,
        uint64_t fenceValue,
        uint64_t startQpc,
        std::optional<GpuPacketOwner> const& owner)
    {
        OpenPacket packet{};
        packet.StartQpc = startQpc;

        if (owner.has_value())
        {
            packet.Attributed = true;
            packet.Owner = *owner;
        }

        Push(Key{ source, handle, fenceValue }, packet);
    }

    // A failed submission never runs, but the scheduler still simulates its
    // completion. Queue a placeholder so that completion gets swallowed instead of
    // retiring a real packet.
    void ExpectOrphanCompletion(GpuPacketSource source, uint64_t handle, uint64_t fenceValue, uint64_t startQpc)
    {
        OpenPacket packet{};
        packet.StartQpc = startQpc;
        packet.Discarded = true;

        Push(Key{ source, handle, fenceValue }, packet);
    }

    // nullopt when the completion matched nothing live: the packet was already in
    // flight before the trace started, or the completion belongs to a failed
    // submission, or the scheduler simulated a fence during rundown or after a
    // device reset.
    std::optional<GpuPacketSpan> Close(
        GpuPacketSource source,
        uint64_t handle,
        uint64_t fenceValue,
        uint64_t endQpc)
    {
        auto it = m_openPackets.find(Key{ source, handle, fenceValue });
        if (it == m_openPackets.end())
        {
            return std::nullopt;
        }

        auto& openPackets = it->second;
        OpenPacket const open = openPackets.front();
        openPackets.erase(openPackets.begin());
        --m_openPacketCount;

        if (openPackets.empty())
        {
            m_openPackets.erase(it);
        }

        if (open.Discarded || endQpc <= open.StartQpc)
        {
            return std::nullopt;
        }

        return MakeSpan(source, open, endQpc);
    }

    // Packets still open when the trace stopped never produced a completion.
    // Dropping them would hide the case that matters most - one long packet
    // spanning an entire measurement - so close them at endQpc instead.
    std::vector<GpuPacketSpan> CollectUnfinished(uint64_t endQpc) const
    {
        std::vector<GpuPacketSpan> spans;

        for (auto const& [key, openPackets] : m_openPackets)
        {
            for (auto const& open : openPackets)
            {
                if (!open.Discarded && endQpc > open.StartQpc)
                {
                    spans.push_back(MakeSpan(std::get<0>(key), open, endQpc));
                }
            }
        }

        return spans;
    }

    // Lets a caller tell an idle GPU apart from a trace that never carried packet
    // events at all.
    bool SawAnySubmission() const
    {
        return m_sawAnySubmission;
    }

    uint64_t LastEventQpc() const
    {
        return m_lastEventQpc;
    }
};

} // namespace DirectX::Etw
