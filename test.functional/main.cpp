// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "pch.h"

#include "GpuWorkload.h"

int __cdecl main(int argc, char** argv)
{
    // Relaunched by GpuEngineActivityTests to produce GPU work from a second process.
    bool submitGpuWork = false;
    auto adapter = DxTimingCaptureLibraryTest::GpuAdapterKind::Default;
    for (int i = 1; i < argc; ++i)
    {
        if (strcmp(argv[i], DxTimingCaptureLibraryTest::SubmitGpuWorkSwitch) == 0)
        {
            submitGpuWork = true;
        }
        else if (strcmp(argv[i], DxTimingCaptureLibraryTest::WarpAdapterSwitch) == 0)
        {
            adapter = DxTimingCaptureLibraryTest::GpuAdapterKind::Warp;
        }
    }

    if (submitGpuWork)
    {
        try
        {
            DxTimingCaptureLibraryTest::SubmitGpuWorkAndWait(adapter);
        }
        catch (...)
        {
            return DxTimingCaptureLibraryTest::SubmitGpuWorkFailedExitCode;
        }
        return 0;
    }

    testing::InitGoogleTest(&argc, argv);
    if (IsDebuggerPresent())
    {
        testing::GTEST_FLAG(catch_exceptions) = false;
    }

    return RUN_ALL_TESTS();
}
