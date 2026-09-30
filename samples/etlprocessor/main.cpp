// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "pch.h"

#include <cerrno>
#include <cwchar>
#include <limits>

using namespace EtlProcessor;

namespace
{
    void PrintUsage()
    {
        std::wcout <<
            L"\n"
            L"DxTimingCaptureLibrary ETL processor sample.\n"
            L"\n"
            L"Reads a saved ETW trace (.etl) and reports GPU work in it using\n"
            L"DxTimingCaptureLibrary's EtwDispatcher.\n"
            L"\n"
            L"Usage: DxTimingCaptureLibrary.EtlProcessor --etl <path> --action <action> [options]\n"
            L"\n"
            L"Actions:\n"
            L"  dumppids            List every process that created a D3D12 device.\n"
            L"  dumphistorybuffers  Dump GPU submission timings as a CSV table. Pass\n"
            L"                      --pid to limit output to one process.\n"
            L"  dumpallocations     Report the tracked class of dxgkrnl allocations\n"
            L"                      left outstanding at the end of the trace.\n"
            L"\n"
            L"Options:\n"
            L"  --pid <id>          Process to filter against (-1 means all). Default -1.\n"
            L"  --nohighlow         Emit each timestamp as one 64-bit value instead of\n"
            L"                      separate low/high columns.\n"
            L"  --help              Show this message.\n";
    }

    int UsageError(std::wstring_view message)
    {
        std::wcout << message << L"\n";
        PrintUsage();
        return 1;
    }

    enum class ToolAction
    {
        DumpPids,
        DumpHistoryBuffers,
        DumpAllocations,
    };

    bool TryParseAction(std::wstring const& token, ToolAction& action)
    {
        if (_wcsicmp(token.c_str(), L"dumppids") == 0)
        {
            action = ToolAction::DumpPids;
        }
        else if (_wcsicmp(token.c_str(), L"dumphistorybuffers") == 0)
        {
            action = ToolAction::DumpHistoryBuffers;
        }
        else if (_wcsicmp(token.c_str(), L"dumpallocations") == 0)
        {
            action = ToolAction::DumpAllocations;
        }
        else
        {
            return false;
        }

        return true;
    }

    // Accepts -1 (meaning "all processes") or a non-negative process id. Rejects
    // trailing junk, other negatives, and out-of-range values.
    bool TryParseProcessId(std::wstring const& token, int& processId)
    {
        wchar_t* end = nullptr;
        errno = 0;
        long value = std::wcstol(token.c_str(), &end, 10);

        if (token.empty() || end != token.c_str() + token.size() || errno == ERANGE)
        {
            return false;
        }

        if (value < -1 || value > std::numeric_limits<int>::max())
        {
            return false;
        }

        processId = static_cast<int>(value);
        return true;
    }

    void DumpHistoryBuffers(std::wstring const& etlFilePath, int processId, bool noHighLow)
    {
        // With no specific process, enumerate the processes doing GPU work first
        // and emit a table per process.
        if (processId < 0)
        {
            PidFinderProcessor pidFinder;
            EtwDispatcher<PidFinderProcessor> pidDispatcher(&pidFinder);
            ProcessEtlFile(etlFilePath, &pidDispatcher);

            for (auto const& [pid, eventCount] : pidFinder)
            {
                HistoryBufferProcessor history(pid);
                EtwDispatcher<HistoryBufferProcessor> historyDispatcher(&history);
                ProcessEtlFile(etlFilePath, &historyDispatcher);
                history.OutputSubmissionsAsCSVTable(noHighLow);
                std::cout << "\n";
            }
        }
        else
        {
            HistoryBufferProcessor history(processId);
            EtwDispatcher<HistoryBufferProcessor> dispatcher(&history);
            ProcessEtlFile(etlFilePath, &dispatcher);
            history.OutputSubmissionsAsCSVTable(noHighLow);
        }
    }
}

int __cdecl wmain(int argc, wchar_t** argv)
{
    std::wstring etlFilePath;
    std::wstring actionText;
    int processId = -1;
    bool noHighLow = false;

    for (int i = 1; i < argc; ++i)
    {
        std::wstring arg = argv[i];

        auto takeValue = [&](std::wstring& out) -> bool
        {
            if (i + 1 >= argc)
            {
                return false;
            }
            out = argv[++i];
            return true;
        };

        if (arg == L"--help" || arg == L"-h")
        {
            PrintUsage();
            return 0;
        }
        else if (arg == L"--etl")
        {
            if (!takeValue(etlFilePath))
            {
                return UsageError(L"--etl requires a file path.");
            }
        }
        else if (arg == L"--action")
        {
            if (!takeValue(actionText))
            {
                return UsageError(L"--action requires a value.");
            }
        }
        else if (arg == L"--pid")
        {
            std::wstring pidText;
            if (!takeValue(pidText) || !TryParseProcessId(pidText, processId))
            {
                return UsageError(L"--pid requires -1 or a non-negative process id.");
            }
        }
        else if (arg == L"--nohighlow")
        {
            noHighLow = true;
        }
        else
        {
            return UsageError(L"Unknown argument: " + arg);
        }
    }

    ToolAction action;
    if (etlFilePath.empty() || actionText.empty() || !TryParseAction(actionText, action))
    {
        PrintUsage();
        return 1;
    }

    try
    {
        switch (action)
        {
            case ToolAction::DumpPids:
            {
                PidFinderProcessor pidFinder;
                EtwDispatcher<PidFinderProcessor> dispatcher(&pidFinder);
                ProcessEtlFile(etlFilePath, &dispatcher);
                pidFinder.OutputPidsWithD3D12Devices();
                break;
            }
            case ToolAction::DumpHistoryBuffers:
                DumpHistoryBuffers(etlFilePath, processId, noHighLow);
                break;
            case ToolAction::DumpAllocations:
            {
                AllocationTracker tracker;
                EtwDispatcher<AllocationTracker> dispatcher(&tracker);
                ProcessEtlFile(etlFilePath, &dispatcher);
                break;
            }
        }
    }
    catch (std::exception const& e)
    {
        std::cout << "\n" << e.what() << "\n";
        return 1;
    }

    return 0;
}
