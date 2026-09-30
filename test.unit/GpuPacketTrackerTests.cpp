// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "pch.h"

#include <DxTimingCaptureLibrary/GpuPacketTracker.h>

using namespace DirectX::Etw;

namespace
{
    constexpr GpuPacketOwner Owner(uint32_t processId)
    {
        return GpuPacketOwner{ processId, 0xAAAA, 2, 0b10 };
    }
}

TEST(GpuPacketTrackerTests, PairsSubmitWithCompletion)
{
    GpuPacketTracker tracker;
    tracker.Open(GpuPacketSource::HardwareQueue, 0x1000, 7, 100, Owner(1234));

    auto packet = tracker.Close(GpuPacketSource::HardwareQueue, 0x1000, 7, 250);

    ASSERT_TRUE(packet.has_value());
    EXPECT_TRUE(packet->Attributed);
    EXPECT_EQ(1234u, packet->ProcessId);
    EXPECT_EQ(0xAAAAu, packet->Adapter);
    EXPECT_EQ(2u, packet->NodeOrdinal);
    EXPECT_EQ(0b10u, packet->EngineAffinity);
    EXPECT_EQ(100u, packet->StartQpc);
    EXPECT_EQ(250u, packet->EndQpc);
}

TEST(GpuPacketTrackerTests, PacketWithNoOwnerIsUnattributed)
{
    GpuPacketTracker tracker;
    tracker.Open(GpuPacketSource::LegacyContext, 0x2000, 1, 100, std::nullopt);

    auto packet = tracker.Close(GpuPacketSource::LegacyContext, 0x2000, 1, 200);

    ASSERT_TRUE(packet.has_value());
    EXPECT_FALSE(packet->Attributed);
}

TEST(GpuPacketTrackerTests, SourceIsPartOfTheKey)
{
    GpuPacketTracker tracker;
    tracker.Open(GpuPacketSource::HardwareQueue, 0x3000, 5, 100, Owner(1));

    // Same handle and fence value, other scheduler - must not retire the packet above.
    EXPECT_FALSE(tracker.Close(GpuPacketSource::LegacyContext, 0x3000, 5, 200).has_value());
    EXPECT_TRUE(tracker.Close(GpuPacketSource::HardwareQueue, 0x3000, 5, 200).has_value());
}

TEST(GpuPacketTrackerTests, CollidingKeysRetireInOrder)
{
    GpuPacketTracker tracker;
    tracker.Open(GpuPacketSource::HardwareQueue, 0x4000, 9, 100, Owner(11));
    tracker.Open(GpuPacketSource::HardwareQueue, 0x4000, 9, 150, Owner(22));

    auto oldest = tracker.Close(GpuPacketSource::HardwareQueue, 0x4000, 9, 200);
    ASSERT_TRUE(oldest.has_value());
    EXPECT_EQ(11u, oldest->ProcessId);

    auto newest = tracker.Close(GpuPacketSource::HardwareQueue, 0x4000, 9, 300);
    ASSERT_TRUE(newest.has_value());
    EXPECT_EQ(22u, newest->ProcessId);
}

TEST(GpuPacketTrackerTests, CompletionWithNoOpenPacketIsIgnored)
{
    GpuPacketTracker tracker;

    EXPECT_FALSE(tracker.Close(GpuPacketSource::HardwareQueue, 0x5000, 1, 200).has_value());
}

TEST(GpuPacketTrackerTests, OrphanCompletionDoesNotRetireALivePacket)
{
    GpuPacketTracker tracker;

    // A failed submit, then a real one that happens to reuse the key.
    tracker.ExpectOrphanCompletion(GpuPacketSource::HardwareQueue, 0x6000, 4, 50);
    tracker.Open(GpuPacketSource::HardwareQueue, 0x6000, 4, 100, Owner(33));

    EXPECT_FALSE(tracker.Close(GpuPacketSource::HardwareQueue, 0x6000, 4, 200).has_value());

    auto packet = tracker.Close(GpuPacketSource::HardwareQueue, 0x6000, 4, 300);
    ASSERT_TRUE(packet.has_value());
    EXPECT_EQ(33u, packet->ProcessId);
}

TEST(GpuPacketTrackerTests, OrphanQueuedAfterALivePacketDoesNotStealItsCompletion)
{
    GpuPacketTracker tracker;

    tracker.Open(GpuPacketSource::HardwareQueue, 0x6100, 4, 100, Owner(44));
    tracker.ExpectOrphanCompletion(GpuPacketSource::HardwareQueue, 0x6100, 4, 150);

    auto packet = tracker.Close(GpuPacketSource::HardwareQueue, 0x6100, 4, 200);
    ASSERT_TRUE(packet.has_value());
    EXPECT_EQ(44u, packet->ProcessId);
    EXPECT_EQ(200u, packet->EndQpc);

    EXPECT_FALSE(tracker.Close(GpuPacketSource::HardwareQueue, 0x6100, 4, 300).has_value());
}

TEST(GpuPacketTrackerTests, OrphansAreNotReportedAsUnfinished)
{
    GpuPacketTracker tracker;
    tracker.ExpectOrphanCompletion(GpuPacketSource::HardwareQueue, 0x6200, 1, 100);

    EXPECT_TRUE(tracker.CollectUnfinished(500).empty());
}

TEST(GpuPacketTrackerTests, CompletionAtOrBeforeSubmitIsDropped)
{
    GpuPacketTracker tracker;
    tracker.Open(GpuPacketSource::HardwareQueue, 0x7000, 1, 500, Owner(55));

    EXPECT_FALSE(tracker.Close(GpuPacketSource::HardwareQueue, 0x7000, 1, 500).has_value());
}

TEST(GpuPacketTrackerTests, UnfinishedPacketsAreClosedAtTheEndOfTheTrace)
{
    GpuPacketTracker tracker;
    tracker.Open(GpuPacketSource::HardwareQueue, 0x8000, 1, 100, Owner(66));
    tracker.Open(GpuPacketSource::LegacyContext, 0x8001, 2, 900, Owner(66));

    auto packets = tracker.CollectUnfinished(800);

    // The second packet started after the trace ended, so it has no span.
    ASSERT_EQ(1u, packets.size());
    EXPECT_EQ(66u, packets[0].ProcessId);
    EXPECT_EQ(100u, packets[0].StartQpc);
    EXPECT_EQ(800u, packets[0].EndQpc);
}

TEST(GpuPacketTrackerTests, TracksWhetherAnyPacketWasSeen)
{
    GpuPacketTracker tracker;
    EXPECT_FALSE(tracker.SawAnySubmission());
    EXPECT_EQ(0u, tracker.LastEventQpc());

    // A completion alone doesn't count - it says nothing about what was submitted.
    tracker.NoteEventTimestamp(400);
    tracker.Close(GpuPacketSource::HardwareQueue, 0x9000, 1, 400);
    EXPECT_FALSE(tracker.SawAnySubmission());

    tracker.NoteEventTimestamp(300);
    tracker.Open(GpuPacketSource::HardwareQueue, 0x9000, 2, 300, std::nullopt);
    EXPECT_TRUE(tracker.SawAnySubmission());

    // Events don't always arrive in timestamp order.
    EXPECT_EQ(400u, tracker.LastEventQpc());
}

// A session that loses completions would otherwise grow forever.
TEST(GpuPacketTrackerTests, OpenPacketsAreCappedByDroppingTheOldest)
{
    GpuPacketTracker tracker;

    constexpr uint64_t packetCount = 200 * 1024;
    for (uint64_t packet = 0; packet < packetCount; ++packet)
    {
        tracker.Open(GpuPacketSource::HardwareQueue, 0xA000, packet, packet + 1, Owner(77));
    }

    auto unfinished = tracker.CollectUnfinished(packetCount + 1);
    EXPECT_LT(unfinished.size(), packetCount);

    // Whatever survived must be the most recent work, which is what a consumer
    // still has a chance of correlating against.
    auto oldestSurvivor = std::ranges::min(unfinished, {}, &GpuPacketSpan::StartQpc);
    EXPECT_GT(oldestSurvivor.StartQpc, 1u);
}
