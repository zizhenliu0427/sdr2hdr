// Direct NGX session wrapper for the GPU pipeline (#10/#11 throughput work).
//
// The RTX Video SDK sample wrapper (rtx_video_api_cuda_impl.cpp) is a process
// -wide singleton whose deviceptr path stages frames through CUDA arrays with
// *synchronous* cuMemcpy2D -- every frame blocks the host until the whole
// context drains, which serialises decode / NGX / encode and caps 4K TrueHDR
// at ~130 fps regardless of GPU. This wrapper talks to NGX directly so that:
//
//   - the linear <-> CUDA-array staging copies are cuMemcpy2DAsync on the
//     session's stream (no host stalls, no context-wide syncs);
//   - several sessions can exist at once, each bound to its own CUDA stream,
//     so consecutive frames run on independent NGX feature instances and
//     overlap on the GPU (#11: even/odd frame striping).
//
// Threading: create / evaluateAsync / shutdown must all be called from
// threads with the shared CUcontext current. evaluateAsync only *enqueues*
// onto the session stream. All sessions share NGX's capability parameter
// object, so evaluateAsync must not be called from two threads concurrently
// (the pipeline drives every session from its single process thread).
#pragma once

#include "rtx_converter.h"   // RtxConverter::Mode / ::Params

#include <cstdint>

namespace sdr2hdr {

// Initialise the NGX runtime once per process (NVSDK_NGX_CUDA_Init +
// capability query). Returns false when NGX / the feature DLLs are absent.
// Must be balanced by ngxRuntimeShutdown(); not reference counted.
bool ngxRuntimeInit();
void ngxRuntimeShutdown();

class NgxSession
{
public:
    NgxSession() = default;
    ~NgxSession() { shutdown(); }
    NgxSession(const NgxSession&) = delete;
    NgxSession& operator=(const NgxSession&) = delete;

    // `stream` is a cudaStream_t / CUstream the session is permanently bound
    // to (NGX takes it at feature-creation time).
    bool init(RtxConverter::Mode mode,
              uint32_t inW, uint32_t inH,
              uint32_t outW, uint32_t outH,
              void* cuContext, void* stream);

    // Enqueue: devIn (tightly packed RGBA8, pitch = 4*inW) -> NGX ->
    // devOut (RGBA8 or packed ABGR10 for HDR modes, pitch = 4*outW).
    // Asynchronous with respect to the host; ordered on the session stream.
    bool evaluateAsync(unsigned long long devIn,
                       unsigned long long devOut,
                       const RtxConverter::Params& p);

    void shutdown();

private:
    bool createArrays();

    RtxConverter::Mode m_mode = RtxConverter::Mode::Hdr;
    uint32_t m_inW = 0, m_inH = 0, m_outW = 0, m_outH = 0;
    void*    m_stream = nullptr;          // CUstream

    void*    m_thdrFeature = nullptr;     // NVSDK_NGX_Handle*
    void*    m_vsrFeature  = nullptr;     // NVSDK_NGX_Handle*

    // CUDA array staging (NGX consumes texture/surface objects).
    void*               m_arrSrc = nullptr;   // CUarray
    void*               m_arrMid = nullptr;   // CUarray (VSR+HDR only)
    void*               m_arrDst = nullptr;   // CUarray
    unsigned long long  m_texSrc  = 0;        // CUtexObject
    unsigned long long  m_texMid  = 0;
    unsigned long long  m_surfMid = 0;        // CUsurfObject
    unsigned long long  m_surfDst = 0;
};

} // namespace sdr2hdr
