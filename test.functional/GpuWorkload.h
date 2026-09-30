// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#pragma once

namespace DxTimingCaptureLibraryTest
{
    // Which adapter to run the workload on. Warp is a software adapter, so it is a
    // way to reach a scheduler path the machine's real GPU may not use.
    enum class GpuAdapterKind
    {
        Default,
        Warp,
    };

    // Runs a buffer copy on a direct queue and waits for it, so the scheduler emits a
    // submit/complete pair for GPU work owned by this process.
    void SubmitGpuWorkAndWait(GpuAdapterKind adapter = GpuAdapterKind::Default);

    // Relaunching the test executable with this switch runs SubmitGpuWorkAndWait and
    // exits. That is how a test gets GPU work from a process other than its own.
    // The parent matches on argv, which is narrow; the child is launched wide.
    inline constexpr char SubmitGpuWorkSwitch[] = "--submit-gpu-work";
    inline constexpr wchar_t SubmitGpuWorkSwitchW[] = L"--submit-gpu-work";

    // Passed alongside SubmitGpuWorkSwitch to put the child on WARP.
    inline constexpr char WarpAdapterSwitch[] = "--warp";
    inline constexpr wchar_t WarpAdapterSwitchW[] = L"--warp";

    // Exit code the child uses when it could not create a device or submit work,
    // which tells the parent to skip rather than fail.
    inline constexpr int SubmitGpuWorkFailedExitCode = 2;
}
