// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "pch.h"

#include <PixEventDecoder.h>

#include <DxTimingCaptureLibrary/D3D12EtwEventStructs.h>
#include <DxTimingCaptureLibrary/EventTypes.h>
#include <DxTimingCaptureLibrary/GpuTimingData.h>

#include "D3D12ObjectProcessor.h"

#include "Identifiers.h"

#include "AdapterTracker.h"
#include "MonitorTracker.h"

#include <DxTimingCaptureLibrary/DxTimingCaptureEventHandler.h>
#include <DxTimingCaptureLibrary/QpcTimestampConverter.h>

#include "PerProcessData.h"

#include "DxTimingCaptureProcessor.h"

#ifndef UsePixUtility
// <evntrace.h> declares EventTraceConfigGuid but only defines it when INITGUID
// is active before that header is first included. The library pulls in
// <evntrace.h> (via D3D12Events.h) ahead of <initguid.h>, so the GUID is left
// undefined. In a standalone build, define it here so the library is
// self-contained; in-tree (UsePixUtility) PIX's PixUtility supplies it. Value
// from the Windows SDK <evntrace.h>: {01853a65-418f-4f36-aefc-dc0f1d2fd235}.
EXTERN_C __declspec(selectany) const GUID EventTraceConfigGuid =
    { 0x01853a65, 0x418f, 0x4f36, { 0xae, 0xfc, 0xdc, 0x0f, 0x1d, 0x2f, 0xd2, 0x35 } };
#endif

namespace DirectX::Etw
{

std::unique_ptr<DxTimingCaptureEventHandler> DxTimingCaptureEventHandler::Create(
    DWORD filterToProcessId,
    const DxTimingCaptureLibraryOptions& libraryOptions,
    const DxTimingCaptureEventCallbacks& callbacks)
{
    return std::make_unique<DxTimingCaptureEventHandler>(filterToProcessId, libraryOptions, callbacks, PixEventConfig{}, nullptr);
}

std::unique_ptr<DxTimingCaptureEventHandler> DxTimingCaptureEventHandler::Create(
    DWORD filterToProcessId,
    const DxTimingCaptureLibraryOptions& libraryOptions,
    const DxTimingCaptureEventCallbacks& callbacks,
    const TimestampConverter& timestampConverter,
    const PixEventConfig& pixEventConfig)
{
    return std::make_unique<DxTimingCaptureEventHandler>(filterToProcessId, libraryOptions, callbacks, pixEventConfig, &timestampConverter);
}

DxTimingCaptureEventHandler::DxTimingCaptureEventHandler(
    DWORD filterToProcessId,
    const DxTimingCaptureLibraryOptions& libraryOptions,
    const DxTimingCaptureEventCallbacks& callbacks,
    const PixEventConfig& pixEventConfig,
    const TimestampConverter* timestampConverter)
    : m_ownedTimestampConverter(timestampConverter ? nullptr : std::make_unique<QpcTimestampConverter>(QpcTimestampConverter::ForCurrentMachine()))
    , m_processor(std::make_unique<DxTimingCaptureProcessor>(
        libraryOptions,
        pixEventConfig.MirrorGpuContextEventsToCpu,
        callbacks.ApiObjectCallbacks,
        callbacks.PixCounterCallbacks,
        callbacks.ResidencyEventCallbacks,
        callbacks.PixEventCallbacks,
        callbacks.MonitorEventCallbacks,
        callbacks.GpuTimingsCallbacks,
        callbacks.DxgkObjectCallbacks,
        callbacks.PipelineStateEventCallbacks,
        callbacks.DirectStorageCallbacks,
        callbacks.GpuEngineActivityCallbacks,
        callbacks.DiagnosticsSink,
        callbacks.RuntimeFailureCallbacks,
        timestampConverter ? timestampConverter : m_ownedTimestampConverter.get(),
        filterToProcessId))
    , m_dispatcher(std::make_unique<EtwDispatcher<DxTimingCaptureProcessor>>(m_processor.get()))
    , m_options(libraryOptions)
    , m_mirrorGpuContextEventsToCpu(pixEventConfig.MirrorGpuContextEventsToCpu)
    , m_filterToProcessId(filterToProcessId)
{
}

DxTimingCaptureEventHandler::~DxTimingCaptureEventHandler() = default;

void DxTimingCaptureEventHandler::HandleEventRecord(PEVENT_RECORD record)
{
    if (record->EventHeader.ProviderId == PixEtw::PIX_ETW_PROVIDER_WINDOWS ||
        record->EventHeader.ProviderId == Direct3D12EtwProviderGuid ||
        record->EventHeader.ProviderId == DxgkControlGuid ||
        record->EventHeader.ProviderId == EventTraceConfigGuid ||
        record->EventHeader.ProviderId == DirectStorage::DStorageEtwProvider)
    {
        m_dispatcher->Dispatch(record);
    }
    return;
}

void DxTimingCaptureEventHandler::OnDataComplete()
{
    m_processor->OnComplete();
}

void DxTimingCaptureEventHandler::ReportTraceStatistics(const EVENT_TRACE_LOGFILE& logfile)
{
    m_processor->ReportTraceStatistics(logfile);
}

} // namespace DirectX::Etw