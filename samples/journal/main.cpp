// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

// A minimal console sample that shows the runtime-failure journal path end to end.
//
// It starts a real-time ETW session on its own process, then deliberately records
// a CopyBufferRegion that reads past the end of the source buffer. The D3D12
// runtime rejects the copy, removes the command list, and writes the failure to
// its journal, which it emits over ETW. DxTimingCaptureLibrary decodes that event and hands
// it to our callback, which prints it to the console.

#include "pch.h"

#include <DxTimingCaptureLibrary/DxTimingCaptureLibrary.h>
#include <DxTimingCaptureLibrary/EtwException.h>

#include "EtwConfig.h"

using namespace DirectX::Etw;
using DirectX::Etw::Errors::ThrowFailure;

namespace
{
    // Prints every runtime-failure journal entry the D3D12 runtime emits.
    class ConsoleRuntimeFailureCallbacks : public RuntimeFailureCallbacks
    {
    public:
        HRESULT OnD3D12JournalEntry(INT64 timestamp, UINT32 index, UINT32 code, UINT32 threadId, std::string_view message) override
        {
            printf("[D3D12 runtime failure] hr=0x%08X thread=%u slot=%u: %.*s\n",
                code, threadId, index, static_cast<int>(message.size()), message.data());
            return S_OK;
        }
    };

    ConsoleRuntimeFailureCallbacks g_runtimeFailureCallbacks;
    std::unique_ptr<DxTimingCaptureEventHandler> g_dxTimingCaptureEventHandler;

    void WINAPI OnEvent(EVENT_RECORD* record)
    {
        // HandleEventRecord runs inside ProcessTrace's C call frames, where letting
        // an exception unwind is undefined behaviour. Drop the event and keep going.
        try
        {
            g_dxTimingCaptureEventHandler->HandleEventRecord(record);
        }
        catch (...)
        {
            printf("Dropped an event: HandleEventRecord threw.\n");
        }
    }

    // Pumps the session's events into the handler until the session stops.
    void ConsumerThread(std::wstring sessionName)
    {
        EVENT_TRACE_LOGFILE trace = {};
        trace.LoggerName = const_cast<LPWSTR>(sessionName.c_str());
        trace.ProcessTraceMode =
            PROCESS_TRACE_MODE_REAL_TIME |
            PROCESS_TRACE_MODE_EVENT_RECORD |
            PROCESS_TRACE_MODE_RAW_TIMESTAMP;
        trace.EventRecordCallback = OnEvent;

        TRACEHANDLE traceHandle = OpenTrace(&trace);
        if (traceHandle == INVALID_PROCESSTRACE_HANDLE)
        {
            printf("OpenTrace failed: %lu\n", GetLastError());
            return;
        }

        ProcessTrace(&traceHandle, 1, nullptr, nullptr); // blocks until the session stops
        g_dxTimingCaptureEventHandler->OnDataComplete();
        CloseTrace(traceHandle);
    }

    ComPtr<ID3D12Resource> CreateDefaultBuffer(ID3D12Device* device, UINT64 sizeInBytes)
    {
        D3D12_HEAP_PROPERTIES heapProperties{};
        heapProperties.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC bufferDesc{};
        bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bufferDesc.Width = sizeInBytes;
        bufferDesc.Height = 1;
        bufferDesc.DepthOrArraySize = 1;
        bufferDesc.MipLevels = 1;
        bufferDesc.SampleDesc.Count = 1;
        bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        ComPtr<ID3D12Resource> buffer;
        ThrowFailure(device->CreateCommittedResource(
            &heapProperties,
            D3D12_HEAP_FLAG_NONE,
            &bufferDesc,
            D3D12_RESOURCE_STATE_COMMON,
            nullptr,
            IID_PPV_ARGS(&buffer)));
        return buffer;
    }
}

int __cdecl main()
{
    const DWORD processId = GetCurrentProcessId();
    const std::wstring sessionName =
        L"DxTimingCaptureLibrary.sample.journal.session" + std::to_wstring(processId);

    DxTimingCaptureLibraryOptions libraryOptions{};

    DxTimingCaptureEventCallbacks callbacks;
    callbacks.RuntimeFailureCallbacks = &g_runtimeFailureCallbacks;

    g_dxTimingCaptureEventHandler = DxTimingCaptureEventHandler::Create(processId, libraryOptions, callbacks);

    EventTraceProperties properties{};
    TRACEHANDLE sessionHandle = 0;
    ULONG status = StartTraceW(&sessionHandle, sessionName.c_str(), properties);
    if (status != ERROR_SUCCESS)
    {
        printf("StartTraceW failed: %lu\n", status);
        return 1;
    }

    EnableD3D12Provider(sessionHandle);

    std::thread consumer(ConsumerThread, sessionName);

    printf("Recording a deliberately invalid CopyBufferRegion...\n");

    ComPtr<ID3D12Device> device;
    ThrowFailure(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));

    constexpr UINT64 bufferSizeInBytes = 256;
    ComPtr<ID3D12Resource> sourceBuffer = CreateDefaultBuffer(device.Get(), bufferSizeInBytes);
    ComPtr<ID3D12Resource> destinationBuffer = CreateDefaultBuffer(device.Get(), bufferSizeInBytes);

    ComPtr<ID3D12CommandAllocator> commandAllocator;
    ThrowFailure(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&commandAllocator)));

    ComPtr<ID3D12GraphicsCommandList> commandList;
    ThrowFailure(device->CreateCommandList(
        0,
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        commandAllocator.Get(),
        nullptr,
        IID_PPV_ARGS(&commandList)));

    // The mistake: copy far more bytes than either buffer holds, starting past the
    // end of the source. The runtime rejects the out-of-bounds region, removes the
    // command list, and journals the failure, which our callback then prints.
    commandList->CopyBufferRegion(
        destinationBuffer.Get(),
        0,                       // DstOffset
        sourceBuffer.Get(),
        bufferSizeInBytes,       // SrcOffset: one byte past the last valid byte
        bufferSizeInBytes * 16); // NumBytes: far larger than either buffer

    HRESULT closeResult = commandList->Close();
    printf("Close() returned 0x%08X\n", closeResult);

    // Let the consumer thread receive and dispatch the journal event before we stop.
    Sleep(1000);

    printf("Stopping session...\n");
    ControlTraceW(sessionHandle, sessionName.c_str(), properties, EVENT_TRACE_CONTROL_STOP);
    consumer.join();

    printf("Done.\n");
    return 0;
}
