// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#pragma once

// Minimal subset of the PIX ETW manifest (PIXETW.man, shipped with WinPixEventRuntime). 
// Keep these values in sync with PIXETW.man.

#include <guiddef.h> // GUID
#include <minwindef.h> // USHORT

namespace DirectX::Etw
{
    // These mirror symbols from PIX's generated PixEtw.h, in a separate namespace to avoid collisions.
    namespace PixEtw
    {
        // provider symbol="PIX_ETW_PROVIDER_WINDOWS"
        // guid="{8819CA17-7FAA-4184-AFB7-A4120CA793CD}"
        inline constexpr GUID PIX_ETW_PROVIDER_WINDOWS =
        {
            0x8819ca17, 0x7faa, 0x4184, { 0xaf, 0xb7, 0xa4, 0x12, 0x0c, 0xa7, 0x93, 0xcd }
        };

        // Event IDs (the "value" attribute on each <event> in the manifest). The
        // dispatcher matches these against EVENT_RECORD::EventHeader::EventDescriptor::Id.
        inline constexpr USHORT PIXTrackMemoryAllocation_value = 16;
        inline constexpr USHORT PIXTrackMemoryFree_value = 17;
        inline constexpr USHORT PIXRecordTimingBlock_v1_value = 18;
        inline constexpr USHORT PIXReportCounterData_value = 20;
        inline constexpr USHORT PIXRecordTimingBlock_v2_value = 22;
    }
}
