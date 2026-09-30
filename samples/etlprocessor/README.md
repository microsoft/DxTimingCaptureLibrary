# DxTimingCaptureLibrary ETL processor sample

This sample reads a **saved ETW trace** (`.etl`) offline and reports on the GPU
work recorded in it, using DxTimingCaptureLibrary's `EtwDispatcher` to decode D3D12 and
dxgkrnl events.

This sample processes a trace that was captured earlier, complementing other
samples (e.g. `basic` and `perfetto`) which consume a real-time session. It also
works with more raw building blocks from DxTimingCaptureLibrary: it drives `EtwDispatcher`
with per-event handlers and correlates the events itself, instead of consuming
the higher-level typed event callbacks that the `basic` sample uses.

The sample may be helpful to developers who are debugging correctness issues with
"history buffers" (GPU timing data) in D3D12 drivers.

## What it does

Point it at an `.etl` and pick an action:

| Action | Description |
| --- | --- |
| `dumppids` | List every process that created a D3D12 device in the trace. |
| `dumphistorybuffers` | Reconstruct GPU submissions and print their ToP/EoP timings as a CSV table. |
| `dumpallocations` | Track a specific class of dxgkrnl allocations (those tagged with the `0x4000` flag) and report how many were still outstanding when the trace ended. |

Each action is driven by a small processor class
(`PidFinderProcessor`, `HistoryBufferProcessor`, `AllocationTracker`) that
receives decoded events from `EtwDispatcher<Processor>`. They are a good starting
point for writing your own trace analysis.

## Usage

```
DxTimingCaptureLibrary.EtlProcessor --etl <path> --action <action> [options]
```

Options:

- `--pid <id>` — restrict `dumphistorybuffers` to a single process (`-1` means
  all processes, which is the default).
- `--nohighlow` — print each timestamp as one 64-bit value instead of separate
  low/high columns.
- `--help` — show usage.

Examples:

```
DxTimingCaptureLibrary.EtlProcessor --etl capture.etl --action dumppids
DxTimingCaptureLibrary.EtlProcessor --etl capture.etl --action dumphistorybuffers --pid 1234
DxTimingCaptureLibrary.EtlProcessor --etl capture.etl --action dumpallocations
```

To capture a trace to feed this sample, run the `basic` sample (or any ETW
session that enables the D3D12 and dxgkrnl providers) with real-time mode
swapped for a file-mode session.

## Building

The project builds as part of the DxTimingCaptureLibrary solution and references
`DxTimingCaptureLibrary.lib`.
