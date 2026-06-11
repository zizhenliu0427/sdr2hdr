#include "bitstream_pipe.h"
#include "win32_utils.h"

#include <cstdio>
#include <cstring>
#include <share.h>
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

struct CodecInfo
{
    const char* canonical;
    const char* bsfName;
    const char* muxFmt;
};

CodecInfo classifyCodec(const std::string& codec)
{
    CodecInfo c{"", nullptr, nullptr};
    if (codec == "h264" || codec == "avc")        { c.canonical = "h264"; c.bsfName = "h264_mp4toannexb"; c.muxFmt = "h264"; }
    else if (codec == "hevc" || codec == "h265")  { c.canonical = "hevc"; c.bsfName = "hevc_mp4toannexb"; c.muxFmt = "hevc"; }
    else if (codec == "av1")                       { c.canonical = "av1";  c.bsfName = nullptr;            c.muxFmt = "ivf";  }
    else if (codec == "vp9")                       { c.canonical = "vp9";  c.bsfName = nullptr;            c.muxFmt = "ivf";  }
    else if (codec == "vp8")                       { c.canonical = "vp8";  c.bsfName = nullptr;            c.muxFmt = "ivf";  }
    return c;
}

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
                             bool verbose,
                             const std::string& tsCapturePath)
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
        << " -nostdin -y -loglevel " << (verbose ? "info" : "error") << " ";

    cmd << "-i " << quote(input)
        << " -map 0:v:0"
        << " -c:v copy";
    if (info.bsfName)
        cmd << " -bsf:v " << info.bsfName;
    cmd << " -f " << info.muxFmt
        << " pipe:1";

    // Optional second output: exact per-packet pts (framecrc format includes
    // a "#tb 0: num/den" header and "stream, dts, pts, ..." lines). Costs no
    // extra read of the source -- the packets are already in memory.
    if (!tsCapturePath.empty())
        cmd << " -map 0:v:0 -c:v copy -f framecrc " << quote(tsCapturePath);

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

bool BitstreamMuxer::writeSink(const void* data, size_t bytes)
{
    if (m_rawFile)
        return std::fwrite(data, 1, bytes, m_rawFile) == bytes;
    if (m_hStdinWrite)
        return writeAll(static_cast<HANDLE>(m_hStdinWrite), data, bytes);
    return false;
}

bool BitstreamMuxer::start(const std::string& sourceForAudio,
                           const std::string& output,
                           const MuxerOptions& opts,
                           bool verbose)
{
    const CodecInfo info = classifyCodec(opts.codec);
    if (!info.muxFmt)
    {
        fprintf(stderr, "BitstreamMuxer: unsupported codec '%s'.\n", opts.codec.c_str());
        return false;
    }

    m_rawFileMode = !opts.rawOutputPath.empty();
    if (m_rawFileMode)
    {
        m_rawFilePath = opts.rawOutputPath;
        m_rawFile = _fsopen(m_rawFilePath.c_str(), "wb", _SH_DENYNO);
        if (!m_rawFile)
        {
            fprintf(stderr, "BitstreamMuxer: cannot open raw output '%s'.\n",
                    m_rawFilePath.c_str());
            return false;
        }
        (void)sourceForAudio;
        (void)output;
        (void)verbose;
        return true;
    }

    PipePair pIn;
    if (!makePipe(pIn, /*childReads=*/true)) return false;

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

    cmd << "-f " << info.muxFmt;
    if (opts.fpsNum && opts.fpsDen)
        cmd << " -framerate " << opts.fpsNum << "/" << opts.fpsDen;
    else
        cmd << " -framerate " << opts.fps;
    if (opts.hdr10)
        cmd << " -color_primaries bt2020 -color_trc smpte2084 -colorspace bt2020nc";
    else
        cmd << " -color_primaries bt709  -color_trc bt709     -colorspace bt709";
    cmd << " -i pipe:0";

    if (opts.copyAudio && !sourceForAudio.empty())
    {
        cmd << " -i " << quote(sourceForAudio)
            << " -map 0:v:0 -map 1:a:0 -c:a copy";
    }
    else
    {
        cmd << " -map 0:v:0";
    }

    cmd << " -c:v copy";
    (void)opts.maxCll;

    if (cont == Container::Mp4Like)
    {
        if (opts.codec == "hevc") cmd << " -tag:v hvc1";
        // No +faststart: it rewrites the entire output a second time just to
        // move moov to the front (20+ GB of extra I/O on a long 4K capture).
        // Local players handle moov-at-end fine; it only matters for
        // progressive HTTP streaming. It would also break the VFR re-timing
        // pass, which requires moov to be the last box (mp4_retime.cpp).
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

bool BitstreamMuxer::writeChunk(const void* src, size_t bytes, size_t /*frameIndex*/)
{
    if (!bytes) return true;
    if (m_rawFileMode)
        return writeSink(src, bytes);
    if (!m_hStdinWrite) return false;
    return writeAll(static_cast<HANDLE>(m_hStdinWrite), src, bytes);
}

void BitstreamMuxer::finish()
{
    if (m_rawFile)
    {
        std::fclose(m_rawFile);
        m_rawFile = nullptr;
    }
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
    m_rawFileMode = false;
    m_rawFilePath.clear();
}

void BitstreamMuxer::dumpStderrTail(const char* label)
{
    dumpStderrLog(m_stderrLog, label);
}

// ===========================================================================
// IvfStreamParser
// ===========================================================================

namespace {

constexpr size_t kIvfFileHeaderSize  = 32;
constexpr size_t kIvfFrameHeaderSize = 12;
constexpr size_t kIvfMaxFrameBytes   = 64u << 20; // 64 MiB sanity cap

} // namespace

void IvfStreamParser::append(const uint8_t* data, size_t n)
{
    if (!data || !n) return;
    m_buf.insert(m_buf.end(), data, data + n);
}

void IvfStreamParser::setEof()
{
    m_eof = true;
}

bool IvfStreamParser::nextFrame(std::vector<uint8_t>& payloadOut)
{
    payloadOut.clear();

    if (!m_headerDone)
    {
        if (m_buf.size() < kIvfFileHeaderSize)
            return false;
        if (!(m_buf[0] == 'D' && m_buf[1] == 'K' && m_buf[2] == 'I' && m_buf[3] == 'F'))
        {
            fprintf(stderr, "IvfStreamParser: bad IVF magic (expected DKIF).\n");
            return false;
        }
        m_buf.erase(m_buf.begin(), m_buf.begin() + static_cast<std::ptrdiff_t>(kIvfFileHeaderSize));
        m_headerDone = true;
    }

    if (m_buf.size() < kIvfFrameHeaderSize)
        return false;

    const uint32_t frameSize =
        static_cast<uint32_t>(m_buf[0]) |
        (static_cast<uint32_t>(m_buf[1]) << 8) |
        (static_cast<uint32_t>(m_buf[2]) << 16) |
        (static_cast<uint32_t>(m_buf[3]) << 24);

    if (frameSize == 0 || frameSize > kIvfMaxFrameBytes)
    {
        fprintf(stderr, "IvfStreamParser: invalid IVF frame size %u.\n", frameSize);
        return false;
    }

    const size_t total = kIvfFrameHeaderSize + static_cast<size_t>(frameSize);
    if (m_buf.size() < total)
        return false;

    payloadOut.assign(m_buf.begin() + kIvfFrameHeaderSize,
                      m_buf.begin() + static_cast<std::ptrdiff_t>(total));
    m_buf.erase(m_buf.begin(), m_buf.begin() + static_cast<std::ptrdiff_t>(total));
    return true;
}
