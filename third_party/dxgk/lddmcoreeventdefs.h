// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
//
// Vendored DirectX graphics-kernel (DXGK / LDDMCore) ETW values used by DxTimingCaptureLibrary: the
// provider GUID, the keyword (log-flag) bits, and the event ids the dispatcher
// switches on. These are defined by the DXGK ETW manifest LDDMCore.man, which
// ships with GPUView in the Windows Performance Toolkit. Only the values
// DxTimingCaptureLibrary references are listed here; see LDDMCore.man for the rest.

#pragma once

#include <wmistr.h>
#include <evntrace.h>
#include <evntprov.h>

// Microsoft-Windows-DxgKrnl provider GUID {802ec45a-1e99-4b83-9920-87c98277ba9d}.
EXTERN_C __declspec(selectany) const GUID DxgkControlGuid =
    { 0x802ec45a, 0x1e99, 0x4b83, { 0x99, 0x20, 0x87, 0xc9, 0x82, 0x77, 0xba, 0x9d } };

// Keyword (log-flag) bits the session enables.
#define DXGK_KEYWORD_LOG_FLAGS_BASE                    0x1
#define DXGK_KEYWORD_LOG_FLAGS_PROFILER               0x2
#define DXGK_KEYWORD_LOG_FLAGS_ALLOCATIONS_REFERENCES 0x4
#define DXGK_KEYWORD_LOG_FLAGS_RESOURCE               0x40
#define DXGK_KEYWORD_LOG_FLAGS_LONG_HAUL              0x800
#define DXGK_KEYWORD_LOG_FLAGS_HISTORY_BUFFER         0x4000

// Event ids the dispatcher switches on, grouped as in EtwDispatcher.h.
// Devices / contexts
#define EventVSyncDPC_value                            0x11
#define EventCreateDevice_value                        0x1b
#define EventDestroyDevice_value                       0x1c
#define EventReportDevice_value                        0x1d
#define EventCreateContext_value                       0x1e
#define EventDestroyContext_value                      0x1f
#define EventReportContext_value                       0x20
// Allocations (adapter + device)
#define EventCreateAdapterAllocation_value             0x21
#define EventDestroyAdapterAllocation_value            0x22
#define EventReportAdapterAllocation_value             0x23
#define EventCreateDeviceAllocation_value              0x24
#define EventDestroyDeviceAllocation_value             0x25
#define EventReportDeviceAllocation_value              0x26
#define EventMigrateAllocation_value                   0x30
#define EventCompleteAllocationMigration_value         0x31
// Paging / segments
#define EventPagingOpMapApertureSegment_value          0x3a
#define EventReportSegment_value                       0x4e
#define EventPagingOpVirtualTransfer_value             0x132
#define EventPagingOpVirtualFill_value                 0x133
#define EventPagingOpSysmemCommit_value                0x139
// Adapter metadata / clock / history buffer
#define EventDpiReportAdapter_value                    0x6e
#define EventNodeMetadata_value                        0xfa
#define EventCalibrateGpuClock_value                   0x106
#define EventHistoryBuffer_value                       0x107
// Residency / VidMm
#define EventVidMmMakeResident_value                   0x140
#define EventVidMmEvict_value                          0x141
#define EventVidMmProcessBudgetChange_value            0x16e
#define EventVidMmProcessUsageChange_value             0x16f
#define EventVidMmProcessDemotedCommitmentChange_value 0x172
#define EventVidMmProcessCommitmentChange_value        0x173
// Hardware queues (HW scheduling)
#define EventCreateHwQueue_value                       0x1a6
#define EventDestroyHwQueue_value                      0x1a7
#define EventReportHwQueue_value                       0x1a8
// DMA packets
#define EventDmaSubmit_value                           0xaf
#define EventDmaIsrComplete_value                      0xb1
#define EventDmaReleaseToGpu_value                     0x1c2
#define EventDmaCompleteByGpu_value                    0x1c3
