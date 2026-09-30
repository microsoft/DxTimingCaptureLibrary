// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#pragma once

#include <perfetto.h>

// A single track-event category; everything this sample emits uses it.
PERFETTO_DEFINE_CATEGORIES(
    perfetto::Category("dxtimingcapture").SetDescription("DirectX 12 ETW events"));
