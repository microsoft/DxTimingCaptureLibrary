// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#pragma once

#include <cstddef>
#include <exception>
#include <string>
#include <thread>
#include <vector>

// Include ApiObjectCallbacks's full definition so `std::unique_ptr<ApiObjectCallbacks>` works
// under clang-cl (clang's STL eagerly instantiates the deleter, which requires a complete type).
#include <DxTimingCaptureLibrary/EtwCallbacks.h>
#include <DxTimingCaptureLibrary/EtwDiagnostics.h>
#include <DxTimingCaptureLibrary/DxTimingCaptureEventHandler.h>

using namespace DirectX::Etw;

struct _EVENT_RECORD;

// A diagnostic the library reported through OnDiagnostic during a session,
// retained so a test can assert on what the library surfaced.
struct SessionDiagnostic
{
    DiagnosticSeverity Severity;
    DiagnosticCode Code;
    std::wstring Message;
};

// Drives a real-time ETW session end to end for the functional tests: it starts a
// trace, enables the D3D12 and DXGK providers, pumps event records into a
// DxTimingCaptureEventHandler on a consumer thread, and (on End) flushes deferred entries and
// records ETW data-loss totals. State is per-instance (no globals), so each test owns
// an isolated session.
class SimpleEtwSession : public DiagnosticsSink
{
    std::thread m_thread;
    bool m_running = false;

    std::wstring m_sessionName;
    unsigned long long m_sessionHandle = 0; // TRACEHANDLE, kept opaque in the header

    std::unique_ptr<ApiObjectCallbacks> m_apiObjectCallbacks;
    std::unique_ptr<RuntimeFailureCallbacks> m_runtimeFailureCallbacks;
    std::unique_ptr<GpuTimingsCallbacks> m_gpuTimingsCallbacks;
    std::unique_ptr<GpuEngineActivityCallbacks> m_gpuEngineActivityCallbacks;
    std::unique_ptr<DirectStorageCallbacks> m_directStorageCallbacks;

    std::unique_ptr<DxTimingCaptureEventHandler> m_handler;

    std::exception_ptr m_consumerException;
    std::vector<SessionDiagnostic> m_diagnostics;

    void ConsumerThread();

public:
    // Routes a single ETW record to the handler, capturing the first exception that
    // escapes (see m_consumerException). Public only so the C-style ETW callback can
    // reach it; not intended to be called directly by tests.
    void OnEventRecord(_EVENT_RECORD* record);

    ~SimpleEtwSession()
    {
        if (m_running)
        {
            End();
        }
    }

    // Supplies the ApiObjectCallbacks the handler will drive. Optional: if not set,
    // the session installs a no-op implementation.
    void SetApiObjectCallbacks(std::unique_ptr<ApiObjectCallbacks> callbacks)
    {
        m_apiObjectCallbacks = std::move(callbacks);
    }

    // Supplies the RuntimeFailureCallbacks the handler will drive. Optional: if not
    // set, the handler receives null and journal-entry events are ignored.
    void SetRuntimeFailureCallbacks(std::unique_ptr<RuntimeFailureCallbacks> callbacks)
    {
        m_runtimeFailureCallbacks = std::move(callbacks);
    }

    // Supplies the GpuTimingsCallbacks the handler will drive. Optional: if not set,
    // the session installs a no-op implementation and leaves GPU-timing capture off.
    // Setting it turns on TrackGpuTiming for this session (see Begin).
    void SetGpuTimingsCallbacks(std::unique_ptr<GpuTimingsCallbacks> callbacks)
    {
        m_gpuTimingsCallbacks = std::move(callbacks);
    }

    // Supplies the DirectStorageCallbacks the handler will drive. Optional: if not set, the
    // session installs a no-op implementation.
    void SetDirectStorageCallbacks(std::unique_ptr<DirectStorageCallbacks> callbacks)
    {
        m_directStorageCallbacks = std::move(callbacks);
    }

    // Supplies the GpuEngineActivityCallbacks the handler will drive. Optional: if not
    // set, the session installs a no-op implementation and leaves packet tracking off.
    // Setting it turns on TrackGpuEngineActivity for this session (see Begin).
    void SetGpuEngineActivityCallbacks(std::unique_ptr<GpuEngineActivityCallbacks> callbacks)
    {
        m_gpuEngineActivityCallbacks = std::move(callbacks);
    }

    void Begin();

    // Stops the trace, joins the consumer thread, flushes any deferred entries
    // (OnDataComplete) and records the final data-loss totals. After End returns the
    // consumer thread has joined, so every callback has run and the recorded data is
    // safe to read without further synchronization.
    void End();

    // DiagnosticsSink: the library calls this on the consumer thread as it reports
    // problems (lost events, clock issues, ...), exactly as it does in production.
    void OnDiagnostic(DiagnosticSeverity severity, DiagnosticCode code, std::wstring_view message) override;

    // Number of diagnostics the library reported with the given code. Tests use
    // this to assert, e.g., that no EventsLost/BuffersLost diagnostic fired.
    size_t CountDiagnostics(DiagnosticCode code) const;

    // Rethrows the first exception (if any) that propagated out of the handler on
    // the consumer thread, so a test can assert on the library's failure behavior.
    // No-op if the consumer thread completed cleanly.
    void RethrowConsumerException() const
    {
        if (m_consumerException)
        {
            std::rethrow_exception(m_consumerException);
        }
    }

    bool HadConsumerException() const
    {
        return static_cast<bool>(m_consumerException);
    }
};
