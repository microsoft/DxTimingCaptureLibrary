// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "pch.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include <DxTimingCaptureLibrary/DxTimingCaptureEventHandler.h>
#include "EtwSession.h"

using namespace DirectX::Etw;

// Live-device test for the command-list-name path. It captures GPU timing over ETW
// while a named command list executes real GPU work, then confirms the friendly name
// reaches the consumer through GpuTimingsCallbacks::OnCommandListName. The name is
// only surfaced once the execution completes and carried work, so the test submits a
// buffer copy and waits for the GPU before ending the session.

namespace
{
    class RecordingGpuTimingsCallbacks : public GpuTimingsCallbacks
    {
        std::atomic<UINT64> m_nextExecutionId{ 1 };
        std::atomic<UINT64> m_nextMarkerId{ 1 };

    public:
        struct CommandListNameEntry
        {
            UINT64 GpuExecutionId;
            UINT32 CommandListIndexInExecution;
            std::wstring Name;
        };

        std::vector<CommandListNameEntry> CommandListNames;
        int ExecutionCompleteCount = 0;

        HRESULT OnGpuExecutionBegin(UINT32, UINT32, UINT64, INT64, UINT64, UINT64* gpuExecutionId) override
        {
            if (gpuExecutionId != nullptr)
            {
                *gpuExecutionId = ++m_nextExecutionId;
            }
            return S_OK;
        }

        HRESULT OnGpuExecutionComplete(UINT64) override
        {
            ++ExecutionCompleteCount;
            return S_OK;
        }

        HRESULT OnGpuWork(UINT64, UINT64, UINT32, UINT64, INT64, INT64) override
        {
            return S_OK;
        }

        HRESULT OnApiMarker(UINT32, UINT32, PCWSTR, INT64, UINT64* apiMarkerId) override
        {
            if (apiMarkerId != nullptr)
            {
                *apiMarkerId = ++m_nextMarkerId;
            }
            return S_OK;
        }

        HRESULT OnCommandListName(UINT64 gpuExecutionId, UINT32 commandListIndexInExecution, PCWSTR name) override
        {
            CommandListNames.push_back({ gpuExecutionId, commandListIndexInExecution, name == nullptr ? std::wstring() : std::wstring(name) });
            return S_OK;
        }
    };

    ComPtr<ID3D12Device> CreateDevice()
    {
        ComPtr<ID3D12Device> device;
        ThrowFailure(D3D12CreateDevice(
            nullptr,
            D3D_FEATURE_LEVEL_11_0,
            IID_PPV_ARGS(&device)));
        return device;
    }

    ComPtr<ID3D12Resource> CreateBuffer(ID3D12Device* device, UINT64 sizeInBytes)
    {
        D3D12_HEAP_PROPERTIES heapProperties{};
        heapProperties.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC bufferDesc{};
        bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bufferDesc.Width = sizeInBytes;
        bufferDesc.Height = 1;
        bufferDesc.DepthOrArraySize = 1;
        bufferDesc.MipLevels = 1;
        bufferDesc.Format = DXGI_FORMAT_UNKNOWN;
        bufferDesc.SampleDesc.Count = 1;
        bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        bufferDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

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

TEST(GpuTimingTests, NamedCommandListSurfacesOnCommandListName)
{
    constexpr wchar_t CommandListName[] = L"DxTimingCaptureLibrary.TestCommandList";
    constexpr UINT64 BufferSizeInBytes = 256;

    auto callbacks = std::make_unique<RecordingGpuTimingsCallbacks>();
    auto* recorder = callbacks.get();

    SimpleEtwSession session;
    session.SetGpuTimingsCallbacks(std::move(callbacks));
    session.Begin();

    ComPtr<ID3D12Device> device = CreateDevice();

    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> commandQueue;
    ThrowFailure(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&commandQueue)));

    ComPtr<ID3D12CommandAllocator> commandAllocator;
    ThrowFailure(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&commandAllocator)));

    ComPtr<ID3D12GraphicsCommandList> commandList;
    ThrowFailure(device->CreateCommandList(
        0,
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        commandAllocator.Get(),
        nullptr,
        IID_PPV_ARGS(&commandList)));

    // The friendly name is what OnCommandListName is expected to surface.
    ThrowFailure(commandList->SetName(CommandListName));

    // Buffers are implicitly in (and promoted from) the COMMON state, so a copy between two
    // distinct buffers needs no explicit barriers and gives the submission real GPU work.
    ComPtr<ID3D12Resource> sourceBuffer = CreateBuffer(device.Get(), BufferSizeInBytes);
    ComPtr<ID3D12Resource> destinationBuffer = CreateBuffer(device.Get(), BufferSizeInBytes);
    commandList->CopyBufferRegion(destinationBuffer.Get(), 0, sourceBuffer.Get(), 0, BufferSizeInBytes);
    ThrowFailure(commandList->Close());

    ID3D12CommandList* commandListsToExecute[] = { commandList.Get() };
    commandQueue->ExecuteCommandLists(1, commandListsToExecute);

    // Wait for the GPU to finish so the DXGK scheduler emits the execution's timing events.
    ComPtr<ID3D12Fence> fence;
    ThrowFailure(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
    ThrowFailure(commandQueue->Signal(fence.Get(), 1));
    // A null event handle makes SetEventOnCompletion block until the fence reaches the value.
    ThrowFailure(fence->SetEventOnCompletion(1, nullptr));

    // ETW delivery is asynchronous; give the scheduler's completion events time to reach the
    // consumer thread before the session stops the trace.
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    session.End();
    session.RethrowConsumerException();

    if (recorder->ExecutionCompleteCount == 0)
    {
        GTEST_SKIP() << "No GPU-timing executions were captured in this environment; OnCommandListName cannot be exercised.";
    }

    const bool sawNamedCommandList = std::ranges::any_of(
        recorder->CommandListNames,
        [&](const RecordingGpuTimingsCallbacks::CommandListNameEntry& entry) { return entry.Name == CommandListName; });
    EXPECT_TRUE(sawNamedCommandList) << "Expected the named command list to surface via OnCommandListName";
}
