These are the public headers of the WinPixEventRuntime event decoder, copied
from https://github.com/microsoft/pixevents (decoder/include).

Only the headers are vendored here, not the decoder implementation:

  - DecodedPixEventTypes.h - the decoded-event types (PixEventType, PixCpuEvent,
    DecodedPixEventBlock, ...). PixEventType and PixCpuEvent are part of
    DxTimingCaptureLibrary's public API (see include/DxTimingCaptureLibrary/Types.h and
    EtwCallbacks.h), so this header is required by consumers regardless.

  - PixEventDecoder.h - declares PixEventDecoder::DecodeTimingBlock, the single
    decoder entry point DxTimingCaptureLibrary calls.

DxTimingCaptureLibrary does NOT link the decoder implementation (PixEventDecoder.lib).
DecodeTimingBlock is stubbed in lib/PixEventDecoderStub.cpp, so PIX
timing/marker events are dropped. To restore real PIX decoding, replace that
stub and link the genuine PixEventDecoder.lib (or vendor the decoder sources
and build them, the same way third_party/googletest is built).

Both headers are MIT-licensed. If you update them, update cgmanifest.json (in
the repository root) accordingly.
