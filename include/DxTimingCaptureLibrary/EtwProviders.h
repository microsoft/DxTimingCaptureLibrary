// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#pragma once

// Provider GUIDs and keyword flags for the ETW providers the library decodes.
// Include this where you configure your ETW session (EnableTraceEx2); the event
// handler itself does not need it. Requires the Agility SDK include path (for
// D3D12Events.h) plus third_party/dxgk and third_party/directstorage.

#include <windows.h>
#include <evntrace.h>
#include <evntprov.h>

#include <D3D12Events.h>
#include <lddmcoreeventdefs.h>
#include <dstorage_etw.h>
#include <DxTimingCaptureLibrary/PixEtwManifest.h>
