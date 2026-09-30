// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#pragma once

#include <windows.h>
#include <evntrace.h>
#include <evntcons.h>

#include <d3d12.h>
#include <dxgi1_4.h>

#include <cassert>
#include <cstdint>
#include <format>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <DxTimingCaptureLibrary/EtwDispatcher.h>
#include <DxTimingCaptureLibrary/GpuTimingData.h>
#include <DxTimingCaptureLibrary/MarkerOp.h>

#include "CsvTableWriter.h"
#include "ProcessEtlFile.h"
#include "PidFinderProcessor.h"
#include "AllocationTracker.h"
#include "HistoryBufferProcessor.h"
