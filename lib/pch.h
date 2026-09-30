// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#pragma once

#include <algorithm>
#include <assert.h>
#include <codecvt>
#include <format>
#include <functional>
#include <locale>
#include <map>
#include <queue>
#include <set>
#include <stdint.h>
#include <sstream>
#include <string>
#include <vector>

#include <d3d12.h>
#include <D3D12Events.h>

#include <Windows.h>
#include <initguid.h>
#include <evntcons.h>

#include <wrl\client.h>
#include <wrl\implements.h>
#include <wrl\wrappers\corewrappers.h>
using Microsoft::WRL::ComPtr;

#include <DxTimingCaptureLibrary/EtwDiagnostics.h>
#include <DxTimingCaptureLibrary/EtwException.h>

using DirectX::Etw::Errors::ThrowToolException;
using DirectX::Etw::Errors::ThrowFailure;
using DirectX::Etw::Errors::ThrowIf;

#include <DxTimingCaptureLibrary/EtwDispatcher.h>

using namespace DirectX::Etw;
