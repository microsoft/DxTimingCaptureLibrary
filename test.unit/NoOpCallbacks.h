// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#pragma once

// The no-op callback bases are part of the public surface. Tests use the shipped
// header so they exercise exactly what consumers get.
#include <DxTimingCaptureLibrary/NoOpCallbacks.h>
#include <DxTimingCaptureLibrary/DxTimingCaptureEventHandler.h>

using namespace DirectX::Etw;

// Passes the incoming ETW timestamp through unchanged (identity conversion).
// Adequate for tests; a real consumer must convert QPC ticks to nanoseconds
// (see TimestampConverter and QpcTimestampConverter).
class NoOpTimestampConverter : public TimestampConverter
{
public:
    INT64 GetLastEventTimeStamp() const override { return 0; }
    INT64 ConvertClockToTimeStamp(INT64 count) const override { return count; }
    INT64 GetHighPerformanceFrequency() const override { return 10000000; }
};
