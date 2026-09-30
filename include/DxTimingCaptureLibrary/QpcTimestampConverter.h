// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#pragma once

#include <DxTimingCaptureLibrary/DxTimingCaptureEventHandler.h>
#include <DxTimingCaptureLibrary/TicksToNanoseconds.h>

namespace DirectX::Etw
{

// A ready-made TimestampConverter for the common raw-QPC case, so most consumers
// don't have to write one. Uses the frequency etc from the current machine.
class QpcTimestampConverter : public TimestampConverter
{
    INT64 m_frequency;

public:
    explicit QpcTimestampConverter(INT64 qpcFrequency) : m_frequency(qpcFrequency) {}

    static QpcTimestampConverter ForCurrentMachine()
    {
        LARGE_INTEGER frequency;
        QueryPerformanceFrequency(&frequency);
        return QpcTimestampConverter(frequency.QuadPart);
    }

    INT64 GetLastEventTimeStamp() const override { return 0; }

    INT64 ConvertClockToTimeStamp(INT64 count) const override
    {
        return static_cast<INT64>(TicksToNanoseconds(
            static_cast<uint64_t>(count), static_cast<uint64_t>(m_frequency)));
    }

    INT64 GetHighPerformanceFrequency() const override { return m_frequency; }
};

} // namespace DirectX::Etw