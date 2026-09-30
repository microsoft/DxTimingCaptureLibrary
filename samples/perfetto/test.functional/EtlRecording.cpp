// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "pch.h"

#include "EtlRecording.h"

#include <DxTimingCaptureLibrary/DxTimingCaptureLibrary.h>

// The sample's own session configuration, so a recorded trace holds exactly the
// events a live run would have seen.
#include "EtwConfig.h"

#include "SampleProcess.h"

namespace
{
    class FileModeSession
    {
        TRACEHANDLE m_handle = 0;
        std::wstring m_name;

    public:
        FileModeSession(const std::wstring& name, const std::filesystem::path& path)
            : m_name(name)
        {
            // A session left over from an earlier run would make StartTrace fail.
            EventTraceProperties stopProperties{};
            ControlTraceW(0, m_name.c_str(), stopProperties, EVENT_TRACE_CONTROL_STOP);

            EventTraceProperties properties{};
            properties->LogFileMode = EVENT_TRACE_FILE_MODE_SEQUENTIAL;
            properties->LogFileNameOffset = FIELD_OFFSET(EventTraceProperties, LogFileName);
            wcscpy_s(properties.LogFileName, path.c_str());

            if (StartTraceW(&m_handle, m_name.c_str(), properties) != ERROR_SUCCESS)
            {
                m_handle = 0;
            }
        }

        ~FileModeSession()
        {
            Stop();
        }

        FileModeSession(const FileModeSession&) = delete;
        FileModeSession& operator=(const FileModeSession&) = delete;

        bool Started() const { return m_handle != 0; }
        TRACEHANDLE Handle() const { return m_handle; }

        // Valid after Stop().
        unsigned int EventsLost() const { return m_eventsLost; }

        void Stop()
        {
            if (m_handle != 0)
            {
                EventTraceProperties properties{};
                if (ControlTraceW(m_handle, m_name.c_str(), properties, EVENT_TRACE_CONTROL_STOP) == ERROR_SUCCESS)
                {
                    m_eventsLost = properties->EventsLost + properties->LogBuffersLost;
                }
                m_handle = 0;
            }
        }

    private:
        unsigned int m_eventsLost = 0;
    };
}

namespace DxTimingCaptureLibraryTest
{
    EtlRecording RecordGpuWorkToEtl(const std::filesystem::path& path, unsigned int seconds)
    {
        EtlRecording recording;
        recording.Path = path;
        recording.Seconds = seconds;

        std::error_code errorCode;
        std::filesystem::remove(path, errorCode);

        const std::wstring sessionName = L"DxTimingCaptureLibrary.sample.perfetto.test.recording" + std::to_wstring(GetCurrentProcessId());
        FileModeSession session(sessionName, path);
        if (!session.Started())
        {
            return recording;
        }

        // Everything on, so one recording can be replayed in any of the sample's modes.
        EnableD3D12Provider(session.Handle(), /*captureGpuMemoryUsage*/ true);
        EnableDxgkProvider(session.Handle(), /*captureGpuTiming*/ true, /*captureGpuMemoryUsage*/ true);

        {
            // Both devices are created inside the recording, which is what lets their
            // packets be attributed back to them.
            GpuWorkChildProcess firstChild(seconds);
            GpuWorkChildProcess secondChild(seconds);
            recording.FirstChildProcessId = firstChild.ProcessId();
            recording.SecondChildProcessId = secondChild.ProcessId();

            firstChild.WaitForSuccess();
            secondChild.WaitForSuccess();
        }

        session.Stop();

        recording.EventsLost = session.EventsLost();
        recording.Succeeded = std::filesystem::exists(path) && std::filesystem::file_size(path) > 0;
        return recording;
    }
}
