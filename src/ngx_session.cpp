#include "ngx_session.h"

#include <cuda.h>

#include <nvsdk_ngx.h>
#include <nvsdk_ngx_helpers_truehdr.h>
#include <nvsdk_ngx_helpers_vsr.h>

#include <cstdio>
#include <cstring>

namespace sdr2hdr {

namespace {

// Same identification the RTX Video SDK sample wrapper uses.
constexpr unsigned long long kNgxAppId = 0;
constexpr const wchar_t*     kNgxAppPath = L".";

NVSDK_NGX_Parameter* g_ngxParams = nullptr;   // shared capability params
bool                 g_ngxInited = false;

bool ngxOk(NVSDK_NGX_Result r, const char* what)
{
    if (NVSDK_NGX_SUCCEED(r)) return true;
    fprintf(stderr, "[NGX] %s failed (0x%08x)\n", what, static_cast<unsigned>(r));
    return false;
}

bool drvOk(CUresult r, const char* what)
{
    if (r == CUDA_SUCCESS) return true;
    const char* name = nullptr;
    cuGetErrorName(r, &name);
    fprintf(stderr, "[NGX] %s failed: %s\n", what, name ? name : "?");
    return false;
}

} // namespace

bool ngxRuntimeInit()
{
    if (g_ngxInited) return true;
    if (!ngxOk(NVSDK_NGX_CUDA_Init(kNgxAppId, kNgxAppPath), "NVSDK_NGX_CUDA_Init"))
        return false;
    if (!ngxOk(NVSDK_NGX_CUDA_GetCapabilityParameters(&g_ngxParams),
               "NVSDK_NGX_CUDA_GetCapabilityParameters"))
    {
        NVSDK_NGX_CUDA_Shutdown();
        return false;
    }
    g_ngxInited = true;
    return true;
}

void ngxRuntimeShutdown()
{
    if (!g_ngxInited) return;
    if (g_ngxParams)
    {
        NVSDK_NGX_CUDA_DestroyParameters(g_ngxParams);
        g_ngxParams = nullptr;
    }
    NVSDK_NGX_CUDA_Shutdown();
    g_ngxInited = false;
}

// ---------------------------------------------------------------------------
// NgxSession
// ---------------------------------------------------------------------------

bool NgxSession::createArrays()
{
    // Mirrors the staging layout of the SDK sample's deviceptr path: NGX
    // consumes CUDA texture objects and writes surface objects, both backed
    // by CUarrays. Source is always RGBA8; destination is packed 10:10:10:2
    // for the HDR modes (what launchAbgr2101010ToP010 expects), RGBA8 for
    // SDR VSR. VSR+HDR routes through an RGBA8 middle array at output size.
    auto makeTex = [&](CUarray arr, unsigned long long& tex) -> bool
    {
        CUDA_RESOURCE_DESC res{};
        res.resType = CU_RESOURCE_TYPE_ARRAY;
        res.res.array.hArray = arr;
        CUDA_TEXTURE_DESC td{};
        td.addressMode[0] = CU_TR_ADDRESS_MODE_CLAMP;
        td.addressMode[1] = CU_TR_ADDRESS_MODE_CLAMP;
        td.addressMode[2] = CU_TR_ADDRESS_MODE_CLAMP;
        td.filterMode     = CU_TR_FILTER_MODE_LINEAR;
        td.flags          = CU_TRSF_NORMALIZED_COORDINATES;
        CUtexObject t = 0;
        if (!drvOk(cuTexObjectCreate(&t, &res, &td, nullptr), "cuTexObjectCreate"))
            return false;
        tex = t;
        return true;
    };
    auto makeSurf = [&](CUarray arr, unsigned long long& surf) -> bool
    {
        CUDA_RESOURCE_DESC res{};
        res.resType = CU_RESOURCE_TYPE_ARRAY;
        res.res.array.hArray = arr;
        CUsurfObject s = 0;
        if (!drvOk(cuSurfObjectCreate(&s, &res), "cuSurfObjectCreate"))
            return false;
        surf = s;
        return true;
    };

    const bool hdrOut = (m_mode == RtxConverter::Mode::Hdr ||
                         m_mode == RtxConverter::Mode::VsrHdr);
    const bool both   = (m_mode == RtxConverter::Mode::VsrHdr);

    CUDA_ARRAY_DESCRIPTOR src{ m_inW, m_inH, CU_AD_FORMAT_UNSIGNED_INT8, 4 };
    CUarray arr = nullptr;
    if (!drvOk(cuArrayCreate(&arr, &src), "cuArrayCreate(src)")) return false;
    m_arrSrc = arr;
    if (!makeTex(arr, m_texSrc)) return false;

    if (both)
    {
        CUDA_ARRAY_DESCRIPTOR mid{ m_outW, m_outH, CU_AD_FORMAT_UNSIGNED_INT8, 4 };
        if (!drvOk(cuArrayCreate(&arr, &mid), "cuArrayCreate(mid)")) return false;
        m_arrMid = arr;
        if (!makeTex(arr, m_texMid))  return false;
        if (!makeSurf(arr, m_surfMid)) return false;
    }

    CUDA_ARRAY_DESCRIPTOR dst{
        m_outW, m_outH,
        hdrOut ? CU_AD_FORMAT_UNORM_INT_101010_2 : CU_AD_FORMAT_UNSIGNED_INT8,
        4 };
    if (!drvOk(cuArrayCreate(&arr, &dst), "cuArrayCreate(dst)")) return false;
    m_arrDst = arr;
    if (!makeSurf(arr, m_surfDst)) return false;

    return true;
}

bool NgxSession::init(RtxConverter::Mode mode,
                      uint32_t inW, uint32_t inH,
                      uint32_t outW, uint32_t outH,
                      void* cuContext, void* stream)
{
    if (!g_ngxInited) return false;
    m_mode = mode;
    m_inW = inW;  m_inH = inH;
    m_outW = outW; m_outH = outH;
    m_stream = stream;

    const bool wantThdr = (mode == RtxConverter::Mode::Hdr ||
                           mode == RtxConverter::Mode::VsrHdr);
    const bool wantVsr  = (mode == RtxConverter::Mode::Vsr ||
                           mode == RtxConverter::Mode::VsrHdr);

    if (wantThdr)
    {
        int avail = 0;
        g_ngxParams->Get(NVSDK_NGX_Parameter_TrueHDR_Available, &avail);
        if (!avail)
        {
            fprintf(stderr, "[NGX] TrueHDR not available on this driver/GPU.\n");
            return false;
        }
        NVSDK_NGX_CUDA_TRUEHDR_Create_Params cp{};
        cp.InCUContext = cuContext;
        cp.InCUStream  = stream;
        NVSDK_NGX_Handle* h = nullptr;
        // NOTE: NGX allows only ONE live TrueHDR feature instance per
        // process -- a second create returns FeatureAlreadyExists
        // (0xbad00003), verified even with a separate parameter object and
        // a separate CUDA context (driver 610.47 / SDK v1.1.0). In-process
        // multi-session parallelism is therefore impossible; scaling beyond
        // one instance needs multi-process sharding.
        if (!ngxOk(NGX_CUDA_CREATE_TRUEHDR(&h, g_ngxParams, &cp),
                   "NGX_CUDA_CREATE_TRUEHDR"))
            return false;
        m_thdrFeature = h;
    }
    if (wantVsr)
    {
        int avail = 0;
        g_ngxParams->Get(NVSDK_NGX_Parameter_VSR_Available, &avail);
        if (!avail)
        {
            fprintf(stderr, "[NGX] VSR not available on this driver/GPU.\n");
            return false;
        }
        NVSDK_NGX_CUDA_VSR_Create_Params cp{};
        cp.InCUContext = cuContext;
        cp.InCUStream  = stream;
        NVSDK_NGX_Handle* h = nullptr;
        if (!ngxOk(NGX_CUDA_CREATE_VSR(&h, g_ngxParams, &cp),
                   "NGX_CUDA_CREATE_VSR"))
            return false;
        m_vsrFeature = h;
    }

    return createArrays();
}

bool NgxSession::evaluateAsync(unsigned long long devIn,
                               unsigned long long devOut,
                               const RtxConverter::Params& p)
{
    CUstream stream = static_cast<CUstream>(m_stream);

    // Linear RGBA8 -> staging array, ordered on the session stream.
    {
        CUDA_MEMCPY2D cp{};
        cp.srcMemoryType = CU_MEMORYTYPE_DEVICE;
        cp.srcDevice     = static_cast<CUdeviceptr>(devIn);
        cp.srcPitch      = static_cast<size_t>(m_inW) * 4;
        cp.dstMemoryType = CU_MEMORYTYPE_ARRAY;
        cp.dstArray      = static_cast<CUarray>(m_arrSrc);
        cp.WidthInBytes  = static_cast<size_t>(m_inW) * 4;
        cp.Height        = m_inH;
        if (!drvOk(cuMemcpy2DAsync(&cp, stream), "cuMemcpy2DAsync(in)"))
            return false;
    }

    const bool both = (m_mode == RtxConverter::Mode::VsrHdr);

    if (m_vsrFeature)
    {
        // Rect plumbing mirrors the SDK sample's evaluate() exactly.
        NVSDK_NGX_CUDA_VSR_Eval_Params ep{};
        unsigned long long vsrOut = both ? m_texMid : m_surfDst;
        ep.pInput                  = &m_texSrc;
        ep.pOutput                 = &vsrOut;
        ep.InputSubrectBase.X      = 0;
        ep.InputSubrectBase.Y      = 0;
        ep.InputSubrectSize.Width  = m_inW;
        ep.InputSubrectSize.Height = m_inH;
        ep.OutputSubrectBase.X     = 0;
        ep.OutputSubrectBase.Y     = 0;
        ep.OutputSubrectSize.Width  = m_outW;
        ep.OutputSubrectSize.Height = m_outH;
        ep.QualityLevel = static_cast<NVSDK_NGX_VSR_QualityLevel>(p.vsrQuality);
        if (!ngxOk(NGX_CUDA_EVALUATE_VSR(
                       static_cast<NVSDK_NGX_Handle*>(m_vsrFeature),
                       g_ngxParams, &ep),
                   "NGX_CUDA_EVALUATE_VSR"))
            return false;
    }
    if (m_thdrFeature)
    {
        NVSDK_NGX_CUDA_TRUEHDR_Eval_Params ep{};
        ep.pInput                    = both ? &m_texMid : &m_texSrc;
        ep.pOutput                   = &m_surfDst;
        ep.InputSubrectTL.X          = 0;
        ep.InputSubrectTL.Y          = 0;
        ep.InputSubrectBR.Width      = both ? m_outW : m_inW;
        ep.InputSubrectBR.Height     = both ? m_outH : m_inH;
        ep.OutputSubrectTL.X         = 0;
        ep.OutputSubrectTL.Y         = 0;
        ep.OutputSubrectBR.Width     = m_outW;
        ep.OutputSubrectBR.Height    = m_outH;
        ep.Contrast     = p.contrast;
        ep.Saturation   = p.saturation;
        ep.MiddleGray   = p.middleGray;
        ep.MaxLuminance = p.maxLum;
        if (!ngxOk(NGX_CUDA_EVALUATE_TRUEHDR(
                       static_cast<NVSDK_NGX_Handle*>(m_thdrFeature),
                       g_ngxParams, &ep),
                   "NGX_CUDA_EVALUATE_TRUEHDR"))
            return false;
    }

    // Staging array -> linear output, ordered behind the NGX work.
    {
        CUDA_MEMCPY2D cp{};
        cp.srcMemoryType = CU_MEMORYTYPE_ARRAY;
        cp.srcArray      = static_cast<CUarray>(m_arrDst);
        cp.dstMemoryType = CU_MEMORYTYPE_DEVICE;
        cp.dstDevice     = static_cast<CUdeviceptr>(devOut);
        cp.dstPitch      = static_cast<size_t>(m_outW) * 4;
        cp.WidthInBytes  = static_cast<size_t>(m_outW) * 4;
        cp.Height        = m_outH;
        if (!drvOk(cuMemcpy2DAsync(&cp, stream), "cuMemcpy2DAsync(out)"))
            return false;
    }
    return true;
}

void NgxSession::shutdown()
{
    if (m_thdrFeature)
    {
        NVSDK_NGX_CUDA_ReleaseFeature(static_cast<NVSDK_NGX_Handle*>(m_thdrFeature));
        m_thdrFeature = nullptr;
    }
    if (m_vsrFeature)
    {
        NVSDK_NGX_CUDA_ReleaseFeature(static_cast<NVSDK_NGX_Handle*>(m_vsrFeature));
        m_vsrFeature = nullptr;
    }
    if (m_texSrc)  { cuTexObjectDestroy(m_texSrc);   m_texSrc = 0; }
    if (m_texMid)  { cuTexObjectDestroy(m_texMid);   m_texMid = 0; }
    if (m_surfMid) { cuSurfObjectDestroy(m_surfMid); m_surfMid = 0; }
    if (m_surfDst) { cuSurfObjectDestroy(m_surfDst); m_surfDst = 0; }
    if (m_arrSrc)  { cuArrayDestroy(static_cast<CUarray>(m_arrSrc)); m_arrSrc = nullptr; }
    if (m_arrMid)  { cuArrayDestroy(static_cast<CUarray>(m_arrMid)); m_arrMid = nullptr; }
    if (m_arrDst)  { cuArrayDestroy(static_cast<CUarray>(m_arrDst)); m_arrDst = nullptr; }
}

} // namespace sdr2hdr
