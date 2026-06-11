#include "engine.h"
#include "gpu_pipeline.h"
#include "path_utils.h"
#include "i18n.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <vector>

namespace fs = std::filesystem;

namespace sdr2hdr {

namespace {

using i18n::tr;

void reportProgress(const ProcessProgressCallback& cb,
                    uint64_t frameIdx,
                    int64_t frameTotal,
                    std::chrono::steady_clock::time_point tStart)
{
    if (!cb) return;
    auto now = std::chrono::steady_clock::now();
    double secs = std::chrono::duration<double>(now - tStart).count();
    ProcessProgress p;
    p.framesDone  = frameIdx;
    p.framesTotal = frameTotal > 0 ? static_cast<uint64_t>(frameTotal) : 0;
    p.elapsedSec  = secs;
    p.fps         = secs > 0 ? static_cast<double>(frameIdx) / secs : 0.0;
    p.percent     = frameTotal > 0
                  ? 100.0 * static_cast<double>(frameIdx)
                          / static_cast<double>(frameTotal)
                  : 0.0;
    cb(p);
}

} // namespace

void applyDefaultQuality(ProcessOptions& opts)
{
    if (opts.quality < 0 && !opts.qualityAuto)
        opts.quality = (opts.backend == "nvenc") ? 19 : 18;
}

ProcessResult processFile(const std::string& inputIn,
                          const std::string& outputIn,
                          const ProcessOptions& opts,
                          ProcessProgressCallback onProgress,
                          std::atomic<bool>* cancelFlag)
{
    ProcessResult result;
    std::string input  = inputIn;
    std::string output = outputIn;

    {
        std::string inExt  = extOf(input);
        std::string outExt = extOf(output);
        if (!inExt.empty() && inExt != outExt)
            output = replaceExt(output, inExt);
    }

    {
        std::string curExt = extOf(output);
        if (curExt == "webm" && opts.codec != "av1")
            output = replaceExt(output, "mp4");
    }

    VideoInfo info;
    if (!probeVideo(input, info))
    {
        result.exitCode = 2;
        result.errorDetail = "Failed to probe input video.";
        return result;
    }

    auto roundEven = [](double v) {
        auto u = static_cast<uint32_t>(v + 0.5);
        return u & ~1u;
    };

    uint32_t outW = info.width;
    uint32_t outH = info.height;
    if (opts.mode == RtxConverter::Mode::Vsr || opts.mode == RtxConverter::Mode::VsrHdr)
    {
        if (opts.outputSizeSet)
        {
            outW = opts.outW;
            outH = opts.outH;
        }
        else if (opts.targetHeight)
        {
            outH = opts.targetHeight & ~1u;
            outW = roundEven(info.width * static_cast<double>(opts.targetHeight)
                                        / static_cast<double>(info.height));
        }
        else if (opts.scaleSet)
        {
            outW = roundEven(info.width  * opts.scale);
            outH = roundEven(info.height * opts.scale);
        }
        else
        {
            const uint32_t defaultH = 2160;
            outH = defaultH;
            outW = roundEven(info.width * static_cast<double>(defaultH)
                                        / static_cast<double>(info.height));
        }
    }

    const bool wantsHdrOutEff =
        (opts.mode == RtxConverter::Mode::Hdr ||
         opts.mode == RtxConverter::Mode::VsrHdr);
    int effectiveQuality;
    if (opts.qualityAuto && info.bitRateBps > 0)
    {
        effectiveQuality = recommendQuality(
            info.bitRateBps, outW, outH, info.fps, wantsHdrOutEff, /*keepRatio=*/0.75);
    }
    else if (opts.quality >= 0)
    {
        effectiveQuality = opts.quality;
    }
    else
    {
        effectiveQuality = (opts.backend == "nvenc") ? 19 : 18;
    }

    bool useGpuOnly = opts.gpuOnly && opts.backend == "nvenc";
    if (useGpuOnly &&
        info.codecName != "h264" && info.codecName != "avc"  &&
        info.codecName != "hevc" && info.codecName != "h265" &&
        info.codecName != "av1")
    {
        useGpuOnly = false;
    }

    if (useGpuOnly)
    {
        GpuPipelineOptions gopts;
        gopts.mode        = opts.mode;
        gopts.input       = info;
        gopts.outWidth    = outW;
        gopts.outHeight   = outH;
        gopts.codec       = opts.codec;
        gopts.rtx.contrast   = opts.contrast;
        gopts.rtx.saturation = opts.saturation;
        gopts.rtx.middleGray = opts.middleGray;
        gopts.rtx.maxLum     = opts.maxLum;
        gopts.rtx.vsrQuality = opts.vsrQuality;
        gopts.maxLuminance = opts.maxLum;
        gopts.copyAudio    = opts.copyAudio;
        gopts.ptsPassthrough = opts.vfrPts;
        gopts.ngxSessions  = opts.ngxSessions;
        gopts.cancelFlag   = cancelFlag;
        gopts.quality      = effectiveQuality;
        gopts.preset       = opts.preset;
        gopts.verbose      = opts.verbose;
        if (onProgress)
        {
            gopts.onProgress = [onProgress](const GpuPipelineOptions::ProgressUpdate& u) {
                ProcessProgress p;
                p.framesDone  = u.framesDone;
                p.framesTotal = u.framesTotal;
                p.elapsedSec  = u.elapsedSec;
                p.fps         = u.fps;
                p.percent     = u.percent;
                p.finalizing  = u.finalizing;
                onProgress(p);
            };
        }

        auto r = runGpuOnly(input, output, gopts);
        result.framesProcessed = r.framesIn;
        result.seconds         = r.seconds;
        if (!r.ok)
        {
            result.exitCode = 6;
            result.errorDetail = r.errorDetail.empty()
                               ? "GPU-only pipeline failed."
                               : r.errorDetail;
            return result;
        }
        result.ok = true;
        result.exitCode = 0;
        return result;
    }

    RtxConverter converter;
    if (!converter.initialize(opts.mode, info.width, info.height, outW, outH))
    {
        result.exitCode = 3;
        result.errorDetail = "Failed to initialize RTX Video SDK.";
        return result;
    }

    FFmpegDecoder decoder;
    if (!decoder.start(input, info.width, info.height, opts.hwDecode, opts.verbose))
    {
        result.exitCode = 4;
        result.errorDetail = "Failed to spawn ffmpeg decoder.";
        return result;
    }

    EncoderOptions eo;
    eo.width        = outW;
    eo.height       = outH;
    eo.fps          = info.fps;
    eo.fpsNum       = info.fpsNum;
    eo.fpsDen       = info.fpsDen;
    eo.quality      = effectiveQuality;
    eo.preset       = opts.preset;
    eo.backend      = opts.backend;
    eo.codec        = opts.codec;
    eo.copyAudio    = opts.copyAudio;
    eo.maxLuminance = opts.maxLum;
    eo.hdrOutput    = converter.outputIsHdr();

    FFmpegEncoder encoder;
    if (!encoder.start(input, output, eo, opts.verbose))
    {
        result.exitCode = 5;
        result.errorDetail = "Failed to spawn ffmpeg encoder.";
        return result;
    }

    std::vector<uint8_t> inFrame(converter.inputFrameBytes());
    std::vector<uint8_t> outFrame(converter.outputFrameBytes());

    RtxConverter::Params p{};
    p.contrast   = opts.contrast;
    p.saturation = opts.saturation;
    p.middleGray = opts.middleGray;
    p.maxLum     = opts.maxLum;
    p.vsrQuality = opts.vsrQuality;

    uint64_t frameIdx = 0;
    auto tStart = std::chrono::steady_clock::now();
    auto tLast  = tStart;

    bool encoderDied = false;
    bool decoderDied = false;
    while (true)
    {
        if (cancelFlag && cancelFlag->load())
        {
            result.errorDetail = "Cancelled.";
            break;
        }
        if (!decoder.readFrame(inFrame.data(), inFrame.size()))
        {
            if (frameIdx == 0) decoderDied = true;
            break;
        }
        if (!converter.convertFrame(inFrame.data(), outFrame.data(), p))
            break;
        if (!encoder.writeFrame(outFrame.data(), outFrame.size()))
        {
            encoderDied = true;
            break;
        }
        ++frameIdx;

        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - tLast).count() >= 250)
        {
            double secs = std::chrono::duration<double>(now - tStart).count();
            double fps  = secs > 0 ? static_cast<double>(frameIdx) / secs : 0.0;
            double pct  = info.frameCount > 0
                        ? 100.0 * static_cast<double>(frameIdx)
                                / static_cast<double>(info.frameCount)
                        : 0.0;
            printf("\r  Frame %llu / %lld  (%.1f%%, %.1f fps)   ",
                   static_cast<unsigned long long>(frameIdx),
                   static_cast<long long>(info.frameCount),
                   pct, fps);
            fflush(stdout);
            reportProgress(onProgress, frameIdx, info.frameCount, tStart);
            tLast = now;
        }
    }

    if (encoderDied) encoder.dumpStderrTail("ffmpeg encoder");
    if (decoderDied) decoder.dumpStderrTail("ffmpeg decoder");

    encoder.finish();
    decoder.finish();
    converter.shutdown();

    auto tEnd = std::chrono::steady_clock::now();
    result.seconds = std::chrono::duration<double>(tEnd - tStart).count();
    result.framesProcessed = frameIdx;

    if (cancelFlag && cancelFlag->load())
    {
        // Cancelled mid-stream: the output is truncated -- remove it and
        // report failure (previously this fell through to ok=true).
        std::error_code ec;
        fs::remove(pathFromUtf8(output), ec);
        result.exitCode = 5;
        result.errorDetail = "Cancelled";
        return result;
    }

    if (!encoderDied && !decoderDied && eo.copyAudio &&
        !verifyMuxOutput(output, opts.verbose))
    {
        std::error_code ec;
        fs::remove(pathFromUtf8(output), ec);
        result.exitCode = 5;
        result.errorDetail = "A/V sanity check failed.";
        return result;
    }

    if ((encoderDied || decoderDied) && frameIdx == 0)
    {
        std::error_code ec;
        fs::remove(pathFromUtf8(output), ec);
    }

    if (encoderDied || decoderDied)
    {
        result.exitCode = 5;
        if (result.errorDetail.empty())
            result.errorDetail = "Pipeline aborted.";
        return result;
    }

    result.ok = true;
    result.exitCode = 0;
    printf("Done. %llu frames in %.1f s (%.1f fps).\n",
           static_cast<unsigned long long>(frameIdx), result.seconds,
           result.seconds > 0 ? frameIdx / result.seconds : 0.0);
    printf("Wrote: %s\n", output.c_str());
    return result;
}

} // namespace sdr2hdr
