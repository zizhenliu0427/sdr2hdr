// Win32 helpers to spawn ffmpeg/ffprobe child processes and exchange raw frames
// via anonymous pipes.
//
// The helpers first look for ffmpeg.exe / ffprobe.exe next to the running
// executable. If not found there, they fall back to whatever is on PATH.
#pragma once

#include <cstdint>
#include <string>

struct VideoInfo
{
    uint32_t    width      = 0;
    uint32_t    height     = 0;
    double      fps        = 0.0;
    int64_t     frameCount = 0;     // 0 if unknown
    std::string codecName;          // "h264", "hevc", "av1", "vp9", ...

    // Decoded surface bit depth derived from ffprobe pix_fmt. The GPU
    // pipeline branches on this: 8-bit -> NV12 -> launchNv12ToRgba8,
    // 10-bit -> P010 -> launchP010ToRgba8. Without this branch a
    // 10-bit input gets read as if it were 8-bit, which produces
    // high-entropy garbage that NVENC can't compress and balloons the
    // output by ~30x while killing throughput.
    std::string pixFmt;             // e.g. "yuv420p", "yuv420p10le", "nv12"
    int         bitDepth = 8;       // 8, 10, or 12

    // Probed video stream bitrate in bits/second (ffprobe `stream=bit_rate`).
    // 0 when ffprobe reports "N/A" (the muxer didn't record a per-stream
    // bitrate). Used by recommendQuality() to size NVENC's CQP so that the
    // output bitrate roughly tracks the source instead of dropping to the
    // q=19 default ~58 Mbps regardless of how rich the source was.
    int64_t     bitRateBps = 0;
};

// ---------------------------------------------------------------------------
// Bitrate <-> CQP helpers for "match source" quality picking
// ---------------------------------------------------------------------------
//
// Empirically fitted against NVENC HEVC p5 on 4K120 HDR game content
// (calibration: q=19 -> ~58 Mbps, q=15 -> ~103 Mbps). The model assumes
// bitrate scales linearly with pixel-rate (width*height*fps) and as
// 1.78^((19-q)/4) with CQP. SDR output is roughly 0.6x of HDR at equal q.
//
// These are coarse heuristics: real per-content variance is +/- 30%. They
// exist purely so the wizard can show "this tier ~ X Mbps" labels and so
// --quality auto can pick a sensible q from the probed source bitrate
// (high-bitrate source -> tighter q; low-bitrate source -> looser q).

// Predicted NVENC output bitrate (Mbps) for the given CQP and stream params.
double predictBitrateMbps(int q, uint32_t w, uint32_t h, double fps, bool hdrOut);

// Recommend a CQP that targets `srcBps * keepRatio` Mbps. Returns clamped
// value in [12, 28]. keepRatio=1.0 -> literal match; ~0.7 -> source minus
// the typical DVR / recorder bloat.
int recommendQuality(int64_t srcBps, uint32_t w, uint32_t h, double fps,
                     bool hdrOut, double keepRatio = 0.75);

// Runs `ffprobe` and fills VideoInfo. Returns false on failure.
bool probeVideo(const std::string& input, VideoInfo& out);

// ffmpeg decoder: reads input file, writes rawvideo (RGBA, 8-bit, pitch=4*w)
// to its stdout. Always decoded at the source resolution.
//
// When hwDecode==true, passes `-hwaccel cuda` to ffmpeg so NVDEC handles the
// bitstream -> YUV step on the GPU. Output is still downloaded back to system
// memory (as RGBA) for the pipe to convert.exe; this is a pragmatic trade-off
// vs a full zero-copy CUDA pipeline.
class FFmpegDecoder
{
public:
    FFmpegDecoder() = default;
    ~FFmpegDecoder();

    bool start(const std::string& input, uint32_t w, uint32_t h,
               bool hwDecode, bool verbose);
    bool readFrame(void* dst, size_t bytes);
    void finish();

    // Dump tail of captured ffmpeg stderr (if any) to stderr. Safe to call
    // at any point; if verbose was on, this is a no-op.
    void dumpStderrTail(const char* label);

private:
    void*       m_hStdoutRead  = nullptr;
    void*       m_hProcess     = nullptr;
    std::string m_stderrLog;     // UTF-8 path of temp log file, or empty
};

struct EncoderOptions
{
    uint32_t    width   = 0;
    uint32_t    height  = 0;
    double      fps     = 30.0;
    int         quality = 19;       // nvenc -cq / x264/x265 -crf / svtav1 -crf
    std::string preset  = "";       // "" = encoder default
    std::string backend = "nvenc";  // "nvenc" or "software"
    std::string codec   = "hevc";   // "hevc" | "h264" | "av1"
    bool        copyAudio    = true;
    uint32_t    maxLuminance = 1000;
    bool        hdrOutput    = true; // true => HDR10 + ABGR10 input; false => SDR bt709 + RGBA8 input
};

class FFmpegEncoder
{
public:
    FFmpegEncoder() = default;
    ~FFmpegEncoder();

    bool start(const std::string& sourceForAudio,
               const std::string& output,
               const EncoderOptions& opts,
               bool verbose);
    bool writeFrame(const void* src, size_t bytes);
    void finish();

    void dumpStderrTail(const char* label);

private:
    void*       m_hStdinWrite = nullptr;
    void*       m_hProcess    = nullptr;
    std::string m_stderrLog;
};
