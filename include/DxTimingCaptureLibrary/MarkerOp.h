// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#pragma once
// This is a custom header that's based on data in D3D12MarkerApiEnums.idl.

namespace DirectX::Etw
{
#define ETW_MANIFEST_MARKER_VALUE( ID ) (ID - MSG_Map_D3D12_MARKER_API_SETMARKER)

// Render ops which exist on multiple command list types and insert markers on all applicable command lists
#define FOR_EACH_RENDER_OP_COMMON_API(macro) \
    macro(SetMarker                     , SETMARKER )                 \
    macro(BeginEvent                    , BEGINEVENT )                \
    macro(EndEvent                      , ENDEVENT )                  \
    macro(ResolveQueryData              , RESOLVEQUERYDATA )          \

// Render ops which exist on graphics command lists
#define FOR_EACH_GRAPHICS_RENDER_OP_API(macro) \
    macro(DrawInstanced                 , DRAWINSTANCED )               \
    macro(DrawIndexedInstanced          , DRAWINDEXEDINSTANCED )        \
    macro(ExecuteIndirect               , EXECUTEINDIRECT )             \
    macro(Dispatch                      , DISPATCH )                    \
    macro(CopyBufferRegion              , COPYBUFFERREGION )            \
    macro(CopyTextureRegion             , COPYTEXTUREREGION )           \
    macro(CopyResource                  , COPYRESOURCE )                \
    macro(CopyTiles                     , COPYTILES )                   \
    macro(ResolveSubresource            , RESOLVESUBRESOURCE )          \
    macro(ClearRenderTargetView         , CLEARRENDERTARGETVIEW )       \
    macro(ClearDepthStencilView         , CLEARDEPTHSTENCILVIEW )       \
    macro(ResourceBarrier               , RESOURCEBARRIER )             \
    macro(ExecuteBundle                 , EXECUTEBUNDLE )               \
    macro(AtomicCopyBufferUINT          , ATOMICCOPYBUFFERUINT )        \
    macro(AtomicCopyBufferUINT64        , ATOMICCOPYBUFFERUINT64 )      \
    macro(ResolveSubresourceRegion      , RESOLVESUBRESOURCEREGION )    \
    macro(WriteBufferImmediate          , WRITEBUFFERIMMEDIATE )        \
    macro(InitializeMetaCommand         , INITIALIZEMETACOMMAND )       \
    macro(ExecuteMetaCommand            , EXECUTEMETACOMMAND )          \
    macro(BuildRaytracingAccelerationStructure, BUILDRAYTRACINGACCELERATIONSTRUCTURE )        \
    macro(EmitRaytracingAccelerationStructurePostbuildInfo, EMITRAYTRACINGACCELERATIONSTRUCTUREPOSTBUILDINFO )        \
    macro(CopyRaytracingAccelerationStructure, COPYRAYTRACINGACCELERATIONSTRUCTURE )        \
    macro(DispatchRays                  , DISPATCHRAYS )                \
    macro(DispatchMesh                  , DISPATCHMESH )                \
    macro(Barrier                       , BARRIER )                     \
    macro(DispatchGraph                 , DISPATCHGRAPH )               \
    macro(SetProgram                    , SETPROGRAM )                  \

// These render ops only exist in the preview SDK; older Agility SDKs do not
// declare their D3D12_MARKER_API_* values, so the library does not decode them.
#if defined(D3D12_PREVIEW_SDK_VERSION) && D3D12_PREVIEW_SDK_VERSION >= 721

#define FOR_EACH_GRAPHICS_PREVIEW_RENDER_OP_API(macro) \
    macro(WaitBarrier                   , WAITBARRIER )                 \
    macro(SignalBarrier                 , SIGNALBARRIER )               \
    macro(ConvertLinearAlgebraMatrix    , CONVERT_LINEAR_ALGEBRA_MATRIX) \

// Render ops which exist on async command lists
#define FOR_EACH_ASYNC_COMMANDS_RENDER_OP_API(macro) \
    macro(CopyBufferRegions             , COPYBUFFERREGIONS )           \
    macro(CopyTextureRegions            , COPYTEXTUREREGIONS )          \
    macro(CopyResources                 , COPYRESOURCES )               \
    macro(CopyTilesAsync                , ASYNCCOPYTILES )              \
    macro(ResolveSubresourceRegionAsync , ASYNCRESOLVESUBRESOURCEREGION ) \
    macro(ResolveQueryDataAsync         , ASYNCRESOLVEQUERYDATA )       \
    macro(ClearBoundDepthStencilView    , CLEARBOUNDDEPTHSTENCILVIEW )  \
    macro(ClearBoundRenderTargetViews   , CLEARBOUNDRENDERTARGETVIEWS ) \
    macro(ClearTextureSubresources      , CLEARTEXTURESUBRESOURCES )    \
    macro(FillBuffers                   , FILLBUFFERS )                 \

#else

#define FOR_EACH_GRAPHICS_PREVIEW_RENDER_OP_API(macro)
#define FOR_EACH_ASYNC_COMMANDS_RENDER_OP_API(macro)

#endif

#define FOR_EACH_RENDER_OP_COMMON_VIDEO_RENDER_OP_API(macro) \
    macro(InitializeExtensionCommand    , INITIALIZEEXTENSIONCOMMAND )  \
    macro(ExecuteExtensionCommand       , EXECUTEEXTENSIONCOMMAND )     \

// Render ops which exist on video decode command lists
#define FOR_EACH_VIDEO_DECODE_RENDER_OP_API(macro)              \
    macro(DecodeFrame               , DECODEFRAME )             \
    macro(DecodeFrame1              , DECODEFRAME1 )            \
    macro(DecodeFrame2              , DECODEFRAME2 )            \

// Render ops which exist on video process command lists
#define FOR_EACH_VIDEO_PROCESS_RENDER_OP_API(macro)             \
    macro(ProcessFrames             , PROCESSFRAMES )           \
    macro(ProcessFrames1            , PROCESSFRAMES1 )          \

// Render ops which exist on video encode command lists
#define FOR_EACH_VIDEO_ENCODE_RENDER_OP_API(macro) \
    macro(EstimateMotion                , ESTIMATEMOTION )              \
    macro(ResolveMotionVectorHeap       , RESOLVEMOTIONVECTORHEAP )     \
    macro(EncodeFrame                   , ENCODEFRAME )              \
    macro(ResolveEncoderOutputMetadata  , RESOLVEENCODEROUTPUTMETADATA )     \

// Render ops which do not have matching public APIs
#define FOR_EACH_RENDER_OP(macro) \
    FOR_EACH_RENDER_OP_COMMON_API(macro) \
    FOR_EACH_RENDER_OP_COMMON_VIDEO_RENDER_OP_API(macro) \
    FOR_EACH_GRAPHICS_RENDER_OP_API(macro) \
    FOR_EACH_GRAPHICS_PREVIEW_RENDER_OP_API(macro) \
    FOR_EACH_VIDEO_DECODE_RENDER_OP_API(macro) \
    FOR_EACH_VIDEO_PROCESS_RENDER_OP_API(macro) \
    FOR_EACH_VIDEO_ENCODE_RENDER_OP_API(macro) \
    FOR_EACH_ASYNC_COMMANDS_RENDER_OP_API(macro) \
    macro(ClearUnorderedAccessView , CLEARUNORDEREDACCESSVIEW )  \
    macro(Present                  , PRESENT )                   \
    macro(BeginSubmission          , BEGINSUBMISSION )           \
    macro(EndSubmission            , ENDSUBMISSION )             \

    enum EMarkerOp : uint8_t
    {
#define DECLARE_MARKER_OP(ApiName, ETWNAME)                             \
        e_MOp_##ApiName = D3D12_MARKER_API_##ETWNAME,

        // Add new markers by updating the ETW manifest first, and then updating
        // the FOR_EACH macros above to declare a mapping from API/enum name to
        // ETW manifest entry name.  If you hit a compiler error here, you
        // likely need to update the ETW manifest or confirm ETWNAME is correct.
        FOR_EACH_RENDER_OP(DECLARE_MARKER_OP)

#undef DECLARE_MARKER_OP

        e_MOp_None = -1
    };

    inline wchar_t const* ToString(EMarkerOp op)
    {
        switch (op)
        {
#define E(ApiName, ETWNAME) case e_MOp_##ApiName: return L#ApiName;
            FOR_EACH_RENDER_OP(E)
#undef E
        case e_MOp_None: return L"None";
        default: return L"<<unknown>>";
        }
    }

#undef FOR_EACH_RENDER_OP
#undef FOR_EACH_ASYNC_COMMANDS_RENDER_OP_API
#undef FOR_EACH_VIDEO_ENCODE_RENDER_OP_API
#undef FOR_EACH_VIDEO_PROCESS_RENDER_OP_API
#undef FOR_EACH_VIDEO_DECODE_RENDER_OP_API
#undef FOR_EACH_GRAPHICS_RENDER_OP_API
#undef FOR_EACH_GRAPHICS_PREVIEW_RENDER_OP_API
#undef FOR_EACH_RENDER_OP_COMMON_VIDEO_RENDER_OP_API
#undef FOR_EACH_RENDER_OP_COMMON_API
#undef ETW_MANIFEST_MARKER_VALUE

} // namespace DirectX::Etw
