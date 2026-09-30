// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "pch.h"

#include <memory>

// MarkerOp.h (pulled in transitively) expands the D3D12_MARKER_API_* constants
// from the D3D12 ETW manifest headers, so those must be included first.
#include <d3d12.h>
#include <D3D12Events.h>

#include <DxTimingCaptureLibrary/DxTimingCaptureEventHandler.h>

using namespace DirectX::Etw;

#include "NoOpCallbacks.h"

namespace
{
    // Owns a full set of no-op callbacks and hands out a DxTimingCaptureEventCallbacks that
    // borrows them. Keep the instance alive as long as any handler built from Config().
    struct FullCallbacks
    {
        NoOpApiObjectCallbacks apiObject;
        NoOpPixCounterCallbacks pixCounter;
        NoOpResidencyEventCallbacks residency;
        NoOpPixEventCallbacks pixEvent;
        NoOpMonitorEventCallbacks monitor;
        NoOpGpuTimingsCallbacks gpuTimings;
        NoOpDxgkObjectCallbacks dxgkObject;
        NoOpPipelineStateEventCallbacks pipelineState;
        NoOpDirectStorageCallbacks directStorage;
        NoOpGpuEngineActivityCallbacks gpuEngineActivity;
        NullDiagnosticsSink diagnosticsSink;
        NoOpRuntimeFailureCallbacks runtimeFailure;

        DxTimingCaptureEventCallbacks Config()
        {
            DxTimingCaptureEventCallbacks callbacks;
            callbacks.ApiObjectCallbacks = &apiObject;
            callbacks.PixCounterCallbacks = &pixCounter;
            callbacks.ResidencyEventCallbacks = &residency;
            callbacks.PixEventCallbacks = &pixEvent;
            callbacks.MonitorEventCallbacks = &monitor;
            callbacks.GpuTimingsCallbacks = &gpuTimings;
            callbacks.DxgkObjectCallbacks = &dxgkObject;
            callbacks.PipelineStateEventCallbacks = &pipelineState;
            callbacks.DirectStorageCallbacks = &directStorage;
            callbacks.GpuEngineActivityCallbacks = &gpuEngineActivity;
            callbacks.DiagnosticsSink = &diagnosticsSink;
            callbacks.RuntimeFailureCallbacks = &runtimeFailure;
            return callbacks;
        }
    };
}

// Filling in every slot works.
TEST(ConfigContract, FullyPopulatedSucceeds)
{
    NoOpTimestampConverter converter;
    FullCallbacks callbacks;
    EXPECT_NO_THROW({
        auto handler = DxTimingCaptureEventHandler::Create(1234u, {}, callbacks.Config(), converter);
        EXPECT_NE(handler, nullptr);
    });
}

// Every callback is optional: passing an empty callbacks bundle must still
// succeed, with the library filling in no-ops.
TEST(ConfigContract, NoConfigSucceeds)
{
    NoOpTimestampConverter converter;
    EXPECT_NO_THROW({
        auto handler = DxTimingCaptureEventHandler::Create(1234u, {}, {}, converter);
        EXPECT_NE(handler, nullptr);
    });
}
