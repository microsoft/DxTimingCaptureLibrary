// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "pch.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#include <DxTimingCaptureLibrary/DxTimingCaptureEventHandler.h>
#include "EtwSession.h"

using namespace DirectX::Etw;

// Live-device test for the runtime-failure journal path (EventD3D12JournalEntry).
// It drives a real D3D12 device into a recording-time failure that removes a
// command list, which the runtime records in its failure journal and emits over
// ETW, and confirms the entry reaches the consumer callback. The deterministic
// decode of the payload is covered by the unit tests (RuntimeFailureTests.cpp).

namespace
{
    class RecordingRuntimeFailureCallbacks : public RuntimeFailureCallbacks
    {
    public:
        struct Entry
        {
            INT64 Timestamp;
            UINT32 Index;
            UINT32 Code;
            UINT32 ThreadId;
            std::string Message;
        };

        std::vector<Entry> Entries;

        HRESULT OnD3D12JournalEntry(INT64 timestamp, UINT32 index, UINT32 code, UINT32 threadId, std::string_view message) override
        {
            Entries.push_back({ timestamp, index, code, threadId, std::string(message) });
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
}

TEST(JournalEntryTests, CommandListRemovalIsJournaled)
{
    auto callbacks = std::make_unique<RecordingRuntimeFailureCallbacks>();
    auto* recorder = callbacks.get();

    SimpleEtwSession session;
    session.SetRuntimeFailureCallbacks(std::move(callbacks));
    session.Begin();

    ComPtr<ID3D12Device> device = CreateDevice();

    // A single-slot occlusion query heap.
    D3D12_QUERY_HEAP_DESC queryHeapDesc{};
    queryHeapDesc.Type = D3D12_QUERY_HEAP_TYPE_OCCLUSION;
    queryHeapDesc.Count = 1;
    ComPtr<ID3D12QueryHeap> queryHeap;
    ThrowFailure(device->CreateQueryHeap(&queryHeapDesc, IID_PPV_ARGS(&queryHeap)));

    ComPtr<ID3D12CommandAllocator> commandAllocator;
    ThrowFailure(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&commandAllocator)));

    ComPtr<ID3D12GraphicsCommandList> commandList;
    ThrowFailure(device->CreateCommandList(
        0,
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        commandAllocator.Get(),
        nullptr,
        IID_PPV_ARGS(&commandList)));

    // Begin an occlusion query but never end it. Closing a command list with an
    // open query is a recording-time error the runtime resolves by removing the
    // command list, which records a journal entry. Close() reports the failure as an
    // HRESULT, so do not throw on it.
    commandList->BeginQuery(queryHeap.Get(), D3D12_QUERY_TYPE_OCCLUSION, 0);
    HRESULT closeResult = commandList->Close();
    (void)closeResult;

    session.End();
    session.RethrowConsumerException();

    ASSERT_FALSE(recorder->Entries.empty()) << "Expected the command-list removal to be journaled";

    // The failure is journaled on the thread that hit it (our thread, during Close)
    // with a failing HRESULT/NTSTATUS (high bit set).
    const bool hasFailureOnThisThread = std::ranges::any_of(recorder->Entries, [](const RecordingRuntimeFailureCallbacks::Entry& entry)
    {
        return (entry.Code & 0x80000000u) != 0 && entry.ThreadId == GetCurrentThreadId();
    });
    EXPECT_TRUE(hasFailureOnThisThread) << "Expected a failing journal entry recorded on the calling thread";
}
