// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "pch.h"

#include "GpuWorkload.h"

namespace
{
    ComPtr<ID3D12Device> CreateDevice(DxTimingCaptureLibraryTest::GpuAdapterKind adapter)
    {
        ComPtr<IDXGIAdapter> warpAdapter;
        if (adapter == DxTimingCaptureLibraryTest::GpuAdapterKind::Warp)
        {
            ComPtr<IDXGIFactory4> factory;
            ThrowFailure(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
            ThrowFailure(factory->EnumWarpAdapter(IID_PPV_ARGS(&warpAdapter)));
        }

        ComPtr<ID3D12Device> device;
        ThrowFailure(D3D12CreateDevice(
            warpAdapter.Get(),
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

namespace DxTimingCaptureLibraryTest
{
    void SubmitGpuWorkAndWait(GpuAdapterKind adapter)
    {
        constexpr UINT64 BufferSizeInBytes = 256;

        ComPtr<ID3D12Device> device = CreateDevice(adapter);

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

        // Buffers are implicitly in (and promoted from) the COMMON state, so a copy
        // between two distinct buffers needs no explicit barriers.
        ComPtr<ID3D12Resource> sourceBuffer = CreateBuffer(device.Get(), BufferSizeInBytes);
        ComPtr<ID3D12Resource> destinationBuffer = CreateBuffer(device.Get(), BufferSizeInBytes);
        commandList->CopyBufferRegion(destinationBuffer.Get(), 0, sourceBuffer.Get(), 0, BufferSizeInBytes);
        ThrowFailure(commandList->Close());

        ID3D12CommandList* commandListsToExecute[] = { commandList.Get() };
        commandQueue->ExecuteCommandLists(1, commandListsToExecute);

        ComPtr<ID3D12Fence> fence;
        ThrowFailure(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
        ThrowFailure(commandQueue->Signal(fence.Get(), 1));
        // A null event handle makes SetEventOnCompletion block until the fence reaches the value.
        ThrowFailure(fence->SetEventOnCompletion(1, nullptr));
    }
}
