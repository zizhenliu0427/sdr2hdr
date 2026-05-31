// GPU-only pipeline:
//
//   ffmpeg(demux, -c:v copy)  ->  Annex-B bitstream on our stdin
//                             ->  NvDecoder            (NV12/P010 in VRAM)
//                             ->  CUDA kernel          (NV12/P010 -> RGBA8)
//                             ->  RTX Video SDK        (deviceptr, TrueHDR/VSR)
//                             ->  CUDA kernel          (RGBA8/ABGR10 -> NV12/P010)
//                             ->  NvEncoderCuda        (HEVC Main10 / AVC / AV1)
//                             ->  our stdout           (encoded Annex-B packets)
//   ffmpeg(mux, -c:v copy)    <-
//
// Every frame stays in GPU VRAM from cuvidDecodeFrame() to nvEncEncodePicture();
// the only traffic crossing the process boundary is the compressed bitstream
// (~10 MB/s at 4K 60fps, two orders of magnitude less than raw 4:2:0).
//
// The pipeline is intentionally single-threaded: bitstream handoff is
// pull-based (we read a demuxer chunk then loop over whatever ready frames
// NvDecoder produced). Three FIFOs of device buffers (decode / RTX-in / RTX-out)
// decouple the inner stages, but scheduling is driven by one CUDA stream so we
// don't accidentally race with NVDEC output or NVENC input reads.
#pragma once

#include "rtx_converter.h"
#include "ffmpeg_process.h"   // VideoInfo

#include <cstdint>
#include <string>

namespace sdr2hdr {

struct GpuPipelineOptions
{
    RtxConverter::Mode mode = RtxConverter::Mode::Hdr;

    // Source info (already probed by caller).
    VideoInfo input{};

    // Output sizing (equal to input for HDR-only; caller-computed for VSR).
    uint32_t outWidth  = 0;
    uint32_t outHeight = 0;

    // Output codec container & codec: ffmpeg muxer handles container; NVENC
    // handles codec. Must be one of "hevc" | "h264" | "av1".
    std::string codec  = "hevc";

    // TrueHDR artistic parameters.
    RtxConverter::Params rtx{};

    // HDR10 mastering display max luminance (nits) -- becomes MaxCLL in the
    // output SEI / muxer metadata.
    uint32_t maxLuminance = 1000;

    // Whether to copy original audio into the muxed output.
    bool copyAudio = true;

    // NVENC target bitrate hint (kbps, 0 = auto derived from pixels*fps).
    // For a first version we use CQP with this "quality" integer mapped from
    // the CLI --quality flag; 0 means pick a sensible default.
    int  quality   = 0;

    // NVENC preset index: p1..p7 (1=fastest, 7=slowest/highest-quality).
    // Empty = default p5.
    std::string preset = "";

    // Verbose ffmpeg stderr passthrough.
    bool verbose = false;
};

struct GpuPipelineResult
{
    uint64_t framesIn  = 0;
    uint64_t framesOut = 0;
    double   seconds   = 0.0;
    bool     ok        = false;
    std::string errorDetail;
};

// Run the full GPU-only pipeline once, blocking until EOF. Returns the result
// struct; errorDetail is populated on failure.
GpuPipelineResult runGpuOnly(const std::string& input,
                             const std::string& output,
                             const GpuPipelineOptions& opts);

} // namespace sdr2hdr
