// GPU-only pipeline implementation. See gpu_pipeline.h for the high-level
// data flow diagram.
//
// NOTE: we include CUDA / NvDecoder / NvEncoder headers only here, inside a
// .cpp file, so the rest of sdr2hdr doesn't pay the compile-time cost nor
// the transitive #define pollution.

#include "gpu_pipeline.h"
#include "bitstream_pipe.h"
#include "cuda_kernels.h"

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

    // Derive frameRateNum/Den from fps (round to sensible rational).
    double fps = opts.input.fps > 0.1 ? opts.input.fps : 30.0;
    // Heuristic: common frame rates
    auto setRational = [&](uint32_t num, uint32_t den)
    {
        initParams.frameRateNum = num;
        initParams.frameRateDen = den;
    };
    if      (fabs(fps - 23.976) < 0.05) setRational(24000, 1001);
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

        if (hdrOut)
        {
            // Don't flip outputMaxCll / outputMasteringDisplay without also
            // supplying per-pic pMaxCll / pMasteringDisplay payloads; some
            // driver versions will either reject the config or write a
            // malformed empty SEI. VUI below is what HDR10 players actually
            // key off in practice.
            auto& vui = hevc.hevcVUIParameters;
            vui.videoSignalTypePresentFlag   = 1;
            vui.videoFullRangeFlag           = 0;           // limited range
            vui.colourDescriptionPresentFlag = 1;
            vui.colourPrimaries              = NV_ENC_VUI_COLOR_PRIMARIES_BT2020;
            vui.transferCharacteristics      = NV_ENC_VUI_TRANSFER_CHARACTERISTIC_SMPTE2084;
            vui.colourMatrix                 = NV_ENC_VUI_MATRIX_COEFFS_BT2020_NCL;
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
    if (cuCtxCreate(&cuCtx, 0, cuDev) != CUDA_SUCCESS)
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
    // ------------------------------------------------------------------
    BitstreamDemuxer demuxer;
    if (!demuxer.start(input, opts.input.codecName, opts.verbose))
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
    // 4. RTX converter (uses our context)
    // ------------------------------------------------------------------
    RtxConverter rtx;
    if (!rtx.initializeWithContext(opts.mode, inW, inH, outW, outH,
                                   cuCtx, stream, 0))
    {
        demuxer.finish();
        dec.reset();
        if (stream) cudaStreamDestroy(stream);
        cuCtxDestroy(cuCtx);
        return bailout("RtxConverter init failed");
    }

    // ------------------------------------------------------------------
    // 5. Intermediate RGBA8 device buffers (RTX SDK input/output)
    // ------------------------------------------------------------------
    DevBuffer rgbaIn, rgbaOut;
    if (!rgbaIn.alloc(inW, inH, 4) ||
        !rgbaOut.alloc(outW, outH, 4))
    {
        rtx.shutdown(); demuxer.finish(); dec.reset();
        if (stream) cudaStreamDestroy(stream);
        cuCtxDestroy(cuCtx);
        return bailout("cuMemAlloc for RGBA scratch failed");
    }

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
        rgbaIn.free_(); rgbaOut.free_();
        rtx.shutdown(); demuxer.finish(); dec.reset();
        if (stream) cudaStreamDestroy(stream);
        cuCtxDestroy(cuCtx);
        result.errorDetail = std::string("NvEncoder create failed: ") + e.what();
        return bailout(result.errorDetail.c_str());
    }

    // ------------------------------------------------------------------
    // 7. Muxer (receives Annex-B bitstream over stdin, muxes into opts.codec container)
    // ------------------------------------------------------------------
    MuxerOptions mo;
    mo.codec     = opts.codec;
    mo.width     = outW;
    mo.height    = outH;
    mo.fps       = opts.input.fps > 0.1 ? opts.input.fps : 30.0;
    // Pass the exact rational NVENC just encoded with, so the mp4 muxer
    // stamps identical container timestamps. Anything less precise (e.g.
    // a double like 119.997) drifts from the VUI baked into the bitstream
    // and can cause visible micro-stutter during playback.
    mo.fpsNum    = encInit.frameRateNum;
    mo.fpsDen    = encInit.frameRateDen;
    mo.hdr10     = hdrOut;
    mo.maxCll    = opts.maxLuminance;
    mo.copyAudio = opts.copyAudio;
    BitstreamMuxer muxer;
    if (!muxer.start(input, output, mo, opts.verbose))
    {
        try { enc->DestroyEncoder(); } catch (...) {}
        enc.reset();
        rgbaIn.free_(); rgbaOut.free_();
        rtx.shutdown(); demuxer.finish(); dec.reset();
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
    struct DecodedFrame { uint8_t* devPtr; int pitch; };

    // --- Encoded packet descriptor passed to mux thread ---
    // NvEncoder reuses its internal bitstream buffers across frames, so we
    // copy the packet bytes into our own std::vector before handing to the
    // mux thread. At ~10 MB/s average this is cheap.
    using PacketBytes = std::vector<uint8_t>;

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
        std::deque<PacketBytes> q;
        std::mutex              m;
        std::condition_variable cv;
        bool                    closed = false;
        size_t                  maxDepth = 64;  // headroom; packets are small
    };
    DecQueue decQ;
    PktQueue pktQ;

    std::atomic<bool> fatal(false);
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
        // Attach this thread to the shared CUDA context.
        if (cuCtxSetCurrent(cuCtx) != CUDA_SUCCESS)
        {
            setFatal("decode thread: cuCtxSetCurrent failed");
            std::lock_guard<std::mutex> g(decQ.m);
            decQ.closed = true; decQ.cv.notify_all();
            return;
        }

        std::vector<uint8_t> pipeBuf(1 << 20);
        bool eof = false;
        while (!eof && !fatal.load())
        {
            size_t n = demuxer.readChunk(pipeBuf.data(), pipeBuf.size());
            int    decFlags = 0;
            if (n == 0) { eof = true; decFlags = CUVID_PKT_ENDOFSTREAM; }

            int nReady = 0;
            try
            {
                nReady = dec->Decode(pipeBuf.data(),
                                     static_cast<int>(n), decFlags);
            }
            catch (const std::exception& e)
            {
                setFatal(std::string("NvDecoder::Decode threw: ") + e.what());
                break;
            }

            for (int i = 0; i < nReady && !fatal.load(); ++i)
            {
                DecodedFrame df;
                // GetLockedFrame (not GetFrame!): without the lock, NvDecoder
                // will reuse the underlying surface on the NEXT Decode() once
                // its output queue wraps around, which silently corrupts
                // frames that Thread B hasn't consumed yet. The symptom is a
                // "skipped/duplicated frame" every few seconds -- exactly
                // what the user saw. Thread B is responsible for calling
                // dec->UnlockFrame() once all stream work that touches this
                // pointer has completed (we defer unlock by m_nOutputDelay+1
                // frames below, so NVENC's internal frame done is a safe
                // upper bound on kernel completion).
                df.devPtr = dec->GetLockedFrame();
                df.pitch  = dec->GetDeviceFramePitch();

                std::unique_lock<std::mutex> lk(decQ.m);
                decQ.cv.wait(lk, [&]
                {
                    return fatal.load() || decQ.q.size() < decQ.maxDepth;
                });
                if (fatal.load()) break;
                decQ.q.push_back(df);
                ++framesDecoded;
                decQ.cv.notify_all();
            }
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
        if (cuCtxSetCurrent(cuCtx) != CUDA_SUCCESS)
        {
            setFatal("process thread: cuCtxSetCurrent failed");
            std::lock_guard<std::mutex> g(pktQ.m);
            pktQ.closed = true; pktQ.cv.notify_all();
            return;
        }

        std::vector<NvEncOutputFrame> encOut;
        auto drainEncoderOutput = [&](bool isFlush)
        {
            for (auto& pkt : encOut)
            {
                // Copy into owning buffer; NvEncoder may recycle pkt.frame.
                PacketBytes bytes(pkt.frame.begin(), pkt.frame.end());
                std::unique_lock<std::mutex> lk(pktQ.m);
                pktQ.cv.wait(lk, [&]
                {
                    return fatal.load() || pktQ.q.size() < pktQ.maxDepth;
                });
                if (fatal.load()) return;
                pktQ.q.emplace_back(std::move(bytes));
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

            // 8a. Decoder surface -> RGBA8 (tightly packed) on our stream.
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
                    reinterpret_cast<void*>(rgbaIn.ptr), rgbaIn.pitch,
                    static_cast<int>(inW), static_cast<int>(inH),
                    /*bt2020*/   false,
                    /*fullRange*/ false,
                    stream);
            }
            else
            {
                ke = kernels::launchNv12ToRgba8(
                    df.devPtr, df.pitch,
                    reinterpret_cast<void*>(rgbaIn.ptr), rgbaIn.pitch,
                    static_cast<int>(inW), static_cast<int>(inH),
                    /*bt601*/ (inH <= 576),
                    /*fullRange*/ false,
                    stream);
            }
            if (ke != cudaSuccess)
            {
                setFatal(std::string(srcIs10bit ? "launchP010ToRgba8"
                                                : "launchNv12ToRgba8")
                         + " failed: " + cudaGetErrorString(ke));
                break;
            }

            // Kernel queued on `stream`; RTX SDK below is told about the
            // same stream at init time, so it will naturally serialise
            // with our write above inside its own stream dependencies.

            // 8b. RTX SDK (device ptrs).
            if (!rtx.convertFrameDevicePtr(
                    static_cast<unsigned long long>(rgbaIn.ptr),
                    static_cast<unsigned long long>(rgbaOut.ptr),
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
                    reinterpret_cast<void*>(rgbaOut.ptr), rgbaOut.pitch,
                    reinterpret_cast<void*>(encY),  encPitch,
                    reinterpret_cast<void*>(encUV), encPitch,
                    static_cast<int>(outW), static_cast<int>(outH),
                    stream);
            else
                ke = kernels::launchRgba8ToNv12(
                    reinterpret_cast<void*>(rgbaOut.ptr), rgbaOut.pitch,
                    reinterpret_cast<void*>(encY),  encPitch,
                    reinterpret_cast<void*>(encUV), encPitch,
                    static_cast<int>(outW), static_cast<int>(outH),
                    stream);
            if (ke != cudaSuccess)
            {
                setFatal(std::string("format-convert kernel failed: ")
                         + cudaGetErrorString(ke));
                break;
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
        // which means every kernel on the stream has also completed.
        // Safe to return ALL remaining locked decoder surfaces to the pool.
        cudaStreamSynchronize(stream);
        unlockOldFrames(/*flushAll=*/ true);

        std::lock_guard<std::mutex> g(pktQ.m);
        pktQ.closed = true;
        pktQ.cv.notify_all();
    });

    // ------------------------------------------------------------------
    // Thread C: consume pktQ, write to muxer ffmpeg stdin pipe.
    // ------------------------------------------------------------------
    std::thread tMux([&]()
    {
        while (!fatal.load())
        {
            PacketBytes bytes;
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
                    bytes = std::move(pktQ.q.front());
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
            if (!muxer.writeChunk(bytes.data(), bytes.size()))
            {
                setFatal("muxer pipe write failed (ffmpeg muxer exited?)");
                break;
            }
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
    rgbaIn.free_();
    rgbaOut.free_();
    rtx.shutdown();
    muxer.finish();
    demuxer.finish();
    dec.reset();
    if (stream) cudaStreamDestroy(stream);
    cuCtxDestroy(cuCtx);

    auto tEnd = std::chrono::steady_clock::now();
    result.seconds = std::chrono::duration<double>(tEnd - tStart).count();
    result.ok = !pumpError;
    return result;
}

} // namespace sdr2hdr
