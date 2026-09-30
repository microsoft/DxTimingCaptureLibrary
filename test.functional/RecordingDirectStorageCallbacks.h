// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#pragma once

#include <string>
#include <vector>

#include <DxTimingCaptureLibrary/EtwCallbacks.h>

using namespace DirectX::Etw;

// A DirectStorageCallbacks test double that records every callback, with its parameters, into
// per-kind vectors. Tests drive a real DirectStorage workload through a live ETW session, then
// search these vectors for the specific queue/file/request they created (a process may host
// several DirectStorage queues/files, so position isn't reliable). The library invokes the
// callbacks on the ETW consumer thread; tests read the vectors only after
// SimpleEtwSession::End() has joined that thread, so no locking is needed here.
class RecordingDirectStorageCallbacks : public DirectStorageCallbacks
{
public:
    struct FileRecord
    {
        UINT16 FileId;
        INT64 Timestamp;
        std::wstring Path;
    };

    struct QueueRecord
    {
        UINT16 QueueId;
        INT64 Timestamp;
        std::string Name;
    };

    std::vector<FileRecord> Files;
    std::vector<QueueRecord> Queues;
    std::vector<ReadRequest> Reads;
    std::vector<StatusNotification> Statuses;
    std::vector<FenceSignal> Fences;
    std::vector<SetEventNotification> SetEvents;
    std::vector<Submit> Submits;

    HRESULT OnDirectStorageFile(UINT16 fileId, INT64 timestamp, std::wstring_view path) override
    {
        Files.push_back({ fileId, timestamp, std::wstring{ path } });
        return S_OK;
    }

    HRESULT OnDirectStorageQueue(UINT16 queueId, INT64 timestamp, std::string_view name) override
    {
        Queues.push_back({ queueId, timestamp, std::string{ name } });
        return S_OK;
    }

    HRESULT OnDirectStorageReadRequest(const ReadRequest& request) override
    {
        Reads.push_back(request);
        return S_OK;
    }

    HRESULT OnDirectStorageStatus(const StatusNotification& status) override
    {
        Statuses.push_back(status);
        return S_OK;
    }

    HRESULT OnDirectStorageFenceSignal(const FenceSignal& signal) override
    {
        Fences.push_back(signal);
        return S_OK;
    }

    HRESULT OnDirectStorageSetEvent(const SetEventNotification& setEvent) override
    {
        SetEvents.push_back(setEvent);
        return S_OK;
    }

    HRESULT OnDirectStorageSubmit(const Submit& submit) override
    {
        Submits.push_back(submit);
        return S_OK;
    }
};
