// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#pragma once

#include <functional>
#include <string>

#include "PerfettoTraceReader.h"

namespace DxTimingCaptureLibraryTest
{
    struct SampleRun
    {
        // False when the sample never got an ETW session up. Session creation needs
        // administrator rights, so tests skip rather than fail in that case.
        bool SessionStarted = false;
        unsigned long ExitCode = 0;
        unsigned long ProcessId = 0;

        // The sample's stdout and stderr, combined.
        std::string Output;

        ParsedTrace Trace;
    };

    struct SampleInvocation
    {
        unsigned long ExitCode = 0;
        std::string Output;
        ParsedTrace Trace;
    };

    // Runs the sample to completion without waiting for a session or producing any
    // work, for command lines that are not meant to capture anything.
    SampleInvocation RunSampleToCompletion(const std::wstring& sampleArguments);

    bool SampleSessionExists(unsigned long sampleProcessId);

    // Runs the perfetto sample executable against a real ETW session, calls
    // workDuringCapture once the session is up, and parses the trace it writes.
    SampleRun RunSample(
        const std::wstring& sampleArguments,
        unsigned int captureSeconds,
        const std::function<void()>& workDuringCapture);

    // Runs this executable again in its GPU work mode. Start it only once a trace is
    // running, since the child's contexts have to be created inside the trace, and
    // keep it alive to the end of the capture so the sample can still name it.
    class GpuWorkChildProcess
    {
        void* m_process = nullptr;
        unsigned long m_processId = 0;

    public:
        explicit GpuWorkChildProcess(unsigned int seconds);
        ~GpuWorkChildProcess();

        GpuWorkChildProcess(const GpuWorkChildProcess&) = delete;
        GpuWorkChildProcess& operator=(const GpuWorkChildProcess&) = delete;

        unsigned long ProcessId() const { return m_processId; }

        // False when the child could not create a device or submit work, which tells
        // the test to skip rather than fail.
        bool WaitForSuccess();
    };

    // Passed with SubmitGpuWorkSwitch to keep the child submitting for a while
    // instead of exiting after a single submit.
    inline constexpr char SubmitGpuWorkSecondsSwitch[] = "--for-seconds";
    inline constexpr wchar_t SubmitGpuWorkSecondsSwitchW[] = L"--for-seconds";

    // How the sample labels a process, so tests can look for a specific one.
    std::string SampleProcessLabel(unsigned long processId);
}
