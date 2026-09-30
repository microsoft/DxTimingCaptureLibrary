// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#pragma once

#include <memory>

#include <windows.h>
#include <evntrace.h>
#include <evntcons.h>

#include <DxTimingCaptureLibrary/EtwCallbacks.h>
#include <DxTimingCaptureLibrary/EtwDiagnostics.h>

namespace DirectX::Etw
{

class DxTimingCaptureProcessor;
template <typename T> class EtwDispatcher;

// DxTimingCaptureEventCallbacks: callbacks you implement. The library will dispatch to your callbacks when relevant
// events occur. Leaving a callback as nullptr is fine - the library will replace it with a barebones impl.
// Supplied callbacks must outlive the handler. One object could implement multiple callbacks.
struct DxTimingCaptureEventCallbacks
{
    ApiObjectCallbacks* ApiObjectCallbacks = nullptr;
    PixCounterCallbacks* PixCounterCallbacks = nullptr;
    ResidencyEventCallbacks* ResidencyEventCallbacks = nullptr;
    PixEventCallbacks* PixEventCallbacks = nullptr;
    MonitorEventCallbacks* MonitorEventCallbacks = nullptr;
    GpuTimingsCallbacks* GpuTimingsCallbacks = nullptr;
    DxgkObjectCallbacks* DxgkObjectCallbacks = nullptr;
    PipelineStateEventCallbacks* PipelineStateEventCallbacks = nullptr;
    DirectStorageCallbacks* DirectStorageCallbacks = nullptr;
    DiagnosticsSink* DiagnosticsSink = nullptr;
    RuntimeFailureCallbacks* RuntimeFailureCallbacks = nullptr;
    GpuEngineActivityCallbacks* GpuEngineActivityCallbacks = nullptr;
};

// Filters to help make the ETW processing code a bit more efficient. Try to make these match the callbacks
// you set in DxTimingCaptureEventCallbacks, but setting them to true will give you maximal data.
struct DxTimingCaptureLibraryOptions
{
    // Track D3D12 API-object lifetimes (resources, heaps, command queues,
    // descriptor heaps, command allocators, pipeline states, object names) and
    // their GPU residency/memory activity.
    bool TrackApiObjects = false;

    // Track + process GPU timing data that comes from the driver (via history buffers).
    // Do additional tracking to provide context/data, not just the raw timing data.
    bool TrackGpuTiming = false;

    // Pair up DMA packet submit/complete events into per-engine spans, for every
    // process on the machine. Attribution comes from the DXGK device, context and
    // hardware queue handlers, which always run, so this works on its own.
    bool TrackGpuEngineActivity = false;
};

// TimestampConverter: Optional interface to convert raw timestamps carried by ETW event records
// into nanosecond values that the library will pass into your callbacks.
// 
// The library will use a reasonable default (QpcTimestampConverter) if you don't provide your
// own impl. That should be fine for most live situations, but probably not for offline .etl file
// processing - you'll probably want to implement your own TimestampConverter for that.
//
// IMPORTANT: the consumer must open the ETW session in raw-QPC mode -
// PROCESS_TRACE_MODE_RAW_TIMESTAMP on the trace, and Wnode.ClientContext = 1
// (QPC) on the session. In that mode EVENT_RECORD::EventHeader.TimeStamp is a
// QPC tick count, which is what ConvertClockToTimeStamp receives. Without
// raw-QPC mode ProcessTrace delivers FILETIME-based timestamps and the
// conversion overflows / produces garbage.
class TimestampConverter
{
public:
    virtual ~TimestampConverter() = default;

    // Timestamp (in nanoseconds) of the most recently processed event, or 0 if
    // none has been processed yet.
    virtual INT64 GetLastEventTimeStamp() const = 0;

    // Converts a raw QPC tick count (EventHeader.TimeStamp in raw-QPC mode) to a
    // nanosecond timestamp on the library's timeline.
    virtual INT64 ConvertClockToTimeStamp(INT64 count) const = 0;

    // The QPC frequency in ticks per second (QueryPerformanceFrequency).
    virtual INT64 GetHighPerformanceFrequency() const = 0;
};


// PixEventConfig: Optional interface to configure how PIX events are decoded. You probably don't need to set this.
struct PixEventConfig
{
    // When true, GPU-context begin/end events also show on the CPU thread that
    // emitted them (legacy behavior); when false they appear only on GPU lanes.
    bool MirrorGpuContextEventsToCpu = false;
};

// DxTimingCaptureEventHandler: the object you create to handle ETW events. You own the ETW
// session and feed it records; the handler decodes them and dispatches to your callbacks.
class DxTimingCaptureEventHandler
{
    std::unique_ptr<TimestampConverter> m_ownedTimestampConverter;
    std::unique_ptr<DxTimingCaptureProcessor> m_processor;
    std::unique_ptr<EtwDispatcher<DxTimingCaptureProcessor>> m_dispatcher;

    DxTimingCaptureLibraryOptions m_options;
    bool m_mirrorGpuContextEventsToCpu;
    DWORD m_filterToProcessId;

public:
    static std::unique_ptr<DxTimingCaptureEventHandler> Create(
        DWORD filterToProcessId,
        const DxTimingCaptureLibraryOptions& libraryOptions,
        const DxTimingCaptureEventCallbacks& callbacks);

    static std::unique_ptr<DxTimingCaptureEventHandler> Create(
        DWORD filterToProcessId,
        const DxTimingCaptureLibraryOptions& libraryOptions,
        const DxTimingCaptureEventCallbacks& callbacks,
        const TimestampConverter& timestampConverter,
        const PixEventConfig& pixEventConfig = {});

    // Prefer to use Create() but using the constructor will work.
    DxTimingCaptureEventHandler(
        DWORD filterToProcessId,
        const DxTimingCaptureLibraryOptions& libraryOptions,
        const DxTimingCaptureEventCallbacks& callbacks,
        const PixEventConfig& pixEventConfig,
        const TimestampConverter* timestampConverter);

    virtual ~DxTimingCaptureEventHandler();

    DxTimingCaptureEventHandler(const DxTimingCaptureEventHandler&) = delete;
    DxTimingCaptureEventHandler& operator=(const DxTimingCaptureEventHandler&) = delete;

    // Decodes and correlates one ETW event record, invoking the relevant
    // callbacks. Call it from your consumer's EventRecordCallback for every
    // delivered record; records from providers the library doesn't handle are
    // ignored.
    void HandleEventRecord(PEVENT_RECORD record);

    // Flushes any remaining correlation state, invoking the callbacks for data
    // that was still pending.
    void OnDataComplete();

    // Records ETW data-loss totals for the trace, surfacing any newly observed
    // loss to the diagnostics sink.
    void ReportTraceStatistics(const EVENT_TRACE_LOGFILE& logfile);
};

} // namespace DirectX::Etw
