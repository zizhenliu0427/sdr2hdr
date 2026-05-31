#include "rtx_converter.h"

#include <cuda.h>

#include "rtx_video_api.h"

#include <cstdio>

#if defined(_WIN32)
  #if defined(NDEBUG)
    #pragma comment(lib, "nvsdk_ngx_d.lib")
  #else
    #pragma comment(lib, "nvsdk_ngx_d_dbg.lib")
  #endif
  #pragma comment(lib, "cuda.lib")
#endif

namespace {
bool checkCu(CUresult r, const char* what)
{
    if (r == CUDA_SUCCESS) return true;
    const char* s = nullptr;
    cuGetErrorString(r, &s);
    fprintf(stderr, "[CUDA] %s failed: %s\n", what, s ? s : "unknown");
    return false;
}
} // namespace

RtxConverter::RtxConverter() = default;
RtxConverter::~RtxConverter() { shutdown(); }

bool RtxConverter::initialize(Mode mode,
                              uint32_t inWidth,  uint32_t inHeight,
                              uint32_t outWidth, uint32_t outHeight,
                              int gpuIndex)
{
    m_mode = mode;
    m_inW  = inWidth;  m_inH  = inHeight;
    m_outW = outWidth; m_outH = outHeight;

    // VSR requires output larger than input; same-size VSR is undefined.
    if (mode != Mode::Hdr)
    {
        if (outWidth < inWidth || outHeight < inHeight)
        {
            fprintf(stderr,
                "VSR output size (%ux%u) must be >= input size (%ux%u).\n",
                outWidth, outHeight, inWidth, inHeight);
            return false;
        }
    }
    else
    {
        // HDR-only path: output size MUST equal input size.
        m_outW = inWidth; m_outH = inHeight;
    }

    if (!checkCu(cuInit(0), "cuInit")) return false;

    int nGpu = 0;
    if (!checkCu(cuDeviceGetCount(&nGpu), "cuDeviceGetCount")) return false;
    if (gpuIndex < 0 || gpuIndex >= nGpu)
    {
        fprintf(stderr, "[CUDA] Invalid GPU index %d (count=%d)\n", gpuIndex, nGpu);
        return false;
    }

    CUdevice dev = 0;
    if (!checkCu(cuDeviceGet(&dev, gpuIndex), "cuDeviceGet")) return false;

    CUcontext ctx = nullptr;
    if (!checkCu(cuCtxCreate(&ctx, 0, dev), "cuCtxCreate")) return false;
    m_cuContext   = ctx;
    m_ownsContext = true;
    m_cuStream    = nullptr;

    const API_BOOL enableHdr = (mode == Mode::Hdr    || mode == Mode::VsrHdr) ? API_BOOL_SUCCESS : API_BOOL_FAIL;
    const API_BOOL enableVsr = (mode == Mode::Vsr    || mode == Mode::VsrHdr) ? API_BOOL_SUCCESS : API_BOOL_FAIL;

    if (!rtx_video_api_cuda_create(m_cuContext, /*cuStream*/ nullptr,
                                   gpuIndex, enableHdr, enableVsr))
    {
        fprintf(stderr, "rtx_video_api_cuda_create failed "
                        "(hdr=%d, vsr=%d). Check driver/DLL presence.\n",
                enableHdr, enableVsr);
        return false;
    }
    m_rtxAlive = true;
    return true;
}

bool RtxConverter::initializeWithContext(Mode mode,
                                         uint32_t inWidth,  uint32_t inHeight,
                                         uint32_t outWidth, uint32_t outHeight,
                                         void* existingCuContext,
                                         void* cuStream,
                                         int   gpuIndex)
{
    m_mode = mode;
    m_inW  = inWidth;  m_inH  = inHeight;
    m_outW = outWidth; m_outH = outHeight;

    if (!existingCuContext)
    {
        fprintf(stderr, "RtxConverter: existingCuContext is null.\n");
        return false;
    }

    if (mode != Mode::Hdr)
    {
        if (outWidth < inWidth || outHeight < inHeight)
        {
            fprintf(stderr,
                "VSR output size (%ux%u) must be >= input size (%ux%u).\n",
                outWidth, outHeight, inWidth, inHeight);
            return false;
        }
    }
    else
    {
        m_outW = inWidth; m_outH = inHeight;
    }

    m_cuContext   = existingCuContext;
    m_cuStream    = cuStream;
    m_ownsContext = false;

    const API_BOOL enableHdr = (mode == Mode::Hdr    || mode == Mode::VsrHdr) ? API_BOOL_SUCCESS : API_BOOL_FAIL;
    const API_BOOL enableVsr = (mode == Mode::Vsr    || mode == Mode::VsrHdr) ? API_BOOL_SUCCESS : API_BOOL_FAIL;

    if (!rtx_video_api_cuda_create(m_cuContext, m_cuStream,
                                   gpuIndex, enableHdr, enableVsr))
    {
        fprintf(stderr, "rtx_video_api_cuda_create failed "
                        "(hdr=%d, vsr=%d). Driver/DLL missing or busy?\n",
                enableHdr, enableVsr);
        return false;
    }
    m_rtxAlive = true;
    return true;
}

bool RtxConverter::convertFrame(const void* hostIn, void* hostOut, const Params& p)
{
    if (!m_rtxAlive) return false;

    API_RECT src{ 0, 0, m_inW,  m_inH  };
    API_RECT dst{ 0, 0, m_outW, m_outH };

    API_VSR_Setting  vsr{};
    vsr.QualityLevel = p.vsrQuality;

    API_THDR_Setting thdr{};
    thdr.Contrast     = p.contrast;
    thdr.Saturation   = p.saturation;
    thdr.MiddleGray   = p.middleGray;
    thdr.MaxLuminance = p.maxLum;

    // hostptr path requires pitch = 4*width on both sides.
    API_BOOL ok = rtx_video_api_cuda_evaluate_hostptr(
        const_cast<void*>(hostIn), hostOut,
        src, dst, &vsr, &thdr);
    if (ok != API_BOOL_SUCCESS) return false;

    cuCtxSynchronize();
    return true;
}

bool RtxConverter::convertFrameDevicePtr(unsigned long long devIn,
                                         unsigned long long devOut,
                                         const Params& p)
{
    if (!m_rtxAlive) return false;

    API_RECT src{ 0, 0, m_inW,  m_inH  };
    API_RECT dst{ 0, 0, m_outW, m_outH };

    API_VSR_Setting  vsr{};
    vsr.QualityLevel = p.vsrQuality;

    API_THDR_Setting thdr{};
    thdr.Contrast     = p.contrast;
    thdr.Saturation   = p.saturation;
    thdr.MiddleGray   = p.middleGray;
    thdr.MaxLuminance = p.maxLum;

    // deviceptr path: RTX SDK reads/writes the buffers directly in GPU VRAM.
    // The API requires pitch = 4*width on both sides, so callers must
    // allocate with cuMemAlloc / cudaMalloc (tightly packed).
    void* in  = reinterpret_cast<void*>(static_cast<uintptr_t>(devIn));
    void* out = reinterpret_cast<void*>(static_cast<uintptr_t>(devOut));
    API_BOOL ok = rtx_video_api_cuda_evaluate_deviceptr(
        in, out, src, dst, &vsr, &thdr);
    return ok == API_BOOL_SUCCESS;
}

void RtxConverter::shutdown()
{
    if (m_rtxAlive)
    {
        rtx_video_api_cuda_shutdown();
        m_rtxAlive = false;
    }
    if (m_cuContext && m_ownsContext)
    {
        cuCtxDestroy(static_cast<CUcontext>(m_cuContext));
        m_cuContext = nullptr;
    }
    else
    {
        m_cuContext = nullptr;    // not ours; caller will destroy
    }
}
