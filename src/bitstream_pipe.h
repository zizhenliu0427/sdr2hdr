// Annex-B elementary-stream pipes to/from ffmpeg subprocesses.
//
// Unlike ffmpeg_process.{h,cpp} (which pipes raw uncompressed pixel frames),
// these pipe *encoded* bitstreams -- ~10 MB/s at 4K 60fps vs ~2 GB/s -- so
// pipe bandwidth is no longer a bottleneck. NVDEC/NVENC operate entirely in
// GPU memory inside sdr2hdr.exe, with only the compressed bitstream crossing
// process boundaries.
//
//   BitstreamDemuxer  :  ffmpeg (demux) -c:v copy -bsf:v *_mp4toannexb
//                        -f <codec> pipe:1      -->  our stdin
//   BitstreamMuxer    :  our stdout  -->  ffmpeg -f <codec> -i pipe:0
//                        -c:v copy  (mux into mp4/mkv/mov/...)
#pragma once

#include "ffmpeg_process.h"    // VideoInfo, probeVideo(...) reused

#include <cstdint>
#include <string>

// ---------------------------------------------------------------------------
// BitstreamDemuxer: pipe source-file Annex-B bitstream out of ffmpeg.
// readChunk() returns the number of bytes actually read, 0 on EOF/error.
// ---------------------------------------------------------------------------
class BitstreamDemuxer
{
public:
    BitstreamDemuxer() = default;
    ~BitstreamDemuxer();

    bool start(const std::string& input, const std::string& codecName, bool verbose);

    // Fills up to `capacity` bytes; returns bytes read (0 on EOF).
    size_t readChunk(void* dst, size_t capacity);

    void finish();
    void dumpStderrTail(const char* label);

    // Codec string fed to ffmpeg's -f flag / the NvDecoder (h264|hevc|av1|vp9).
    const std::string& pipeCodec() const { return m_pipeCodec; }

private:
    void*       m_hStdoutRead = nullptr;
    void*       m_hProcess    = nullptr;
    std::string m_stderrLog;
    std::string m_pipeCodec;
};

// ---------------------------------------------------------------------------
// BitstreamMuxer: accept Annex-B bitstream packets written by us, mux into
// the requested container via ffmpeg -c:v copy. Also copies source audio.
// ---------------------------------------------------------------------------
struct MuxerOptions
{
    std::string codec   = "hevc";   // matches the bitstream we're producing
    uint32_t    width   = 0;
    uint32_t    height  = 0;
    double      fps     = 30.0;
    // Exact frame-rate rational. When non-zero, passed to the muxer as
    // `-framerate num/den` which is more precise than the double `fps`
    // field. Should match the rational NVENC used for VUI timing.
    uint32_t    fpsNum  = 0;
    uint32_t    fpsDen  = 0;
    bool        hdr10   = true;
    uint32_t    maxCll  = 1000;     // nits, used for -max_cll
    bool        copyAudio = true;
};

class BitstreamMuxer
{
public:
    BitstreamMuxer() = default;
    ~BitstreamMuxer();

    bool start(const std::string& sourceForAudio,
               const std::string& output,
               const MuxerOptions& opts,
               bool verbose);

    bool writeChunk(const void* src, size_t bytes);

    void finish();
    void dumpStderrTail(const char* label);

private:
    void*       m_hStdinWrite = nullptr;
    void*       m_hProcess    = nullptr;
    std::string m_stderrLog;
};
