// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#pragma once

#include <DxTimingCaptureLibrary/DxgkEtwEventStructs.h>
#include <DxTimingCaptureLibrary/GpuPacketTracker.h>
#include <DxTimingCaptureLibrary/GpuTimingData.h>

namespace DirectX::Etw
{

inline std::optional<GpuPacketOwner> MakeGpuPacketOwner(DxgkContexts::UmContext const* umContext)
{
    if (umContext == nullptr)
    {
        return std::nullopt;
    }

    return GpuPacketOwner{ umContext->ProcessId, umContext->Adapter, umContext->NodeOrdinal, umContext->EngineAffinity };
}

// The hardware-scheduled pair, raised only by adapters running with hardware
// scheduling enabled. Note the completion is the scheduler retiring the packet
// after seeing its progress fence signalled, batched with whatever else that DPC
// retires, so it lands somewhat after the GPU actually finished.

inline void OnDmaReleaseToGpu(GpuPacketTracker& tracker, DxgkContexts const& contexts, uint64_t releaseQpc, DxgkDmaReleaseToGpuArgs args)
{
    tracker.NoteEventTimestamp(releaseQpc);

    // The event is written whether or not the submit succeeded, and a failed
    // hand-off never runs on the GPU.
    if (static_cast<int32_t>(args.ntStatus) < 0)
    {
        tracker.ExpectOrphanCompletion(GpuPacketSource::HardwareQueue, args.hHwQueue, args.progressFenceValue, releaseQpc);
        return;
    }

    DxgkContexts::UmContext const* umContext = nullptr;

    // "hHwQueue" here is the DXG kernel object pointer, the same value space as
    // DxgkHwQueueArgs::hwQueue - not the OS handle that shares the name in
    // DxgkHwQueueArgs::hwQueueHandle.
    if (auto hwQueue = contexts.TryGetHwQueueByQueueObject(args.hHwQueue))
    {
        umContext = &hwQueue->ParentUmContext;
    }

    tracker.Open(
        GpuPacketSource::HardwareQueue,
        args.hHwQueue,
        args.progressFenceValue,
        releaseQpc,
        MakeGpuPacketOwner(umContext));
}

inline std::optional<GpuPacketSpan> OnDmaCompleteByGpu(GpuPacketTracker& tracker, uint64_t endQpc, DxgkDmaCompleteByGpuArgs args)
{
    tracker.NoteEventTimestamp(endQpc);

    return tracker.Close(GpuPacketSource::HardwareQueue, args.hHwQueue, args.progressFenceValue, endQpc);
}

// The legacy scheduler's equivalents. These identify work by DXG context pointer
// - the same value space as DxgkContextArgs::context - so attribution goes
// through the context map instead. The scheduler's own system and companion
// contexts have no DXG object and report an internal pointer, so a lookup can
// still miss.
//
// DmaDpcComplete also fires for these packets, but it only trails the interrupt
// we already acted on, so we ignore it - as does xperf.

inline void OnDmaSubmit(GpuPacketTracker& tracker, DxgkContexts const& contexts, uint64_t submitQpc, DxgkDmaSubmitArgs args)
{
    tracker.NoteEventTimestamp(submitQpc);

    tracker.Open(
        GpuPacketSource::LegacyContext,
        args.hContext,
        args.submissionId,
        submitQpc,
        MakeGpuPacketOwner(contexts.TryGetUmContextForTimingContext(args.hContext)));
}

inline std::optional<GpuPacketSpan> OnDmaIsrComplete(GpuPacketTracker& tracker, uint64_t endQpc, DxgkDmaIsrCompleteArgs args)
{
    tracker.NoteEventTimestamp(endQpc);

    // Preemption and page faults raise this too; all three mean the packet
    // stopped running. completionId and submissionId are the same packet fence,
    // so they pair directly.
    return tracker.Close(GpuPacketSource::LegacyContext, args.hContext, args.completionId, endQpc);
}

} // namespace DirectX::Etw
