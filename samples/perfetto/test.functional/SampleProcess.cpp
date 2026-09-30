// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "pch.h"

#include "SampleProcess.h"

#include <evntrace.h>

#include <fstream>
#include <sstream>

#include "GpuWorkload.h"

namespace
{
    constexpr wchar_t SampleExecutableName[] = L"DxTimingCaptureLibrary.sample.perfetto.exe";

    // How long to wait for the sample's session to appear before giving up on it.
    constexpr int SessionPollAttempts = 100;
    constexpr DWORD SessionPollIntervalMs = 100;

    struct UniqueHandle
    {
        HANDLE Value = nullptr;

        ~UniqueHandle()
        {
            if (Value != nullptr && Value != INVALID_HANDLE_VALUE)
            {
                CloseHandle(Value);
            }
        }
    };

    std::filesystem::path TestExecutablePath()
    {
        wchar_t path[MAX_PATH]{};
        const DWORD lengthInCharacters = GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));
        ThrowIf(lengthInCharacters == 0 || lengthInCharacters == ARRAYSIZE(path), E_FAIL, L"GetModuleFileNameW failed");
        return std::filesystem::path(std::wstring(path, lengthInCharacters));
    }

    // One directory per run, because the sample writes its trace to a fixed name
    // relative to its working directory and a test may run it more than once.
    std::filesystem::path WorkingDirectoryForCurrentTest()
    {
        static int runIndex = 0;

        const auto* testInformation = ::testing::UnitTest::GetInstance()->current_test_info();
        const auto directory = std::filesystem::temp_directory_path() /
            (std::string("DxTimingCaptureLibrary.sample.perfetto.") + testInformation->test_suite_name() + "." +
             testInformation->name() + "." + std::to_string(++runIndex));

        std::error_code errorCode;
        std::filesystem::remove_all(directory, errorCode);
        std::filesystem::create_directories(directory);
        return directory;
    }

    struct SessionQueryBuffer
    {
        static constexpr size_t NameLengthInCharacters = 1024;

        std::vector<uint8_t> Storage;

        SessionQueryBuffer()
            : Storage(sizeof(EVENT_TRACE_PROPERTIES) + 2 * NameLengthInCharacters * sizeof(wchar_t))
        {
            Properties()->Wnode.BufferSize = static_cast<ULONG>(Storage.size());
            Properties()->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
            Properties()->LogFileNameOffset = static_cast<ULONG>(sizeof(EVENT_TRACE_PROPERTIES) + NameLengthInCharacters * sizeof(wchar_t));
        }

        EVENT_TRACE_PROPERTIES* Properties()
        {
            return reinterpret_cast<EVENT_TRACE_PROPERTIES*>(Storage.data());
        }
    };

    std::wstring SampleSessionName(unsigned long processId)
    {
        return L"DxTimingCaptureLibrary.sample.perfetto.session" + std::to_wstring(processId);
    }

    bool EtwSessionExists(const std::wstring& sessionName)
    {
        SessionQueryBuffer buffer;
        return ControlTraceW(0, sessionName.c_str(), buffer.Properties(), EVENT_TRACE_CONTROL_QUERY) == ERROR_SUCCESS;
    }

    void StopEtwSession(const std::wstring& sessionName)
    {
        SessionQueryBuffer buffer;
        ControlTraceW(0, sessionName.c_str(), buffer.Properties(), EVENT_TRACE_CONTROL_STOP);
    }

    std::string ReadFileText(const std::filesystem::path& path)
    {
        std::ifstream stream(path, std::ios::binary);
        if (!stream)
        {
            return {};
        }

        std::ostringstream text;
        text << stream.rdbuf();
        return text.str();
    }

    UniqueHandle CreateInheritableOutputFile(const std::filesystem::path& path)
    {
        SECURITY_ATTRIBUTES securityAttributes{};
        securityAttributes.nLength = sizeof(securityAttributes);
        securityAttributes.bInheritHandle = TRUE;

        UniqueHandle file{ CreateFileW(
            path.c_str(),
            GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            &securityAttributes,
            CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr) };
        ThrowIf(file.Value == INVALID_HANDLE_VALUE, HRESULT_FROM_WIN32(GetLastError()), L"Could not create the sample's output file");
        return file;
    }

    std::filesystem::path SamplePath()
    {
        const auto path = TestExecutablePath().parent_path() / SampleExecutableName;
        ThrowIf(!std::filesystem::exists(path), E_FAIL, L"The perfetto sample was not built next to the tests");
        return path;
    }

    PROCESS_INFORMATION LaunchSample(
        const std::wstring& sampleArguments,
        const std::filesystem::path& workingDirectory,
        HANDLE outputFile)
    {
        STARTUPINFOW startupInformation{};
        startupInformation.cb = sizeof(startupInformation);
        startupInformation.dwFlags = STARTF_USESTDHANDLES;
        startupInformation.hStdOutput = outputFile;
        startupInformation.hStdError = outputFile;

        std::wstring commandLine = L"\"" + SamplePath().wstring() + L"\" " + sampleArguments;

        PROCESS_INFORMATION processInformation{};
        ThrowIf(!CreateProcessW(
                    nullptr,
                    commandLine.data(),
                    nullptr,
                    nullptr,
                    TRUE,
                    CREATE_NO_WINDOW,
                    nullptr,
                    workingDirectory.c_str(),
                    &startupInformation,
                    &processInformation),
                HRESULT_FROM_WIN32(GetLastError()),
                L"Could not launch the perfetto sample");
        return processInformation;
    }
}

namespace DxTimingCaptureLibraryTest
{
    SampleRun RunSample(
        const std::wstring& sampleArguments,
        unsigned int captureSeconds,
        const std::function<void()>& workDuringCapture)
    {
        SampleRun run;

        const auto workingDirectory = WorkingDirectoryForCurrentTest();
        const auto outputPath = workingDirectory / L"sample-output.txt";
        UniqueHandle outputFile = CreateInheritableOutputFile(outputPath);

        PROCESS_INFORMATION processInformation = LaunchSample(
            sampleArguments + L" " + std::to_wstring(captureSeconds),
            workingDirectory,
            outputFile.Value);

        UniqueHandle process{ processInformation.hProcess };
        UniqueHandle mainThread{ processInformation.hThread };
        run.ProcessId = processInformation.dwProcessId;

        const std::wstring sessionName = SampleSessionName(processInformation.dwProcessId);

        // The sample's stdout is block-buffered into a file, so its banner says nothing
        // about how far along it is. Its ETW session appearing does.
        for (int attempt = 0; attempt < SessionPollAttempts; ++attempt)
        {
            if (EtwSessionExists(sessionName))
            {
                run.SessionStarted = true;
                break;
            }

            if (WaitForSingleObject(process.Value, SessionPollIntervalMs) == WAIT_OBJECT_0)
            {
                break;
            }
        }

        if (run.SessionStarted && workDuringCapture)
        {
            workDuringCapture();
        }

        const DWORD exitWaitMs = (captureSeconds + 30) * 1000;
        if (WaitForSingleObject(process.Value, exitWaitMs) != WAIT_OBJECT_0)
        {
            TerminateProcess(process.Value, 1);
            WaitForSingleObject(process.Value, 5000);
            // A killed sample never stops its session, and the orphan starves later ones.
            StopEtwSession(sessionName);
            ADD_FAILURE() << "The perfetto sample did not stop on its own within " << exitWaitMs << "ms";
        }

        GetExitCodeProcess(process.Value, &run.ExitCode);
        run.Output = ReadFileText(outputPath);

        const auto tracePath = workingDirectory / L"dxtimingcapture.perfetto-trace";
        if (std::filesystem::exists(tracePath))
        {
            run.Trace = ReadPerfettoTrace(tracePath.string());
        }

        return run;
    }

    SampleInvocation RunSampleToCompletion(const std::wstring& sampleArguments)
    {
        SampleInvocation invocation;

        const auto workingDirectory = WorkingDirectoryForCurrentTest();
        const auto outputPath = workingDirectory / L"sample-output.txt";
        UniqueHandle outputFile = CreateInheritableOutputFile(outputPath);

        PROCESS_INFORMATION processInformation = LaunchSample(sampleArguments, workingDirectory, outputFile.Value);
        UniqueHandle process{ processInformation.hProcess };
        UniqueHandle mainThread{ processInformation.hThread };

        ThrowIf(WaitForSingleObject(process.Value, 60 * 1000) != WAIT_OBJECT_0, E_FAIL, L"The perfetto sample did not exit");
        GetExitCodeProcess(process.Value, &invocation.ExitCode);
        invocation.Output = ReadFileText(outputPath);

        const auto tracePath = workingDirectory / L"dxtimingcapture.perfetto-trace";
        if (std::filesystem::exists(tracePath))
        {
            invocation.Trace = ReadPerfettoTrace(tracePath.string());
        }

        return invocation;
    }

    bool SampleSessionExists(unsigned long sampleProcessId)
    {
        return EtwSessionExists(SampleSessionName(sampleProcessId));
    }

    GpuWorkChildProcess::GpuWorkChildProcess(unsigned int seconds)
    {
        std::wstring commandLine = L"\"" + TestExecutablePath().wstring() + L"\" " + SubmitGpuWorkSwitchW +
            L" " + SubmitGpuWorkSecondsSwitchW + L" " + std::to_wstring(seconds);

        STARTUPINFOW startupInformation{};
        startupInformation.cb = sizeof(startupInformation);

        PROCESS_INFORMATION processInformation{};
        ThrowIf(!CreateProcessW(
                    nullptr,
                    commandLine.data(),
                    nullptr,
                    nullptr,
                    FALSE,
                    CREATE_NO_WINDOW,
                    nullptr,
                    nullptr,
                    &startupInformation,
                    &processInformation),
                HRESULT_FROM_WIN32(GetLastError()),
                L"Could not launch the GPU work child process");

        CloseHandle(processInformation.hThread);
        m_process = processInformation.hProcess;
        m_processId = processInformation.dwProcessId;
    }

    GpuWorkChildProcess::~GpuWorkChildProcess()
    {
        if (m_process != nullptr)
        {
            if (WaitForSingleObject(m_process, 60 * 1000) != WAIT_OBJECT_0)
            {
                TerminateProcess(m_process, 1);
            }
            CloseHandle(m_process);
        }
    }

    bool GpuWorkChildProcess::WaitForSuccess()
    {
        if (WaitForSingleObject(m_process, 60 * 1000) != WAIT_OBJECT_0)
        {
            return false;
        }

        DWORD exitCode = 0;
        GetExitCodeProcess(m_process, &exitCode);
        return exitCode == 0;
    }

    std::string SampleProcessLabel(unsigned long processId)
    {
        return TestExecutablePath().filename().string() + " (" + std::to_string(processId) + ")";
    }
}
