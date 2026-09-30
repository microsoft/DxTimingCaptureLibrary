// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "pch.h"

#include <DxTimingCaptureLibrary/TicksToNanoseconds.h>

using DirectX::Etw::TicksToNanoseconds;

TEST(TicksToNanosecondsTests, ZeroTicksReturnsZero)
{
    EXPECT_EQ(0u, TicksToNanoseconds(0, 1000000000));
    EXPECT_EQ(0u, TicksToNanoseconds(0, 100000000));
    EXPECT_EQ(0u, TicksToNanoseconds(0, 10000000));
    EXPECT_EQ(0u, TicksToNanoseconds(0, 1));
}

TEST(TicksToNanosecondsTests, OneTickEqualsOneNanosecond)
{
    // When frequency is 1GHz (1,000,000,000 Hz), 1 tick = 1 nanosecond
    constexpr uint64_t OneGHz = 1000000000;

    EXPECT_EQ(1u, TicksToNanoseconds(1, OneGHz));
    EXPECT_EQ(10u, TicksToNanoseconds(10, OneGHz));
    EXPECT_EQ(100u, TicksToNanoseconds(100, OneGHz));
    EXPECT_EQ(1000u, TicksToNanoseconds(1000, OneGHz));
    EXPECT_EQ(18446744073u, TicksToNanoseconds(18446744073, OneGHz));
    EXPECT_EQ(18446744074u, TicksToNanoseconds(18446744074, OneGHz));
    EXPECT_EQ(1518152166925797472u, TicksToNanoseconds(1518152166925797472, OneGHz));
    EXPECT_EQ(0xFFFFFFFFFFFFFFFFu, TicksToNanoseconds(0xFFFFFFFFFFFFFFFF, OneGHz));
}

TEST(TicksToNanosecondsTests, OneTickEqualsTenNanoseconds)
{
    // When frequency is 100MHz (100,000,000 Hz), 1 tick = 10 nanoseconds
    constexpr uint64_t HundredMHz = 100000000;

    EXPECT_EQ(10u, TicksToNanoseconds(1, HundredMHz));
    EXPECT_EQ(100u, TicksToNanoseconds(10, HundredMHz));
    EXPECT_EQ(184467440730u, TicksToNanoseconds(18446744073, HundredMHz));
    EXPECT_EQ(184467440740u, TicksToNanoseconds(18446744074, HundredMHz));
    EXPECT_EQ(18446744073709551610u, TicksToNanoseconds(1844674407370955161, HundredMHz));
}

TEST(TicksToNanosecondsTests, OneTickEqualsHundredNanoseconds)
{
    // When frequency is 10MHz (10,000,000 Hz), 1 tick = 100 nanoseconds
    // This is the frequency used by Windows FILETIME
    constexpr uint64_t TenMHz = 10000000;

    EXPECT_EQ(100u, TicksToNanoseconds(1, TenMHz));
    EXPECT_EQ(1000u, TicksToNanoseconds(10, TenMHz));
    EXPECT_EQ(10000u, TicksToNanoseconds(100, TenMHz));
    EXPECT_EQ(1000000u, TicksToNanoseconds(10000, TenMHz));
    EXPECT_EQ(1000000000u, TicksToNanoseconds(10000000, TenMHz)); // 1 second
}

TEST(TicksToNanosecondsTests, OneTickEqualsOneMicrosecond)
{
    // When frequency is 1MHz (1,000,000 Hz), 1 tick = 1000 nanoseconds = 1 microsecond
    constexpr uint64_t OneMHz = 1000000;

    EXPECT_EQ(1000u, TicksToNanoseconds(1, OneMHz));
    EXPECT_EQ(10000u, TicksToNanoseconds(10, OneMHz));
    EXPECT_EQ(1000000u, TicksToNanoseconds(1000, OneMHz)); // 1 millisecond
    EXPECT_EQ(1000000000u, TicksToNanoseconds(1000000, OneMHz)); // 1 second
}

TEST(TicksToNanosecondsTests, OneTickEqualsOneMillisecond)
{
    // When frequency is 1KHz (1,000 Hz), 1 tick = 1,000,000 nanoseconds = 1 millisecond
    constexpr uint64_t OneKHz = 1000;

    EXPECT_EQ(1000000u, TicksToNanoseconds(1, OneKHz));
    EXPECT_EQ(10000000u, TicksToNanoseconds(10, OneKHz));
    EXPECT_EQ(1000000000u, TicksToNanoseconds(1000, OneKHz)); // 1 second
}

TEST(TicksToNanosecondsTests, OneTickEqualsOneSecond)
{
    // When frequency is 1Hz, 1 tick = 1 second = 1,000,000,000 nanoseconds
    constexpr uint64_t OneHz = 1;

    EXPECT_EQ(1000000000u, TicksToNanoseconds(1, OneHz));
    EXPECT_EQ(2000000000u, TicksToNanoseconds(2, OneHz));
    EXPECT_EQ(10000000000u, TicksToNanoseconds(10, OneHz));
}

TEST(TicksToNanosecondsTests, TypicalQPCFrequencies)
{
    // Test with typical QueryPerformanceCounter frequencies seen on real hardware
    // These are commonly around 10MHz or based on CPU frequency

    // ~10MHz (common on many systems)
    EXPECT_EQ(100u, TicksToNanoseconds(1, 10000000));
    EXPECT_EQ(1000000000u, TicksToNanoseconds(10000000, 10000000)); // 1 second

    // ~3.3MHz (seen on some systems)
    EXPECT_EQ(303u, TicksToNanoseconds(1, 3300000));

    // ~2.5MHz
    EXPECT_EQ(400u, TicksToNanoseconds(1, 2500000));
}

TEST(TicksToNanosecondsTests, LargeTickValues)
{
    constexpr uint64_t OneGHz = 1000000000;

    // Test large but representable values
    EXPECT_EQ(1000000000000u, TicksToNanoseconds(1000000000000, OneGHz)); // ~16.7 minutes
    EXPECT_EQ(3600000000000u, TicksToNanoseconds(3600000000000, OneGHz)); // 1 hour
    EXPECT_EQ(86400000000000u, TicksToNanoseconds(86400000000000, OneGHz)); // 1 day
}

TEST(TicksToNanosecondsTests, FractionalConversions)
{
    // When the conversion doesn't result in an exact integer, verify truncation behavior
    // 7 ticks at 3Hz = 7 * 1,000,000,000 / 3 = 2,333,333,333.33... -> 2,333,333,333
    EXPECT_EQ(2333333333u, TicksToNanoseconds(7, 3));

    // 1 tick at 3Hz = 1,000,000,000 / 3 = 333,333,333.33... -> 333,333,333
    EXPECT_EQ(333333333u, TicksToNanoseconds(1, 3));

    // 2 ticks at 3Hz = 2,000,000,000 / 3 = 666,666,666.66... -> 666,666,666
    EXPECT_EQ(666666666u, TicksToNanoseconds(2, 3));
}

TEST(TicksToNanosecondsTests, OverflowThrowsException)
{
    // Test cases that would overflow uint64_t and should throw

    // 1 tick == 10ns case: values above 1844674407370955161 will overflow
    EXPECT_THROW(TicksToNanoseconds(1844674407370955162, 100000000), std::exception);
    EXPECT_THROW(TicksToNanoseconds(UINT64_MAX, 100000000), std::exception);

    // 1 tick == 100ns case: values above 184467440737095516 will overflow
    EXPECT_THROW(TicksToNanoseconds(184467440737095517, 10000000), std::exception);

    // 1 tick == 1us case: values above 18446744073709551 will overflow
    EXPECT_THROW(TicksToNanoseconds(18446744073709552, 1000000), std::exception);

    // 1 tick == 1ms case: values above 18446744073709 will overflow
    EXPECT_THROW(TicksToNanoseconds(18446744073710, 1000), std::exception);

    // 1 tick == 1s case: values above 18446744073 will overflow
    EXPECT_THROW(TicksToNanoseconds(18446744074, 1), std::exception);
}

TEST(TicksToNanosecondsTests, BoundaryValues)
{
    constexpr uint64_t OneGHz = 1000000000;
    constexpr uint64_t HundredMHz = 100000000;

    // Test values near uint64_t max for 1:1 conversion (shouldn't overflow)
    EXPECT_EQ(UINT64_MAX, TicksToNanoseconds(UINT64_MAX, OneGHz));
    EXPECT_EQ(UINT64_MAX - 1, TicksToNanoseconds(UINT64_MAX - 1, OneGHz));

    // Test the largest valid value for 1 tick == 10ns
    EXPECT_EQ(18446744073709551610u, TicksToNanoseconds(1844674407370955161, HundredMHz));
}

TEST(TicksToNanosecondsTests, ConsistencyAcrossEquivalentCalculations)
{
    // Verify that different tick/frequency combinations yielding the same real time
    // produce the same nanosecond result

    // 1 second expressed in different ways
    EXPECT_EQ(TicksToNanoseconds(1000000000, 1000000000), TicksToNanoseconds(100000000, 100000000));
    EXPECT_EQ(TicksToNanoseconds(1000000000, 1000000000), TicksToNanoseconds(10000000, 10000000));
    EXPECT_EQ(TicksToNanoseconds(1000000000, 1000000000), TicksToNanoseconds(1000000, 1000000));
    EXPECT_EQ(TicksToNanoseconds(1000000000, 1000000000), TicksToNanoseconds(1, 1));

    // All should equal 1 second = 1,000,000,000 nanoseconds
    EXPECT_EQ(1000000000u, TicksToNanoseconds(1000000000, 1000000000));
}

TEST(TicksToNanosecondsTests, SmallFrequencyValues)
{
    // Very small frequencies (large time per tick)
    EXPECT_EQ(1000000000u, TicksToNanoseconds(1, 1)); // 1 tick at 1Hz = 1 second
    EXPECT_EQ(500000000u, TicksToNanoseconds(1, 2));  // 1 tick at 2Hz = 0.5 seconds
    EXPECT_EQ(250000000u, TicksToNanoseconds(1, 4));  // 1 tick at 4Hz = 0.25 seconds
    EXPECT_EQ(100000000u, TicksToNanoseconds(1, 10)); // 1 tick at 10Hz = 0.1 seconds
}

TEST(TicksToNanosecondsTests, HighFrequencyValues)
{
    // Very high frequencies (small time per tick)
    constexpr uint64_t TenGHz = 10000000000;

    // At 10GHz, 1 tick = 0.1 nanoseconds, which truncates to 0
    EXPECT_EQ(0u, TicksToNanoseconds(1, TenGHz));

    // 10 ticks at 10GHz = 1 nanosecond
    EXPECT_EQ(1u, TicksToNanoseconds(10, TenGHz));

    // 100 ticks at 10GHz = 10 nanoseconds
    EXPECT_EQ(10u, TicksToNanoseconds(100, TenGHz));
}
