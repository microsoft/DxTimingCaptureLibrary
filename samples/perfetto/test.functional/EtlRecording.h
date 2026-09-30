// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#pragma once

#include <filesystem>

namespace DxTimingCaptureLibraryTest
{
    // An ETW trace recorded to a file so the sample can replay it with --etl. Recording
    // it here rather than checking one in keeps the fixture matched to the machine.
    struct EtlRecording
    {
        // False when no session could be started, which needs administrator rights.
        bool Succeeded = false;

        // ETW drops events rather than blocking a provider. A recording that lost any
        // is not worth asserting against.
        unsigned int EventsLost = 0;

        std::filesystem::path Path;

        // Two separate processes submit GPU work while recording, so a test can name
        // one as the sample's target and still have another process in the trace.
        unsigned long FirstChildProcessId = 0;
        unsigned long SecondChildProcessId = 0;

        unsigned int Seconds = 0;
    };

    // Records GPU work from two child processes, with the same providers and
    // keywords the sample enables live.
    EtlRecording RecordGpuWorkToEtl(const std::filesystem::path& path, unsigned int seconds);
}
