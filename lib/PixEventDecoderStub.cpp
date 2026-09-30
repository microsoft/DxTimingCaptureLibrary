// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "pch.h"

#include <PixEventDecoder.h>

#include <atomic>
#include <cstdio>

// Stub for PixEventDecoder::DecodeTimingBlock, the single decoder entry point
// DxTimingCaptureLibrary uses. It returns an empty block, so PIX timing/marker events are
// dropped while every other ETW category works normally. Set
// UseStubPixEventDecoder=false and link the real PixEventDecoder.lib to restore
// PIX decoding.

#pragma message(__FILE__ ": using a stub PixEventDecoder::DecodeTimingBlock; PIX timing/marker events are not decoded.")

namespace PixEventDecoder
{
    DecodedPixEventBlock DecodeTimingBlock(
        bool /*ignoreEventContexts*/,
        bool /*gpuOnlyEvents*/,
        uint32_t /*bufferSize*/,
        uint8_t* /*buffer*/,
        ConvertClockToNanoseconds const& /*convertClockToNanoseconds*/)
    {
        // Warn once, not once per PIX block.
        static std::atomic<bool> warned{ false };
        if (!warned.exchange(true))
        {
            const char* const message =
                "[DxTimingCaptureLibrary] PixEventDecoder::DecodeTimingBlock is a STUB; PIX "
                "timing/marker events are not being decoded. Replace "
                "lib/PixEventDecoderStub.cpp with the real PixEventDecoder.\n";
            OutputDebugStringA(message);
            std::fputs(message, stderr);
        }

        return DecodedPixEventBlock{};
    }
}
