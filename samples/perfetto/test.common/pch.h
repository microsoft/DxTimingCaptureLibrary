// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#pragma once

#include <Windows.h>

#include <d3d12.h>
#include <dxgi1_4.h>

#include <wrl/client.h>
using Microsoft::WRL::ComPtr;

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#include <DxTimingCaptureLibrary/EtwException.h>
using DirectX::Etw::Errors::ThrowToolException;
using DirectX::Etw::Errors::ThrowFailure;
using DirectX::Etw::Errors::ThrowIf;

#include <gtest/gtest.h>
