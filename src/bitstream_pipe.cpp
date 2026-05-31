#include "bitstream_pipe.h"
#include "win32_utils.h"

#include <cstdio>
#include <sstream>
#include <vector>

using sdr2hdr::win32::toWide;
using sdr2hdr::win32::toolPath;
using sdr2hdr::win32::quote;
using sdr2hdr::win32::PipePair;
using sdr2hdr::win32::makePipe;
using sdr2hdr::win32::launch;
using sdr2hdr::win32::readSome;
using sdr2hdr::win32::writeAll;
using sdr2hdr::win32::openStderrCapture;
using sdr2hdr::win32::dumpStderrLog;
using sdr2hdr::win32::encoderHasOption;

namespace {

// ffprobe reports codec_name; map to the shortcut ffmpeg -f /-bsf uses.
struct CodecInfo
{
    const char* canonical;      // "h264" | "hevc" | "av1" | "vp9" | ...
    const char* bsfName;        // bitstream filter, or nullptr for none
    const char* muxFmt;         // value passed to ffmpeg -f (out = same as demux)
};

CodecInfo classifyCodec(const std::string& codec)
{
    CodecInfo c{"", nullptr, nullptr};
    if (codec == "h264" || codec == "avc")        { c.canonical = "h264"; c.bsfName = "h264_mp4toannexb"; c.muxFmt = "h264"; }
    else if (codec == "hevc" || codec == "h265")  { c.canonical = "hevc"; c.bsfName = "hevc_mp4toannexb"; c.muxFmt = "hevc"; }
    else if (codec == "av1")                       { c.canonical = "av1";  c.bsfName = nullptr;            c.muxFmt = "obu";  }
    else if (codec == "vp9")                       { c.canonical = "vp9";  c.bsfName = nullptr;            c.muxFmt = "ivf";  }
    else if (codec == "vp8")                       { c.canonical = "vp8";  c.bsfName = nullptr;            c.muxFmt = "ivf";  }
    return c;
}

// Map container from output filename.
enum class Container { Mp4Like, Matroska, Ts, Avi, Webm, Unknown };

Container detectContainer(const std::string& path)
{
    auto slash = path.find_last_of("\\/");
    auto dot   = path.find_last_of('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash))
        return Container::Unknown;
    std::string ext;
    for (size_t i = dot + 1; i < path.size(); ++i)
        ext.push_back(static_cast<char>(::tolower(path[i])));
    if (ext == "mp4" || ext == "m4v" || ext == "mov" || ext == "qt") return Container::Mp4Like;
    if (ext == "mkv")  return Container::Matroska;
    if (ext == "ts" || ext == "m2ts" || ext == "mts") return Container::Ts;
    if (ext == "avi")  return Container::Avi;
    if (ext == "webm") return Container::Webm;
    return Container::Unknown;
}

} // namespace

// ===========================================================================
// BitstreamDemuxer
// ===========================================================================

BitstreamDemuxer::~BitstreamDemuxer() { finish(); }

bool BitstreamDemuxer::start(const std::string& input,
                             const std::string& codecName,
                             bool verbose)
{
    const CodecInfo info = classifyCodec(codecName);
    if (!info.muxFmt)
    {
        fprintf(stderr, "BitstreamDemuxer: codec '%s' not supported for GPU-only pipeline.\n"
                        "  Supported: h264, hevc, av1, vp9.\n",
                codecName.c_str());
        return false;
    }
    m_pipeCodec = info.canonical;

    PipePair pOut;
    if (!makePipe(pOut, /*childReads=*/false)) return false;

    HANDLE hStderr = verbose ? nullptr : openStderrCapture(m_stderrLog);

    std::ostringstream cmd;
    cmd << toolPath("ffmpeg.exe")
        << " -nostdin -loglevel " << (verbose ? "info" : "error") << " ";

    // Demux only; don't decode. Use -c:v copy so the bitstream flows through
    // untouched. For MP4-stored H.264/HEVC we must strip the avcC/hvcC length
    // prefixes and insert Annex-B start codes via the bitstream filter.
    cmd << "-i " << quote(input)
        << " -map 0:v:0"
        << " -c:v copy";
    if (info.bsfName)
        cmd << " -bsf:v " << info.bsfName;
    cmd << " -f " << info.muxFmt
        << " pipe:1";

    PROCESS_INFORMATION pi{};
    if (!launch(cmd.str(), nullptr, pOut.writeEnd, hStderr, pi))
    {
        CloseHandle(pOut.readEnd); CloseHandle(pOut.writeEnd);
        if (hStderr) CloseHandle(hStderr);
        return false;
    }
    CloseHandle(pOut.writeEnd);
    if (hStderr) CloseHandle(hStderr);
    CloseHandle(pi.hThread);

    m_hStdoutRead = pOut.readEnd;
    m_hProcess    = pi.hProcess;
    return true;
}

size_t BitstreamDemuxer::readChunk(void* dst, size_t capacity)
{
    if (!m_hStdoutRead) return 0;
    return readSome(static_cast<HANDLE>(m_hStdoutRead), dst, capacity);
}

void BitstreamDemuxer::finish()
{
    if (m_hStdoutRead) { CloseHandle(static_cast<HANDLE>(m_hStdoutRead)); m_hStdoutRead = nullptr; }
    if (m_hProcess)
    {
        WaitForSingleObject(static_cast<HANDLE>(m_hProcess), 5000);
        CloseHandle(static_cast<HANDLE>(m_hProcess));
        m_hProcess = nullptr;
    }
    if (!m_stderrLog.empty())
    {
        DeleteFileW(toWide(m_stderrLog).c_str());
        m_stderrLog.clear();
    }
}

void BitstreamDemuxer::dumpStderrTail(const char* label)
{
    dumpStderrLog(m_stderrLog, label);
}

// ===========================================================================
// BitstreamMuxer
// ===========================================================================

BitstreamMuxer::~BitstreamMuxer() { finish(); }

bool BitstreamMuxer::start(const std::string& sourceForAudio,
                           const std::string& output,
                           const MuxerOptions& opts,
                           bool verbose)
{
    PipePair pIn;
    if (!makePipe(pIn, /*childReads=*/true)) return false;

    const CodecInfo info = classifyCodec(opts.codec);
    if (!info.muxFmt)
    {
        fprintf(stderr, "BitstreamMuxer: unsupported codec '%s'.\n", opts.codec.c_str());
        CloseHandle(pIn.readEnd); CloseHandle(pIn.writeEnd);
        return false;
    }
    const Container cont = detectContainer(output);
    if (cont == Container::Webm && opts.codec != "av1")
    {
        fprintf(stderr, "Error: .webm only supports AV1. Pick a different container.\n");
        CloseHandle(pIn.readEnd); CloseHandle(pIn.writeEnd);
        return false;
    }

    std::ostringstream cmd;
    cmd << toolPath("ffmpeg.exe")
        << " -nostdin -loglevel " << (verbose ? "info" : "error") << " -y ";

    // --- Input #0 : raw bitstream on pipe:0 ------------------------------
    cmd << "-f " << info.muxFmt;
    if (opts.fpsNum && opts.fpsDen)
    {
        // Exact rational matches the VUI timing NVENC baked into the stream.
        cmd << " -framerate " << opts.fpsNum << "/" << opts.fpsDen;
    }
    else
    {
        cmd << " -framerate " << opts.fps;
    }

    // Mux-time colour/trc tagging for the video stream. Applies to container
    // and (for MKV) flows into the MKV colour elements.
    if (opts.hdr10)
    {
        cmd << " -color_primaries bt2020 -color_trc smpte2084 -colorspace bt2020nc";
    }
    else
    {
        cmd << " -color_primaries bt709  -color_trc bt709     -colorspace bt709";
    }
    cmd << " -i pipe:0";

    // --- Input #1 : source file for audio --------------------------------
    if (opts.copyAudio && !sourceForAudio.empty())
    {
        cmd << " -i " << quote(sourceForAudio)
            << " -map 0:v:0 -map 1:a? -c:a copy";
    }
    else
    {
        cmd << " -map 0:v:0";
    }

    // --- Pass-through video copy -----------------------------------------
    cmd << " -c:v copy";

    // NOTE on HDR10 static metadata:
    //   `-master_display` / `-max_cll` are libx265 *encoder-private* options,
    //   not top-level ffmpeg flags. They only work when actually re-encoding
    //   through libx265, not with `-c:v copy`. Passing them here makes newer
    //   ffmpeg builds abort with `Unrecognized option 'master_display'`.
    //
    //   For HDR10 playback, the VUI baked into the HEVC bitstream by NVENC
    //   upstream (colourPrimaries=9 BT.2020, transferCharacteristics=16
    //   ST.2084, colourMatrix=9 BT.2020 NCL) plus the stream-level
    //   `-color_primaries/-color_trc/-colorspace` tags we already applied
    //   above are sufficient for VLC/mpv/Windows/YouTube to treat the file
    //   as HDR10. Mastering Display SEI (SMPTE 2086) and MaxCLL/MaxFALL SEI
    //   (CEA-861.3) would be "nice to have" for strict HDR10 compliance,
    //   but they require per-picture SEI payload insertion via NVENC's
    //   NV_ENC_PIC_PARAMS_HEVC.pMasteringDisplay/pMaxCll. That's deferred.
    (void)opts.maxCll;

    // Container-specific flags.
    if (cont == Container::Mp4Like)
    {
        if (opts.codec == "hevc") cmd << " -tag:v hvc1";
        cmd << " -movflags +faststart";
    }

    cmd << " " << quote(output);

    HANDLE hStderr = verbose ? nullptr : openStderrCapture(m_stderrLog);

    PROCESS_INFORMATION pi{};
    if (!launch(cmd.str(), pIn.readEnd, nullptr, hStderr, pi))
    {
        CloseHandle(pIn.readEnd); CloseHandle(pIn.writeEnd);
        if (hStderr) CloseHandle(hStderr);
        return false;
    }
    CloseHandle(pIn.readEnd);
    if (hStderr) CloseHandle(hStderr);
    CloseHandle(pi.hThread);

    m_hStdinWrite = pIn.writeEnd;
    m_hProcess    = pi.hProcess;
    return true;
}

bool BitstreamMuxer::writeChunk(const void* src, size_t bytes)
{
    if (!m_hStdinWrite || !bytes) return true;
    return writeAll(static_cast<HANDLE>(m_hStdinWrite), src, bytes);
}

void BitstreamMuxer::finish()
{
    if (m_hStdinWrite) { CloseHandle(static_cast<HANDLE>(m_hStdinWrite)); m_hStdinWrite = nullptr; }
    if (m_hProcess)
    {
        WaitForSingleObject(static_cast<HANDLE>(m_hProcess), INFINITE);
        CloseHandle(static_cast<HANDLE>(m_hProcess));
        m_hProcess = nullptr;
    }
    if (!m_stderrLog.empty())
    {
        DeleteFileW(toWide(m_stderrLog).c_str());
        m_stderrLog.clear();
    }
}

void BitstreamMuxer::dumpStderrTail(const char* label)
{
    dumpStderrLog(m_stderrLog, label);
}
