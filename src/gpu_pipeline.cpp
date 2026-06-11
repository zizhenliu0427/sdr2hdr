// GPU-only pipeline implementation. See gpu_pipeline.h for the high-level
// data flow diagram.
//
// NOTE: we include CUDA / NvDecoder / NvEncoder headers only here, inside a
// .cpp file, so the rest of sdr2hdr doesn't pay the compile-time cost nor
// the transitive #define pollution.

#include "gpu_pipeline.h"
#include "bitstream_pipe.h"
#include "cuda_kernels.h"
#include "ngx_session.h"

#include <cuda.h>
#include <cuda_runtime.h>

#include "NvDecoder/NvDecoder.h"
#include "NvEncoder/NvEncoderCuda.h"
#include "Logger.h"

// ---------------------------------------------------------------------------
// NVIDIA's NvCodec sample wrappers expect a global `simplelogger::Logger *logger`
// symbol (normally defined once in each sample's main.cpp). We provide a
// quiet ERROR-only console logger here so the unresolved-extern at link time
// goes away; info/warning chatter from the sample code stays suppressed.
// ---------------------------------------------------------------------------
// LogLevel (TRACE/INFO/WARNING/ERROR/FATAL) is declared at global scope in
// Logger.h, not inside the simplelogger namespace. Logger.h does #undef ERROR
// so the enum value resolves correctly here.
simplelogger::Logger* logger =
    simplelogger::LoggerFactory::CreateConsoleLogger(::ERROR);

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace sdr2hdr {

namespace {

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

#define CUDA_DRV_CK(call) do {                                            \
    CUresult _r = (call);                                                 \
    if (_r != CUDA_SUCCESS) {                                             \
        const char* _n = nullptr; cuGetErrorName(_r, &_n);                \
        fprintf(stderr, "[CUDA drv] %s failed: %s\n", #call, _n ? _n : "?"); \
        return false;                                                     \
    } } while (0)

#define CUDA_RT_CK(call) do {                                             \
    cudaError_t _r = (call);                                              \
    if (_r != cudaSuccess) {                                              \
        fprintf(stderr, "[CUDA rt] %s failed: %s\n",                      \
                #call, cudaGetErrorString(_r));                           \
        return false;                                                     \
    } } while (0)

// Map sdr2hdr codec string -> NvDecoder's cudaVideoCodec.
bool codecStringToNvDec(const std::string& s, cudaVideoCodec& out)
{
    if (s == "h264" || s == "avc")  { out = cudaVideoCodec_H264; return true; }
    if (s == "hevc" || s == "h265") { out = cudaVideoCodec_HEVC; return true; }
    if (s == "av1")                 { out = cudaVideoCodec_AV1;  return true; }
    if (s == "vp9")                 { out = cudaVideoCodec_VP9;  return true; }
    if (s == "mpeg4" || s == "xvid"){ out = cudaVideoCodec_MPEG4; return true;}
    if (s == "mpeg2")               { out = cudaVideoCodec_MPEG2; return true;}
    return false;
}

// Map sdr2hdr encode codec -> NvEncoder codec GUID.
bool codecToEncGuid(const std::string& s, GUID& out)
{
    if (s == "h264") { out = NV_ENC_CODEC_H264_GUID; return true; }
    if (s == "hevc" || s == "h265") { out = NV_ENC_CODEC_HEVC_GUID; return true; }
    if (s == "av1")  { out = NV_ENC_CODEC_AV1_GUID;  return true; }
    return false;
}

// Map preset string "p1"... "p7" to NvEnc preset GUID.
GUID presetStringToGuid(const std::string& s)
{
    if (s == "p1") return NV_ENC_PRESET_P1_GUID;
    if (s == "p2") return NV_ENC_PRESET_P2_GUID;
    if (s == "p3") return NV_ENC_PRESET_P3_GUID;
    if (s == "p4") return NV_ENC_PRESET_P4_GUID;
    if (s == "p5") return NV_ENC_PRESET_P5_GUID;
    if (s == "p6") return NV_ENC_PRESET_P6_GUID;
    if (s == "p7") return NV_ENC_PRESET_P7_GUID;
    return NV_ENC_PRESET_P5_GUID;       // default: balanced
}

// ---------------------------------------------------------------------------
// DevBuffer: tightly-packed pitch=width*bpp RGBA device allocation.
// ---------------------------------------------------------------------------
struct DevBuffer
{
    CUdeviceptr ptr    = 0;
    size_t      bytes  = 0;
    int         width  = 0;   // in pixels
    int         height = 0;
    int         pitch  = 0;   // in bytes

    bool alloc(int w, int h, int bytesPerPixel)
    {
        free_();
        width  = w; height = h;
        pitch  = w * bytesPerPixel;
        bytes  = static_cast<size_t>(pitch) * h;
        return cuMemAlloc(&ptr, bytes) == CUDA_SUCCESS;
    }

    void free_()
    {
        if (ptr) { cuMemFree(ptr); ptr = 0; }
        bytes = 0; width = 0; height = 0; pitch = 0;
    }

    ~DevBuffer() { free_(); }
    DevBuffer() = default;
    DevBuffer(const DevBuffer&) = delete;
    DevBuffer& operator=(const DevBuffer&) = delete;
};

// ---------------------------------------------------------------------------
// Configure NVENC init params for our target codec / bit-depth / HDR flags.
// ---------------------------------------------------------------------------
void configureEncoder(NvEncoder& enc,
                      NV_ENC_INITIALIZE_PARAMS& initParams,
                      NV_ENC_CONFIG& encConfig,
                      const GpuPipelineOptions& opts,
                      GUID codecGuid,
                      uint32_t outW, uint32_t outH,
                      bool hdrOut)
{
    memset(&initParams, 0, sizeof(initParams));
    memset(&encConfig, 0, sizeof(encConfig));
    initParams.encodeConfig = &encConfig;

    GUID preset = presetStringToGuid(opts.preset.empty() ? "p5" : opts.preset);
    const NV_ENC_TUNING_INFO tuning = hdrOut
        ? NV_ENC_TUNING_INFO_HIGH_QUALITY
        : NV_ENC_TUNING_INFO_HIGH_QUALITY;

    enc.CreateDefaultEncoderParams(&initParams, codecGuid, preset, tuning);

    initParams.encodeWidth  = outW;
    initParams.encodeHeight = outH;
    initParams.darWidth     = outW;
    initParams.darHeight    = outH;

    // Frame rate for NVENC rate-control hints. When probeVideo filled an
    // exact rational (including VFR -> avg_frame_rate), use it directly
    // instead of re-rounding the double and picking the wrong heuristic.
    double fps = opts.input.fps > 0.1 ? opts.input.fps : 30.0;
    auto setRational = [&](uint32_t num, uint32_t den)
    {
        initParams.frameRateNum = num;
        initParams.frameRateDen = den;
    };
    if (opts.input.fpsNum && opts.input.fpsDen)
    {
        setRational(opts.input.fpsNum, opts.input.fpsDen);
    }
    else if      (fabs(fps - 23.976) < 0.05) setRational(24000, 1001);
    else if (fabs(fps - 24.0)   < 0.05) setRational(24, 1);
    else if (fabs(fps - 25.0)   < 0.05) setRational(25, 1);
    else if (fabs(fps - 29.97)  < 0.05) setRational(30000, 1001);
    else if (fabs(fps - 30.0)   < 0.05) setRational(30, 1);
    else if (fabs(fps - 48.0)   < 0.05) setRational(48, 1);
    else if (fabs(fps - 50.0)   < 0.05) setRational(50, 1);
    else if (fabs(fps - 59.94)  < 0.05) setRational(60000, 1001);
    else if (fabs(fps - 60.0)   < 0.05) setRational(60, 1);
    else if (fabs(fps - 100.0)  < 0.05) setRational(100, 1);
    // 4K120 cameras/capture often stamp 119.997 into avg_frame_rate via
    // count/duration rounding; r_frame_rate should pull it back to 120/1
    // but handle both for safety.
    else if (fabs(fps - 119.88) < 0.05) setRational(120000, 1001);
    else if (fabs(fps - 120.0)  < 0.1)  setRational(120, 1);
    else if (fabs(fps - 144.0)  < 0.1)  setRational(144, 1);
    else if (fabs(fps - 240.0)  < 0.1)  setRational(240, 1);
    else                                setRational(static_cast<uint32_t>(fps * 1000 + 0.5), 1000);

    // Rate control: CQP (constant QP) with a sensible default.
    int q = opts.quality > 0 ? opts.quality : (hdrOut ? 22 : 20);
    if (q < 1)  q = 1;
    if (q > 51) q = 51;
    encConfig.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CONSTQP;
    encConfig.rcParams.constQP.qpInterP = q;
    encConfig.rcParams.constQP.qpInterB = q;
    encConfig.rcParams.constQP.qpIntra  = q;

    // GOP: 5 sec of keyframes.
    //
    // Why 5s and not the more common 2s: at 120fps with IPPP + CQP, the IDR
    // frame is ~10-15x larger than a P-frame (at 4K ~1.5MB vs ~100KB). Every
    // IDR produces a noticeable bitrate spike plus a repeated VPS/SPS/PPS
    // header block. Some players (Windows 'Movies & TV', old mpv/MPC-HC
    // versions) stall for 1-2 frames at each IDR boundary to reset their
    // reference frame state -- the user-visible symptom is a tiny "hitch"
    // every GOP period. 5s stretches that interval out 2.5x, so what used
    // to be a visible jitter every 2s becomes much rarer.
    //
    // Downside: random-access / seeking granularity in the output is 5s
    // instead of 2s. Totally fine for playback; still fine for editing.
    // If this proves too coarse we can make it configurable.
    uint32_t gop = static_cast<uint32_t>(fps * 5.0 + 0.5);
    if (gop < 15) gop = 15;
    encConfig.gopLength = gop;

    // IPPP structure, no B-frames.
    //
    // WHY no B-frames: they save ~10-20% bitrate at the same QP but our mux
    // path is `ffmpeg -f hevc -c:v copy ... .mp4`. With B-frames, the HEVC
    // bitstream has encode-order != display-order, so the muxer has to parse
    // POC from slice headers and reorder PTS/DTS. Annex-B over a pipe with
    // -framerate does this unreliably -- observed symptom is micro-stutter
    // every few frames during playback even though the total frame count
    // and duration are correct.
    //
    // IPPP sidesteps this entirely: encode-order == display-order, so CFR
    // timestamps generated by the muxer from -framerate are always correct.
    // We lose a bit of compression efficiency; we gain smooth playback.
    encConfig.frameIntervalP = 1;

    // NOTE: intentionally NOT setting timingInfoPresentFlag / numUnitInTicks
    // / timeScale in the VUI. When we did, ffmpeg's mp4 muxer with
    // `-c:v copy` picked up those values and inflated the mdhd duration by
    // a huge factor (e.g. 2328:00:00 for a 291s source, 0.10 fps displayed
    // in Explorer Properties). The muxer's HEVC parser apparently multiplies
    // the timing twice under some code paths. The CLI `-framerate num/den`
    // we already pass to the muxer is sufficient for exact CFR timing.

    // Bit depth + HDR VUI.
    if (codecGuid == NV_ENC_CODEC_HEVC_GUID)
    {
        auto& hevc = encConfig.encodeCodecConfig.hevcConfig;
        hevc.outputBitDepth = hdrOut ? NV_ENC_BIT_DEPTH_10 : NV_ENC_BIT_DEPTH_8;
        hevc.inputBitDepth  = hdrOut ? NV_ENC_BIT_DEPTH_10 : NV_ENC_BIT_DEPTH_8;
        hevc.chromaFormatIDC = 1;      // YUV420
        hevc.idrPeriod       = gop;
        hevc.repeatSPSPPS    = 1;
        hevc.outputAUD       = 1;       // write Access Unit Delimiter NALs

        // Colour description always goes into the bitstream VUI: container-
        // level -color_* flags are ignored by ffmpeg's stream copy (observed
        // on 7.x/8.x), so VUI is the only tag that reliably survives the
        // two-pass mux. SDR gets explicit BT.709 rather than "unspecified".
        auto& vui = hevc.hevcVUIParameters;
        vui.videoSignalTypePresentFlag   = 1;
        vui.videoFullRangeFlag           = 0;               // limited range
        vui.colourDescriptionPresentFlag = 1;
        if (hdrOut)
        {
            // Don't flip outputMaxCll / outputMasteringDisplay without also
            // supplying per-pic pMaxCll / pMasteringDisplay payloads; some
            // driver versions will either reject the config or write a
            // malformed empty SEI. VUI below is what HDR10 players actually
            // key off in practice.
            vui.colourPrimaries              = NV_ENC_VUI_COLOR_PRIMARIES_BT2020;
            vui.transferCharacteristics      = NV_ENC_VUI_TRANSFER_CHARACTERISTIC_SMPTE2084;
            vui.colourMatrix                 = NV_ENC_VUI_MATRIX_COEFFS_BT2020_NCL;
        }
        else
        {
            vui.colourPrimaries              = NV_ENC_VUI_COLOR_PRIMARIES_BT709;
            vui.transferCharacteristics      = NV_ENC_VUI_TRANSFER_CHARACTERISTIC_BT709;
            vui.colourMatrix                 = NV_ENC_VUI_MATRIX_COEFFS_BT709;
        }
    }
    else if (codecGuid == NV_ENC_CODEC_H264_GUID)
    {
        auto& h264 = encConfig.encodeCodecConfig.h264Config;
        h264.chromaFormatIDC = 1;
        h264.idrPeriod       = gop;
        h264.repeatSPSPPS    = 1;
        h264.outputAUD       = 1;
        // (H.264 is 8-bit in the SDR path only; HDR on H.264 is unusual.)
        auto& vui = h264.h264VUIParameters;
        vui.videoSignalTypePresentFlag   = 1;
        vui.videoFullRangeFlag           = 0;
        vui.colourDescriptionPresentFlag = 1;
        vui.colourPrimaries              = NV_ENC_VUI_COLOR_PRIMARIES_BT709;
        vui.transferCharacteristics      = NV_ENC_VUI_TRANSFER_CHARACTERISTIC_BT709;
        vui.colourMatrix                 = NV_ENC_VUI_MATRIX_COEFFS_BT709;
    }
    else if (codecGuid == NV_ENC_CODEC_AV1_GUID)
    {
        auto& av1 = encConfig.encodeCodecConfig.av1Config;
        av1.outputBitDepth = hdrOut ? NV_ENC_BIT_DEPTH_10 : NV_ENC_BIT_DEPTH_8;
        av1.inputBitDepth  = hdrOut ? NV_ENC_BIT_DEPTH_10 : NV_ENC_BIT_DEPTH_8;
        av1.chromaFormatIDC = 1;
        av1.idrPeriod      = gop;
        av1.repeatSeqHdr   = 1;
        if (hdrOut)
        {
            av1.colorPrimaries          = NV_ENC_VUI_COLOR_PRIMARIES_BT2020;
            av1.transferCharacteristics = NV_ENC_VUI_TRANSFER_CHARACTERISTIC_SMPTE2084;
            av1.matrixCoefficients      = NV_ENC_VUI_MATRIX_COEFFS_BT2020_NCL;
            av1.colorRange              = 0;
            av1.outputMaxCll            = 1;
            av1.outputMasteringDisplay  = 1;
        }
        else
        {
            av1.colorPrimaries          = NV_ENC_VUI_COLOR_PRIMARIES_BT709;
            av1.transferCharacteristics = NV_ENC_VUI_TRANSFER_CHARACTERISTIC_BT709;
            av1.matrixCoefficients      = NV_ENC_VUI_MATRIX_COEFFS_BT709;
            av1.colorRange              = 0;
        }
    }
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// runGpuOnly : the actual worker.
// ---------------------------------------------------------------------------
GpuPipelineResult runGpuOnly(const std::string& input,
                             const std::string& output,
                             const GpuPipelineOptions& opts)
{
    GpuPipelineResult result;
    auto tStart = std::chrono::steady_clock::now();
    auto bailout = [&](const char* msg) -> GpuPipelineResult
    {
        result.ok = false;
        result.errorDetail = msg ? msg : "";
        return result;
    };

    const uint32_t inW  = opts.input.width;
    const uint32_t inH  = opts.input.height;
    const uint32_t outW = opts.outWidth  ? opts.outWidth  : inW;
    const uint32_t outH = opts.outHeight ? opts.outHeight : inH;

    const bool hdrOut  = (opts.mode == RtxConverter::Mode::Hdr ||
                          opts.mode == RtxConverter::Mode::VsrHdr);

    // Input codec must be one NvDecoder recognises.
    cudaVideoCodec nvdecCodec = cudaVideoCodec_NumCodecs;
    if (!codecStringToNvDec(opts.input.codecName, nvdecCodec))
    {
        result.errorDetail = "Unsupported input codec for NVDEC: " + opts.input.codecName;
        return bailout(result.errorDetail.c_str());
    }
    GUID encGuid{};
    if (!codecToEncGuid(opts.codec, encGuid))
    {
        result.errorDetail = "Unsupported output codec for NVENC: " + opts.codec;
        return bailout(result.errorDetail.c_str());
    }

    // Input bit depth: we currently support 8-bit (NV12) and 10-bit (P010)
    // decoded surfaces. 12-bit (P016) sources are extremely rare in practice
    // (mostly RAW intermediates) and would need a separate kernel path, so
    // reject them up-front rather than silently produce garbage downstream.
    //
    // The historical bug this guards against: NvDecoder always honours the
    // bitstream bit depth, so a 10-bit HEVC Main10 input gets handed back as
    // a P010 surface (2 bytes / sample). If we then blindly launch
    // launchNv12ToRgba8 on it, every 16-bit luma word is sliced into two
    // 8-bit "pixels" which look like uniform noise. RTX TrueHDR passes
    // through the noise; NVENC sees high-entropy input it can't compress
    // and emits ~4 MB per frame instead of ~50 KB, ballooning a 600 MB
    // input into a 22 GB output and stalling the muxer pipe.
    const int inBitDepth = opts.input.bitDepth > 0 ? opts.input.bitDepth : 8;
    if (inBitDepth != 8 && inBitDepth != 10)
    {
        result.errorDetail =
            "Unsupported input bit depth " + std::to_string(inBitDepth) +
            " (pix_fmt=" + opts.input.pixFmt + ").\n"
            "  Only 8-bit and 10-bit YUV 4:2:0 inputs are supported.\n"
            "  Re-encode the source to 8-bit (yuv420p / Main profile) first, e.g.:\n"
            "    ffmpeg -i IN.mp4 -c:v hevc_nvenc -profile:v main -pix_fmt nv12 "
            "-preset p5 -cq 18 -c:a copy IN_8bit.mp4";
        return bailout(result.errorDetail.c_str());
    }
    const bool srcIs10bit = (inBitDepth == 10);

    // ------------------------------------------------------------------
    // 1. CUDA context (shared between NvDecoder, kernels, RTX SDK, NvEncoder)
    // ------------------------------------------------------------------
    CUdevice  cuDev = 0;
    CUcontext cuCtx = nullptr;
    if (cuInit(0) != CUDA_SUCCESS)
        return bailout("cuInit failed (no CUDA driver?)");
    int gpuCount = 0;
    cuDeviceGetCount(&gpuCount);
    if (gpuCount <= 0)
        return bailout("No CUDA GPUs visible");
    if (cuDeviceGet(&cuDev, 0) != CUDA_SUCCESS)
        return bailout("cuDeviceGet failed");
#if CUDA_VERSION >= 13000
    if (cuCtxCreate(&cuCtx, nullptr, 0, cuDev) != CUDA_SUCCESS)
#else
    if (cuCtxCreate(&cuCtx, 0, cuDev) != CUDA_SUCCESS)
#endif
        return bailout("cuCtxCreate failed");

    // Dedicated non-default CUDA stream for our kernels + RTX SDK. Using the
    // legacy default stream (0) would *implicitly* synchronise with every
    // other stream in the context -- including the ones NvDecoder and
    // NvEncoderCuda use internally -- and serialise the whole pipeline on
    // hardware. A non-default stream lets NVDEC / compute / NVENC overlap.
    cudaStream_t stream = nullptr;
    if (cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking) != cudaSuccess)
    {
        cuCtxDestroy(cuCtx);
        return bailout("cudaStreamCreateWithFlags failed");
    }

    // ------------------------------------------------------------------
    // 2. Demuxer
    //
    // VFR PTS passthrough (#32): the demuxer's ffmpeg also writes every
    // packet's exact pts to a small side file (second -f framecrc output).
    // This replaces the old parallel ffprobe pass, which re-read the entire
    // source while the demuxer was reading it -- on slow/network drives the
    // two readers halved throughput until the probe finished.
    // ------------------------------------------------------------------
    const bool wantPtsPassthrough =
        opts.copyAudio && !input.empty() && opts.ptsPassthrough &&
        (opts.codec == "hevc" || opts.codec == "h265" || opts.codec == "h264");
    const std::string tsCapturePath =
        wantPtsPassthrough ? output + ".sdr2hdr.ts.txt" : std::string();

    BitstreamDemuxer demuxer;
    if (!demuxer.start(input, opts.input.codecName, opts.verbose, tsCapturePath))
    {
        if (stream) cudaStreamDestroy(stream);
        cuCtxDestroy(cuCtx);
        return bailout("Failed to start BitstreamDemuxer");
    }

    // ------------------------------------------------------------------
    // 3. NvDecoder (produces NV12 device frames, pitched).
    // ------------------------------------------------------------------
    std::unique_ptr<NvDecoder> dec;
    try
    {
        dec = std::make_unique<NvDecoder>(
            cuCtx,                        // cuContext
            /*bUseDeviceFrame*/ true,
            nvdecCodec,
            /*bLowLatency*/     false,
            /*bDeviceFramePitched*/ true,
            nullptr, nullptr,             // crop/resize
            false, 0, 0, 1000, false, 0, nullptr);
    }
    catch (const std::exception& e)
    {
        demuxer.finish();
        if (stream) cudaStreamDestroy(stream);
        cuCtxDestroy(cuCtx);
        result.errorDetail = std::string("NvDecoder ctor failed: ") + e.what();
        return bailout(result.errorDetail.c_str());
    }

    // ------------------------------------------------------------------
    // 4. NGX sessions (direct), or the RTX sample wrapper as fallback.
    //
    // The direct path creates `numSessions` independent NGX feature
    // instances, each bound to its own CUDA stream; consecutive frames are
    // striped across them so the inference -- the pipeline's bottleneck --
    // overlaps frame-to-frame (#11). The legacy RtxConverter wrapper stays
    // as a fallback: its deviceptr path stages through synchronous
    // cuMemcpy2D, which serialises the whole context every frame.
    // ------------------------------------------------------------------
    // Auto = 1: NGX hard-limits TrueHDR/VSR to one live feature instance per
    // process (FeatureAlreadyExists, verified on driver 610.47), so asking
    // for more just produces a failed create + fallback noise. The striping
    // machinery stays in place for a future multi-process sharding mode.
    int numSessions = opts.ngxSessions > 0 ? opts.ngxSessions : 1;
    if (numSessions > 4) numSessions = 4;

    std::vector<cudaStream_t> sessStream;
    std::vector<std::unique_ptr<NgxSession>> sessions;
    RtxConverter rtx;                 // fallback only
    bool ngxDirect = ngxRuntimeInit();

    auto destroySessions = [&]()
    {
        sessions.clear();             // releases NGX features + arrays
        for (cudaStream_t s : sessStream)
            if (s) cudaStreamDestroy(s);
        sessStream.clear();
        ngxRuntimeShutdown();
    };

    if (ngxDirect)
    {
        for (int i = 0; i < numSessions; ++i)
        {
            cudaStream_t s = nullptr;
            if (cudaStreamCreateWithFlags(&s, cudaStreamNonBlocking) != cudaSuccess)
                break;
            auto sess = std::make_unique<NgxSession>();
            if (!sess->init(opts.mode, inW, inH, outW, outH, cuCtx, s))
            {
                sess->shutdown();
                cudaStreamDestroy(s);
                break;
            }
            sessStream.push_back(s);
            sessions.push_back(std::move(sess));
        }
        if (sessions.empty())
        {
            ngxRuntimeShutdown();
            ngxDirect = false;
        }
    }
    numSessions = ngxDirect ? static_cast<int>(sessions.size()) : 1;

    if (!ngxDirect)
    {
        if (!rtx.initializeWithContext(opts.mode, inW, inH, outW, outH,
                                       cuCtx, stream, 0))
        {
            demuxer.finish();
            dec.reset();
            if (stream) cudaStreamDestroy(stream);
            cuCtxDestroy(cuCtx);
            return bailout("RtxConverter init failed");
        }
        sessStream.push_back(stream);   // kernels share the NVENC IO stream
    }

    // ------------------------------------------------------------------
    // 5. Intermediate RGBA8 device buffers (NGX input/output), one pair per
    //    session. Within a session the stream serialises reuse; across
    //    sessions the buffers are independent.
    // ------------------------------------------------------------------
    std::vector<DevBuffer> rgbaIn(numSessions), rgbaOut(numSessions);
    bool allocOk = true;
    for (int i = 0; i < numSessions; ++i)
        allocOk = allocOk && rgbaIn[i].alloc(inW, inH, 4)
                          && rgbaOut[i].alloc(outW, outH, 4);
    if (!allocOk)
    {
        if (ngxDirect) destroySessions(); else rtx.shutdown();
        demuxer.finish(); dec.reset();
        if (stream) cudaStreamDestroy(stream);
        cuCtxDestroy(cuCtx);
        return bailout("cuMemAlloc for RGBA scratch failed");
    }

    auto freeScratch = [&]()
    {
        for (auto& b : rgbaIn)  b.free_();
        for (auto& b : rgbaOut) b.free_();
    };
    auto shutdownConvert = [&]()
    {
        if (ngxDirect) destroySessions();
        else           rtx.shutdown();
    };

    // ------------------------------------------------------------------
    // 6. NvEncoder (HEVC Main10 P010 for HDR / NV12 for SDR VSR)
    // ------------------------------------------------------------------
    NV_ENC_BUFFER_FORMAT encFmt = hdrOut
        ? NV_ENC_BUFFER_FORMAT_YUV420_10BIT
        : NV_ENC_BUFFER_FORMAT_NV12;

    std::unique_ptr<NvEncoderCuda> enc;
    NV_ENC_INITIALIZE_PARAMS encInit{};
    NV_ENC_CONFIG            encCfg{};
    try
    {
        enc = std::make_unique<NvEncoderCuda>(cuCtx, outW, outH, encFmt, 3, false, false, false);
        configureEncoder(*enc, encInit, encCfg, opts, encGuid, outW, outH, hdrOut);
        enc->CreateEncoder(&encInit);

        // Hand NVENC our CUDA stream for both input (kernel-output) and
        // output (bitstream readback). Without this, NVENC reads input on
        // the default stream and we have to call cudaStreamSynchronize()
        // before every EncodeFrame to guarantee our kernel/NGX output is
        // visible -- which collapses the pipeline to serial per-frame
        // execution. With SetIOCudaStreams the driver inserts the right
        // cross-engine semaphores and we can queue N frames of work
        // without ever blocking the CPU.
        enc->SetIOCudaStreams(
            reinterpret_cast<NV_ENC_CUSTREAM_PTR>(&stream),
            reinterpret_cast<NV_ENC_CUSTREAM_PTR>(&stream));
    }
    catch (const std::exception& e)
    {
        freeScratch();
        shutdownConvert(); demuxer.finish(); dec.reset();
        if (stream) cudaStreamDestroy(stream);
        cuCtxDestroy(cuCtx);
        result.errorDetail = std::string("NvEncoder create failed: ") + e.what();
        return bailout(result.errorDetail.c_str());
    }

    // ------------------------------------------------------------------
    // 7. Muxer (receives encoded bitstream, muxes into container)
    // ------------------------------------------------------------------
    MuxerOptions mo;
    mo.codec     = opts.codec;
    mo.width     = outW;
    mo.height    = outH;
    mo.fps       = opts.input.fps > 0.1 ? opts.input.fps : 30.0;
    mo.fpsNum    = encInit.frameRateNum;
    mo.fpsDen    = encInit.frameRateDen;
    mo.hdr10     = hdrOut;
    mo.maxCll    = opts.maxLuminance;
    mo.copyAudio = opts.copyAudio;

    // Pipe mux + a second file input for audio is unreliable on Windows ffmpeg:
    // observed nb_streams=1 (MP4) and audio clustered at EOF (MKV). So when
    // audio is copied we mux in two passes: the encoded stream is piped into
    // a video-only temp MP4 *during* the encode (single pipe input ->
    // reliable; replaces the old raw-ES-to-disk + containerize passes, saving
    // two full read/writes of the multi-GB stream), then remuxCopyAudio()
    // merges the source audio. The temp MP4 is also where the VFR re-timing
    // (#32) is applied -- mp4_retime.cpp needs an MP4 with moov last.
    //
    // AV1 keeps the raw-ES path: the hevc/h264 pipe demuxers used by the
    // temp-MP4 route don't apply to OBU streams.
    const bool audioTwoPassMux =
        mo.copyAudio && !input.empty();
    const bool pipeTempMp4 =
        audioTwoPassMux && opts.codec != "av1";
    std::string tempVideoOnly;
    std::string muxTarget = output;
    if (audioTwoPassMux)
    {
        // Sweep stale temp artifacts a previous crashed/killed run may have
        // left next to this output (incl. names from older versions).
        for (const char* suffix : { ".sdr2hdr.vidonly.mp4",
                                    ".sdr2hdr.vidonly.hevc",
                                    ".sdr2hdr.vidonly.h264" })
        {
            std::error_code ec;
            std::filesystem::remove(output + suffix, ec);
            std::filesystem::remove(output + suffix + ".mux.mkv", ec);
            std::filesystem::remove(output + suffix + ".mux.mp4", ec);
        }

        if (pipeTempMp4)
        {
            tempVideoOnly = output + ".sdr2hdr.vidonly.mp4";
            muxTarget     = tempVideoOnly;
        }
        else
        {
            const char* esExt = (opts.codec == "h264") ? ".h264" : ".hevc";
            tempVideoOnly = output + ".sdr2hdr.vidonly" + esExt;
            mo.rawOutputPath = tempVideoOnly;
        }
        mo.copyAudio = false;
        if (opts.input.isVfr)
        {
            fprintf(stderr,
                    "Note: VFR source (nominal %.2f fps, average %.2f fps). "
                    "Two-pass mux at %u/%u fps + audio copy.\n",
                    opts.input.rFps, opts.input.avgFps, mo.fpsNum, mo.fpsDen);
        }
        else
        {
            printf("  mux: two-pass (video-only mp4 -> remux with audio copy)\n");
        }
    }

    BitstreamMuxer muxer;
    if (!muxer.start(input, muxTarget, mo, opts.verbose))
    {
        try { enc->DestroyEncoder(); } catch (...) {}
        enc.reset();
        freeScratch();
        shutdownConvert(); demuxer.finish(); dec.reset();
        if (stream) cudaStreamDestroy(stream);
        cuCtxDestroy(cuCtx);
        return bailout("Failed to start BitstreamMuxer");
    }

    // ------------------------------------------------------------------
    // 8. Three-stage threaded pipeline
    // ------------------------------------------------------------------
    //
    //   Thread A (demux+decode)   Thread B (process)         Thread C (mux)
    //   -----------------------   ------------------------   --------------
    //   demuxer.readChunk    -->  queueDec (NV12 ptrs)  -->  queueEnc (packets)
    //   NvDecoder::Decode                                    muxer.writeChunk
    //                             NV12 -> RGBA8 kernel
    //                             RTX SDK  (deviceptr)
    //                             RGBA8/ABGR10 -> NV12/P010
    //                             NvEncoder::EncodeFrame
    //
    // NVDEC (hardware), CUDA compute (SMs + RTX SDK inference), and NVENC
    // (hardware) are three physically independent engines on the GPU. Running
    // them serially on one thread capped us at ~33% NVENC / 13% NVDEC / 14%
    // compute because each engine waits for the other two. With three worker
    // threads driving three queues, each engine gets back-pressure only from
    // its own stage.
    //
    // Note: CUDA contexts are per-thread. Each worker must call
    // cuCtxSetCurrent(cuCtx) on entry, otherwise all CUDA calls return
    // CUDA_ERROR_INVALID_CONTEXT.
    // ------------------------------------------------------------------

    printf("GPU-only pipeline: NVDEC -> CUDA -> RTX SDK -> NVENC -> mux (3-thread)\n");
    if (ngxDirect)
        printf("  ngx: direct, %d session%s (frames striped across CUDA streams)\n",
               numSessions, numSessions > 1 ? "s" : "");
    else
        printf("  ngx: RTX sample wrapper fallback (single session, synchronous)\n");
    printf("  in:  %s  %ux%u  %.3f fps  %s  %d-bit (%s)\n",
           input.c_str(), inW, inH, opts.input.fps,
           opts.input.codecName.c_str(),
           inBitDepth,
           srcIs10bit ? "P010" : "NV12");
    printf("  out: %s  %ux%u  %s  %s\n",
           output.c_str(), outW, outH,
           opts.codec.c_str(), hdrOut ? "HDR10" : "SDR");
    fflush(stdout);

    // --- Frame descriptor passed from decode thread to process thread ---
    // We keep pointers into NvDecoder's internal ring. The process thread
    // MUST consume these in FIFO order before the ring wraps. NvDecoder's
    // default is 20 surfaces, so a queue depth of 4-8 is safe.
    // devPtr is a pointer into NvDecoder's surface pool, held via
    // GetLockedFrame() so the decoder will NOT overwrite it on subsequent
    // Decode() calls. It MUST be returned via UnlockFrame() after all
    // stream work reading from it has completed, otherwise the pool leaks
    // and eventually stalls.
    struct DecodedFrame { uint8_t* devPtr; int pitch; size_t frameIndex; };

    // --- Encoded packet passed to mux thread (owning buffer + source index) ---
    using PacketBytes = std::vector<uint8_t>;
    struct EncodedPacket { PacketBytes bytes; size_t frameIndex = 0; };

    // --- Bounded queues with blocking push/pop ---
    struct DecQueue
    {
        std::deque<DecodedFrame> q;
        std::mutex               m;
        std::condition_variable  cv;
        bool                     closed = false;
        size_t                   maxDepth = 6;
    };
    struct PktQueue
    {
        std::deque<EncodedPacket> q;
        std::mutex              m;
        std::condition_variable cv;
        bool                    closed = false;
        size_t                  maxDepth = 64;  // headroom; packets are small
    };
    DecQueue decQ;
    PktQueue pktQ;

    // Cross-stream sync: after a frame's last kernel on its session stream,
    // record an event and make the NVENC IO stream wait on it before
    // EncodeFrame. In-flight depth is bounded by NVENC's buffer ring (4), so
    // a pool of 8 reusable events can never alias a live frame.
    constexpr size_t kEvtPool = 8;
    std::vector<cudaEvent_t> sessEvt;
    if (ngxDirect)
    {
        sessEvt.resize(kEvtPool, nullptr);
        for (auto& e : sessEvt)
        {
            if (cudaEventCreateWithFlags(&e, cudaEventDisableTiming) != cudaSuccess)
            {
                for (auto& e2 : sessEvt) if (e2) cudaEventDestroy(e2);
                try { enc->DestroyEncoder(); } catch (...) {}
                enc.reset();
                freeScratch();
                shutdownConvert(); muxer.finish(); demuxer.finish(); dec.reset();
                if (stream) cudaStreamDestroy(stream);
                cuCtxDestroy(cuCtx);
                return bailout("cudaEventCreate failed");
            }
        }
    }

    std::atomic<bool> fatal(false);
    std::atomic<bool> cancelled(false);
    std::string       fatalMsg;
    std::mutex        fatalMx;
    auto setFatal = [&](const std::string& why)
    {
        std::lock_guard<std::mutex> g(fatalMx);
        if (!fatal.exchange(true))
            fatalMsg = why;
    };

    // Progress counters.
    std::atomic<uint64_t> framesDecoded(0);
    std::atomic<uint64_t> framesEncoded(0);

    // ------------------------------------------------------------------
    // Thread A: demux + NVDEC -> decQ
    // ------------------------------------------------------------------
    std::thread tDec([&]()
    {
        // NvDecoder reports every problem by THROWING (NVDECException) --
        // including from GetLockedFrame/UnlockFrame, not just Decode. An
        // exception escaping a std::thread calls std::terminate, which the
        // user sees as a silent 0xC0000409 crash with no message. Convert
        // everything to setFatal instead.
        try
        {

        // Attach this thread to the shared CUDA context.
        if (cuCtxSetCurrent(cuCtx) != CUDA_SUCCESS)
        {
            setFatal("decode thread: cuCtxSetCurrent failed");
            throw std::runtime_error("ctx");   // -> common close below
        }

        std::vector<uint8_t> pipeBuf(1 << 20);
        const bool ivfInput = demuxer.usesIvfFraming();
        IvfStreamParser      ivf;
        std::vector<uint8_t> ivfFrame;
        bool eof = false;

        auto decodeOneChunk = [&](const uint8_t* data, int size, int flags) -> bool
        {
            if (!size && !(flags & CUVID_PKT_ENDOFSTREAM))
                return true;
            int nReady = 0;
            try
            {
                nReady = dec->Decode(data, size, flags);
            }
            catch (const std::exception& e)
            {
                setFatal(std::string("NvDecoder::Decode threw: ") + e.what());
                return false;
            }
            for (int i = 0; i < nReady && !fatal.load(); ++i)
            {
                DecodedFrame df;
                df.devPtr     = dec->GetLockedFrame();
                df.pitch      = dec->GetDeviceFramePitch();
                df.frameIndex = static_cast<size_t>(framesDecoded.load());

                std::unique_lock<std::mutex> lk(decQ.m);
                decQ.cv.wait(lk, [&]
                {
                    return fatal.load() || decQ.q.size() < decQ.maxDepth;
                });
                if (fatal.load()) return false;
                decQ.q.push_back(df);
                ++framesDecoded;
                decQ.cv.notify_all();
            }
            return true;
        };

        if (ivfInput)
        {
            while (!eof && !fatal.load())
            {
                size_t n = demuxer.readChunk(pipeBuf.data(), pipeBuf.size());
                if (n == 0)
                {
                    eof = true;
                    ivf.setEof();
                }
                else
                {
                    ivf.append(pipeBuf.data(), n);
                }

                while (!fatal.load() && ivf.nextFrame(ivfFrame))
                {
                    if (!decodeOneChunk(ivfFrame.data(),
                                        static_cast<int>(ivfFrame.size()), 0))
                        break;
                }
            }
            if (!fatal.load())
                decodeOneChunk(nullptr, 0, CUVID_PKT_ENDOFSTREAM);
        }
        else
        {
            while (!eof && !fatal.load())
            {
                size_t n = demuxer.readChunk(pipeBuf.data(), pipeBuf.size());
                int    decFlags = 0;
                if (n == 0) { eof = true; decFlags = CUVID_PKT_ENDOFSTREAM; }

                if (!decodeOneChunk(n ? pipeBuf.data() : nullptr,
                                    static_cast<int>(n), decFlags))
                    break;
            }
        }

        }
        catch (const std::exception& e)
        {
            setFatal(std::string("decode thread: ") + e.what());
        }
        catch (...)
        {
            setFatal("decode thread: unknown exception");
        }

        std::lock_guard<std::mutex> g(decQ.m);
        decQ.closed = true;
        decQ.cv.notify_all();
    });

    // ------------------------------------------------------------------
    // Thread B: consume decQ, do kernels + RTX SDK + NVENC submit, push to pktQ
    // ------------------------------------------------------------------
    std::thread tProc([&]()
    {
        // Same throw-to-setFatal net as the decode thread: NvEncoder and
        // NvDecoder helpers (GetNextInputFrame / UnlockFrame / EndEncode)
        // throw on failure, and an escaped exception would terminate the
        // process without a message.
        try
        {

        if (cuCtxSetCurrent(cuCtx) != CUDA_SUCCESS)
        {
            setFatal("process thread: cuCtxSetCurrent failed");
            throw std::runtime_error("ctx");
        }

        std::vector<NvEncOutputFrame> encOut;
        std::deque<size_t> encFrameOrder;
        auto drainEncoderOutput = [&](bool isFlush)
        {
            for (auto& pkt : encOut)
            {
                EncodedPacket ep;
                ep.bytes.assign(pkt.frame.begin(), pkt.frame.end());
                if (!encFrameOrder.empty())
                {
                    ep.frameIndex = encFrameOrder.front();
                    encFrameOrder.pop_front();
                }
                std::unique_lock<std::mutex> lk(pktQ.m);
                pktQ.cv.wait(lk, [&]
                {
                    return fatal.load() || pktQ.q.size() < pktQ.maxDepth;
                });
                if (fatal.load()) return;
                pktQ.q.emplace_back(std::move(ep));
                pktQ.cv.notify_all();
            }
            encOut.clear();
            (void)isFlush;
        };

        // Sliding window of locked decoder surfaces. We defer UnlockFrame()
        // until the frame has travelled at least `kUnlockDelay` positions
        // through the pipeline, which guarantees its kernel reads have
        // completed on the stream. NvEncoder's internal output-delay is
        // m_nOutputDelay (= 3 with nExtraOutputDelay=3 + no B-frames); we
        // add a safety margin on top.
        //
        // 8 is well within NvDecoder's default surface pool (~20-32) so
        // holding that many locked frames never exhausts the decoder.
        constexpr size_t kUnlockDelay = 8;
        std::deque<uint8_t*> lockedFrames;
        auto unlockOldFrames = [&](bool flushAll)
        {
            const size_t keep = flushAll ? 0 : kUnlockDelay;
            while (lockedFrames.size() > keep)
            {
                uint8_t* p = lockedFrames.front();
                lockedFrames.pop_front();
                if (p) dec->UnlockFrame(&p);
            }
        };

        while (!fatal.load())
        {
            DecodedFrame df;
            bool haveFrame = false;
            {
                std::unique_lock<std::mutex> lk(decQ.m);
                decQ.cv.wait(lk, [&]
                {
                    return fatal.load() || !decQ.q.empty() || decQ.closed;
                });
                if (fatal.load()) break;
                if (!decQ.q.empty())
                {
                    df = decQ.q.front();
                    decQ.q.pop_front();
                    decQ.cv.notify_all();
                    haveFrame = true;
                }
                else if (decQ.closed)
                {
                    break;  // no more input
                }
            }
            if (!haveFrame) continue;

            // Register this locked surface; we'll unlock it after it's far
            // enough back in the pipeline to guarantee its kernel reads
            // have finished.
            lockedFrames.push_back(df.devPtr);

            // Stripe consecutive frames across the NGX sessions: all of this
            // frame's compute work goes on its session's stream, so frame N
            // (session 0) and frame N+1 (session 1) overlap on the GPU. With
            // the fallback wrapper there is one stream and this degenerates
            // to the old serial behaviour.
            const size_t w = df.frameIndex % static_cast<size_t>(numSessions);
            cudaStream_t cs = sessStream[w];

            // 8a. Decoder surface -> RGBA8 (tightly packed).
            //
            // Pick the kernel based on the bit depth we probed up front:
            //   8-bit  -> NvDecoder gave us NV12 (1 byte / sample)
            //   10-bit -> NvDecoder gave us P010 (2 bytes / sample, top 10 bits valid)
            //
            // For 10-bit content we still treat the YUV matrix as the
            // limited-range BT.709 the BSF-rewritten VUI claims (or what
            // Auto-HDR / DVR captures actually encode), so the SDR-side
            // path of RTX TrueHDR sees a sensible image. For genuine
            // BT.2020 PQ HDR inputs (already HDR), the user shouldn't be
            // running SDR-to-HDR conversion in the first place.
            cudaError_t ke;
            if (srcIs10bit)
            {
                ke = kernels::launchP010ToRgba8(
                    df.devPtr, df.pitch,
                    reinterpret_cast<void*>(rgbaIn[w].ptr), rgbaIn[w].pitch,
                    static_cast<int>(inW), static_cast<int>(inH),
                    /*bt2020*/   false,
                    /*fullRange*/ false,
                    cs);
            }
            else
            {
                ke = kernels::launchNv12ToRgba8(
                    df.devPtr, df.pitch,
                    reinterpret_cast<void*>(rgbaIn[w].ptr), rgbaIn[w].pitch,
                    static_cast<int>(inW), static_cast<int>(inH),
                    /*bt601*/ (inH <= 576),
                    /*fullRange*/ false,
                    cs);
            }
            if (ke != cudaSuccess)
            {
                setFatal(std::string(srcIs10bit ? "launchP010ToRgba8"
                                                : "launchNv12ToRgba8")
                         + " failed: " + cudaGetErrorString(ke));
                break;
            }

            // 8b. NGX inference, ordered behind the kernel on the same
            // stream. The direct path only *enqueues* (async staging copies
            // + NGX eval on the session stream); the fallback wrapper blocks
            // the host on synchronous cuMemcpy2D, exactly as before.
            if (ngxDirect)
            {
                if (!sessions[w]->evaluateAsync(
                        static_cast<unsigned long long>(rgbaIn[w].ptr),
                        static_cast<unsigned long long>(rgbaOut[w].ptr),
                        opts.rtx))
                {
                    setFatal("NGX evaluate failed.");
                    break;
                }
            }
            else if (!rtx.convertFrameDevicePtr(
                    static_cast<unsigned long long>(rgbaIn[w].ptr),
                    static_cast<unsigned long long>(rgbaOut[w].ptr),
                    opts.rtx))
            {
                setFatal("RTX deviceptr evaluate failed.");
                break;
            }

            // 8c. Map NVENC input & convert to P010/NV12.
            const NvEncInputFrame* nvin = enc->GetNextInputFrame();
            CUdeviceptr encY     = reinterpret_cast<CUdeviceptr>(nvin->inputPtr);
            uint32_t    encPitch = nvin->pitch;
            CUdeviceptr encUV    = encY + nvin->chromaOffsets[0];

            if (hdrOut)
                ke = kernels::launchAbgr2101010ToP010(
                    reinterpret_cast<void*>(rgbaOut[w].ptr), rgbaOut[w].pitch,
                    reinterpret_cast<void*>(encY),  encPitch,
                    reinterpret_cast<void*>(encUV), encPitch,
                    static_cast<int>(outW), static_cast<int>(outH),
                    cs);
            else
                ke = kernels::launchRgba8ToNv12(
                    reinterpret_cast<void*>(rgbaOut[w].ptr), rgbaOut[w].pitch,
                    reinterpret_cast<void*>(encY),  encPitch,
                    reinterpret_cast<void*>(encUV), encPitch,
                    static_cast<int>(outW), static_cast<int>(outH),
                    cs);
            if (ke != cudaSuccess)
            {
                setFatal(std::string("format-convert kernel failed: ")
                         + cudaGetErrorString(ke));
                break;
            }

            // NVENC reads its input on `stream` (SetIOCudaStreams); make it
            // wait for this frame's session-stream work before encoding.
            if (ngxDirect)
            {
                cudaEvent_t e = sessEvt[df.frameIndex % kEvtPool];
                if (cudaEventRecord(e, cs) != cudaSuccess ||
                    cudaStreamWaitEvent(stream, e, 0) != cudaSuccess)
                {
                    setFatal("cross-stream event sync failed");
                    break;
                }
            }

            // No cudaStreamSynchronize here. EncodeFrame() calls nvEncEncodePicture
            // under the hood, which the driver has already tied to our CUDA
            // stream via SetIOCudaStreams() at init time: NVENC waits for the
            // upstream kernel + NGX work to finish on the input side and
            // signals completion on the output side, all through stream
            // events instead of CPU syncs. This is what lets the 3-thread
            // pipeline actually overlap decode / compute / encode instead
            // of collapsing to serial ~120fps.
            try
            {
                encFrameOrder.push_back(df.frameIndex);
                enc->EncodeFrame(encOut);
            }
            catch (const std::exception& e)
            {
                setFatal(std::string("NvEncoder::EncodeFrame failed: ") + e.what());
                break;
            }
            ++framesEncoded;
            drainEncoderOutput(/*isFlush*/ false);

            // Now that EncodeFrame() returned (which internally waited for
            // frame N-m_nOutputDelay's encode-completion event), the kernel
            // reads for frames older than that are guaranteed done too.
            // Unlock any decoder surfaces that have aged out of the window.
            unlockOldFrames(/*flushAll=*/ false);

            if (fatal.load()) break;
        }

        // Flush encoder (only if no prior fatal error).
        if (!fatal.load())
        {
            try
            {
                encOut.clear();
                enc->EndEncode(encOut);
                drainEncoderOutput(/*isFlush*/ true);
            }
            catch (const std::exception& e)
            {
                setFatal(std::string("NvEncoder::EndEncode failed: ") + e.what());
            }
        }

        // After EndEncode returns, every submitted frame has been encoded,
        // which means every kernel on the session streams has also completed.
        // Safe to return ALL remaining locked decoder surfaces to the pool.
        cudaStreamSynchronize(stream);
        for (cudaStream_t s : sessStream)
            if (s && s != stream) cudaStreamSynchronize(s);
        unlockOldFrames(/*flushAll=*/ true);

        }
        catch (const std::exception& e)
        {
            setFatal(std::string("process thread: ") + e.what());
        }
        catch (...)
        {
            setFatal("process thread: unknown exception");
        }

        std::lock_guard<std::mutex> g(pktQ.m);
        pktQ.closed = true;
        pktQ.cv.notify_all();
    });

    // ------------------------------------------------------------------
    // Thread C: consume pktQ, write to muxer ffmpeg stdin pipe.
    // ------------------------------------------------------------------
    std::thread tMux([&]()
    {
        try
        {

        while (!fatal.load())
        {
            EncodedPacket ep;
            bool havePkt = false;
            {
                std::unique_lock<std::mutex> lk(pktQ.m);
                pktQ.cv.wait(lk, [&]
                {
                    return fatal.load() || !pktQ.q.empty() || pktQ.closed;
                });
                if (fatal.load()) break;
                if (!pktQ.q.empty())
                {
                    ep = std::move(pktQ.q.front());
                    pktQ.q.pop_front();
                    pktQ.cv.notify_all();
                    havePkt = true;
                }
                else if (pktQ.closed)
                {
                    break;
                }
            }
            if (!havePkt) continue;
            if (!muxer.writeChunk(ep.bytes.data(), ep.bytes.size(), ep.frameIndex))
            {
                setFatal("muxer pipe write failed (ffmpeg muxer exited?)");
                break;
            }
        }

        }
        catch (const std::exception& e)
        {
            setFatal(std::string("mux thread: ") + e.what());
        }
        catch (...)
        {
            setFatal("mux thread: unknown exception");
        }
    });

    // ------------------------------------------------------------------
    // Main thread: progress UI + reaper
    // ------------------------------------------------------------------
    auto tLast = std::chrono::steady_clock::now();
    while (true)
    {
        // Done when process thread has closed the packet queue AND mux
        // thread has drained it.
        bool procDone;
        bool muxDone;
        {
            std::lock_guard<std::mutex> g(pktQ.m);
            procDone = pktQ.closed;
            muxDone  = pktQ.q.empty();
        }
        if (fatal.load()) break;
        if (procDone && muxDone) break;

        if (opts.cancelFlag && opts.cancelFlag->load())
        {
            cancelled.store(true);
            setFatal("Cancelled");
            // Wake every worker; their waits all re-check `fatal`.
            { std::lock_guard<std::mutex> g(decQ.m); decQ.cv.notify_all(); }
            { std::lock_guard<std::mutex> g(pktQ.m); pktQ.cv.notify_all(); }
            break;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(
                now - tLast).count() >= 250)
        {
            double secs = std::chrono::duration<double>(now - tStart).count();
            uint64_t enc = framesEncoded.load();
            double fps   = secs > 0 ? enc / secs : 0.0;
            double pct   = opts.input.frameCount > 0
                         ? 100.0 * static_cast<double>(enc)
                                 / static_cast<double>(opts.input.frameCount)
                         : 0.0;
            printf("\r  Frame %llu / %lld  (%.1f%%, %.1f fps)   ",
                   static_cast<unsigned long long>(enc),
                   static_cast<long long>(opts.input.frameCount),
                   pct, fps);
            fflush(stdout);
            if (opts.onProgress)
            {
                GpuPipelineOptions::ProgressUpdate pu;
                pu.framesDone  = enc;
                pu.framesTotal = static_cast<uint64_t>(
                    std::max<int64_t>(0, opts.input.frameCount));
                pu.elapsedSec  = secs;
                pu.fps         = fps;
                pu.percent     = pct;
                opts.onProgress(pu);
            }
            tLast = now;
        }
    }

    // Make sure worker threads see fatal=true if we bailed early, so their
    // waits unblock and they can exit cleanly.
    if (fatal.load())
    {
        { std::lock_guard<std::mutex> g(decQ.m);  decQ.cv.notify_all(); }
        { std::lock_guard<std::mutex> g(pktQ.m);  pktQ.cv.notify_all(); }
    }

    if (tDec.joinable())  tDec.join();
    if (tProc.joinable()) tProc.join();
    if (tMux.joinable())  tMux.join();

    result.framesIn  = framesDecoded.load();
    result.framesOut = framesEncoded.load();
    printf("\n");

    // Frames are all encoded (bar at 100%), but the audio mux + A/V verify
    // still run below and can take a while on a large file. Signal the UI so it
    // doesn't look frozen at 100%. Only when we're actually merging audio —
    // video-only output finalizes near-instantly and needs no banner.
    if (opts.onProgress && audioTwoPassMux)
    {
        GpuPipelineOptions::ProgressUpdate pu;
        pu.framesDone  = framesEncoded.load();
        pu.framesTotal = static_cast<uint64_t>(
            std::max<int64_t>(0, opts.input.frameCount));
        pu.percent     = 100.0;
        pu.finalizing  = true;
        opts.onProgress(pu);
    }

    bool pumpError = fatal.load();
    if (pumpError && !fatalMsg.empty())
        fprintf(stderr, "%s\n", fatalMsg.c_str());

    // ------------------------------------------------------------------
    // 9. Teardown (encoder was already flushed inside tProc).
    // ------------------------------------------------------------------
    if (pumpError)
    {
        demuxer.dumpStderrTail("ffmpeg demuxer");
        muxer.dumpStderrTail("ffmpeg muxer");
    }

    try { enc->DestroyEncoder(); } catch (...) {}
    enc.reset();
    for (cudaEvent_t e : sessEvt)
        if (e) cudaEventDestroy(e);
    freeScratch();
    if (ngxDirect)
    {
        destroySessions();
    }
    else
    {
        rtx.shutdown();
        sessStream.clear();   // held the NVENC stream; destroyed below
    }
    muxer.finish();
    demuxer.finish();
    dec.reset();
    if (stream) cudaStreamDestroy(stream);
    cuCtxDestroy(cuCtx);

    // Cancel arriving between the last frame and the audio merge: skip the
    // merge entirely; the cleanup below removes the temp and the output.
    if (!pumpError && opts.cancelFlag && opts.cancelFlag->load())
    {
        cancelled.store(true);
        pumpError = true;
        if (fatalMsg.empty()) fatalMsg = "Cancelled";
    }

    if (!pumpError && audioTwoPassMux)
    {
        // Per-frame source timestamps are usable only when the decoder kept
        // a strict 1:1 frame mapping (IPPP output preserves display order,
        // so output frame i corresponds to the i-th displayed source frame).
        // The capture file is complete here: the demuxer ffmpeg has exited
        // (demuxer.finish() above waited for it).
        FramePtsInfo srcFramePts;
        const FramePtsInfo* framePts = nullptr;
        if (wantPtsPassthrough)
        {
            if (parseFrameCrcPts(tsCapturePath, srcFramePts) &&
                srcFramePts.pts.size() == framesEncoded.load())
            {
                framePts = &srcFramePts;
            }
            else
            {
                fprintf(stderr,
                        "Note: VFR passthrough unavailable (%zu source "
                        "timestamps, %llu encoded frames); using CFR "
                        "%u/%u fps.\n",
                        srcFramePts.pts.size(),
                        static_cast<unsigned long long>(framesEncoded.load()),
                        mo.fpsNum, mo.fpsDen);
            }
        }

        printf("  mux: copying audio from source...\n");
        if (!remuxCopyAudio(tempVideoOnly, input, output, opts.codec,
                            mo.fpsNum, mo.fpsDen, hdrOut, opts.verbose,
                            framePts))
        {
            pumpError = true;
            if (fatalMsg.empty())
                fatalMsg = "Failed to merge audio from source after encode";
        }
    }
    if (!tempVideoOnly.empty())
    {
        std::error_code ec;
        std::filesystem::remove(tempVideoOnly, ec);
    }
    if (!tsCapturePath.empty())
    {
        std::error_code ec;
        std::filesystem::remove(tsCapturePath, ec);
    }
    // A failed two-pass merge leaves a broken output; a cancelled run leaves
    // a truncated one (whether the muxer wrote it directly or not). Neither
    // is playable -- remove rather than leave junk behind.
    if (pumpError && (audioTwoPassMux || cancelled.load()))
    {
        std::error_code ec;
        std::filesystem::remove(output, ec);
    }

    auto tEnd = std::chrono::steady_clock::now();
    result.seconds = std::chrono::duration<double>(tEnd - tStart).count();
    result.ok = !pumpError;
    if (pumpError && result.errorDetail.empty())
        result.errorDetail = fatalMsg.empty() ? "pipeline error" : fatalMsg;
    return result;
}

} // namespace sdr2hdr
