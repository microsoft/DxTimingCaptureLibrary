// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "pch.h"

#include "SampleProcess.h"

using namespace DxTimingCaptureLibraryTest;

// The sample's command line and its handling of the ETW session it owns. Nothing
// here captures anything, so these run anywhere and finish quickly.

TEST(PerfettoSampleCommandLine, HelpIsPrintedAndSucceeds)
{
    const SampleInvocation invocation = RunSampleToCompletion(L"--help");

    EXPECT_EQ(0u, invocation.ExitCode);
    EXPECT_NE(invocation.Output.find("Usage:"), std::string::npos) << invocation.Output;
    EXPECT_NE(invocation.Output.find("--no-other-processes"), std::string::npos) << invocation.Output;
}

TEST(PerfettoSampleCommandLine, AnUnparseableProcessIdIsRejected)
{
    const SampleInvocation invocation = RunSampleToCompletion(L"not-a-process-id");

    EXPECT_EQ(1u, invocation.ExitCode);
    EXPECT_NE(invocation.Output.find("Invalid process id"), std::string::npos) << invocation.Output;
}

TEST(PerfettoSampleCommandLine, AMissingEtlPathIsRejected)
{
    const SampleInvocation invocation = RunSampleToCompletion(L"all 1 --etl");

    EXPECT_EQ(1u, invocation.ExitCode);
    EXPECT_NE(invocation.Output.find("--etl requires a path"), std::string::npos) << invocation.Output;
}

// A leaked session keeps consuming its buffers and starves later ones.
TEST(PerfettoSampleCommandLine, TheEtwSessionIsGoneOnceTheSampleExits)
{
    const SampleRun run = RunSample(L"all", 1, nullptr);
    if (!run.SessionStarted)
    {
        GTEST_SKIP() << "The sample could not start an ETW session; run the tests elevated.\n" << run.Output;
    }

    EXPECT_EQ(0u, run.ExitCode);
    EXPECT_FALSE(SampleSessionExists(run.ProcessId)) << run.Output;
}
