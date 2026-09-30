// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#pragma once

#include <DxTimingCaptureLibrary/TicksToNanoseconds.h>

// Converts the session's raw QPC tick timestamps to nanoseconds. Same converter
// the basic sample uses; see samples/basic/PrintfCallbacks.h.
class TimestampConverterImpl : public TimestampConverter
{
    LARGE_INTEGER m_frequency;

public:
    TimestampConverterImpl()
    {
        QueryPerformanceFrequency(&m_frequency);
    }

    LONGLONG GetLastEventTimeStamp() const override { return 0; }

    LONGLONG ConvertClockToTimeStamp(LONGLONG count) const override
    {
        return static_cast<LONGLONG>(DirectX::Etw::TicksToNanoseconds(
            static_cast<uint64_t>(count), static_cast<uint64_t>(m_frequency.QuadPart)));
    }

    INT64 GetHighPerformanceFrequency() const override { return m_frequency.QuadPart; }
};
