#include "ffmpeg_process.h"
#include "win32_utils.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

using sdr2hdr::win32::toWide;
using sdr2hdr::win32::fromWide;
using sdr2hdr::win32::exeDir;
using sdr2hdr::win32::fileExists;
using sdr2hdr::win32::toolPath;
using sdr2hdr::win32::quote;
using sdr2hdr::win32::PipePair;
using sdr2hdr::win32::makePipe;
using sdr2hdr::win32::launch;
using sdr2hdr::win32::readAll;
using sdr2hdr::win32::writeAll;
using sdr2hdr::win32::runAndCapture;
using sdr2hdr::win32::openStderrCapture;
using sdr2hdr::win32::dumpStderrLog;
using sdr2hdr::win32::encoderHasOption;

namespace {

std::string trim(const std::string& s)
{
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\r' || s[a] == '\n' || s[a] == '\t')) ++a;
    while (b > a && (s[b-1] == ' ' || s[b-1] == '\r' || s[b-1] == '\n' || s[b-1] == '\t')) --b;
    return s.substr(a, b - a);
}

} // namespace

// ---------------------------------------------------------------------------
// probeVideo (uses ffprobe)
// ---------------------------------------------------------------------------

bool probeVideo(const std::string& input, VideoInfo& out)
{
    // r_frame_rate is the "nominal" rate -- the rational a well-behaved
    // source declares up-front (e.g. "120/1" for 4K120 game capture),
    // which is what we want for VUI timing. avg_frame_rate is derived from
    // frame_count/duration and rounds to things like "119997/1000" which
    // introduces a 25 ppm drift that looks like micro-stutter on playback.
    // Note: we ask for stream-level fields AND format-level duration. MKV /
    // WebM / TS containers don't store nb_frames (it's a MP4-ism), so for
    // those we estimate frameCount = duration * fps further down. The
    // format=duration query is cheap (single ffprobe call) and covers
    // every container we care about.
    std::string cmd =
        toolPath("ffprobe.exe") + " -v error -select_streams v:0 "
        "-show_entries stream=codec_name,width,height,r_frame_rate,avg_frame_rate,nb_frames,pix_fmt,bit_rate"
        ":stream_tags=DURATION:format=duration "
        "-of default=noprint_wrappers=1 " + quote(input);

    std::string text;
    DWORD exit = 0;
    if (!runAndCapture(cmd, text, exit) || exit != 0) return false;

    auto getVal = [&](const std::string& key) -> std::string {
        size_t p = text.find(key + "=");
        if (p == std::string::npos) return {};
        size_t e = text.find('\n', p);
        return trim(text.substr(p + key.size() + 1,
                                e == std::string::npos ? std::string::npos : e - p - key.size() - 1));
    };

    std::string sw  = getVal("width");
    std::string sh  = getVal("height");
    std::string srfr = getVal("r_frame_rate");
    std::string safr = getVal("avg_frame_rate");
    std::string snb = getVal("nb_frames");
    std::string scc = getVal("codec_name");

    if (sw.empty() || sh.empty()) return false;
    out.width     = static_cast<uint32_t>(std::atoi(sw.c_str()));
    out.height    = static_cast<uint32_t>(std::atoi(sh.c_str()));
    out.codecName = scc;

    out.fps = 30.0;
    // Prefer r_frame_rate; fall back to avg_frame_rate if the source didn't
    // declare one.
    const std::string& sfr =
        (!srfr.empty() && srfr != "0/0") ? srfr : safr;
    if (!sfr.empty() && sfr != "0/0")
    {
        auto slash = sfr.find('/');
        if (slash != std::string::npos)
        {
            double num = std::atof(sfr.substr(0, slash).c_str());
            double den = std::atof(sfr.substr(slash + 1).c_str());
            if (den != 0.0) out.fps = num / den;
        }
        else
        {
            out.fps = std::atof(sfr.c_str());
        }
    }

    out.frameCount = 0;
    if (!snb.empty() && snb != "N/A")
    {
        out.frameCount = _atoi64(snb.c_str());
    }
    else
    {
        // MKV / WebM / TS containers don't expose stream=nb_frames, so estimate
        // from duration. Prefer format=duration (always present, second-precision),
        // fall back to MKV's stream=tags=DURATION (HH:MM:SS.fffff string).
        double durationSec = 0.0;
        std::string sdur = getVal("duration");        // format-level
        if (sdur.empty() || sdur == "N/A")
            sdur = getVal("TAG:DURATION");            // MKV stream tag
        if (!sdur.empty() && sdur != "N/A")
        {
            if (sdur.find(':') != std::string::npos)
            {
                // "HH:MM:SS.fff..." -> seconds
                int hh = 0, mm = 0; double ss = 0.0;
                if (std::sscanf(sdur.c_str(), "%d:%d:%lf", &hh, &mm, &ss) == 3)
                    durationSec = hh * 3600.0 + mm * 60.0 + ss;
            }
            else
            {
                durationSec = std::atof(sdur.c_str());
            }
        }
        if (durationSec > 0.0 && out.fps > 0.0)
            out.frameCount = static_cast<int64_t>(durationSec * out.fps + 0.5);
    }

    // pix_fmt -> bit depth. ffprobe reports things like:
    //   yuv420p        -> 8-bit  (NV12 surface from NvDecoder)
    //   yuv420p10le    -> 10-bit (P010 surface)
    //   yuv420p12le    -> 12-bit (P016 surface; rare, not currently supported)
    //   yuv444p / yuv422p* -> we don't support 4:4:4 or 4:2:2 yet
    // Default to 8 if the field is missing or unrecognised; the pipeline
    // will sanity-check before launching kernels.
    out.pixFmt   = getVal("pix_fmt");
    out.bitDepth = 8;
    if (!out.pixFmt.empty())
    {
        if (out.pixFmt.find("p10") != std::string::npos ||
            out.pixFmt.find("10le") != std::string::npos ||
            out.pixFmt == "p010le" || out.pixFmt == "p010be")
        {
            out.bitDepth = 10;
        }
        else if (out.pixFmt.find("p12") != std::string::npos ||
                 out.pixFmt.find("12le") != std::string::npos)
        {
            out.bitDepth = 12;
        }
    }

    std::string sbr = getVal("bit_rate");
    out.bitRateBps = 0;
    if (!sbr.empty() && sbr != "N/A")
        out.bitRateBps = _atoi64(sbr.c_str());

    return out.width > 0 && out.height > 0;
}

// ---------------------------------------------------------------------------
// Quality / bitrate heuristics
// ---------------------------------------------------------------------------

double predictBitrateMbps(int q, uint32_t w, uint32_t h, double fps, bool hdrOut)
{
    if (w == 0 || h == 0 || fps <= 0.0) return 0.0;
    // Calibration anchor: 4K120 HDR @ q=19 -> ~58 Mbps NVENC HEVC p5.
    const double pixelFactor =
        static_cast<double>(w) * h * fps / (3840.0 * 2160.0 * 120.0);
    double base = 58.0 * pixelFactor * std::pow(1.78, (19.0 - q) / 4.0);
    if (!hdrOut) base *= 0.6;
    return base;
}

int recommendQuality(int64_t srcBps, uint32_t w, uint32_t h, double fps,
                     bool hdrOut, double keepRatio)
{
    if (srcBps <= 0 || w == 0 || h == 0 || fps <= 0.0) return 19;

    const double srcMbps    = srcBps / 1.0e6;
    double       targetMbps = srcMbps * keepRatio;
    if (targetMbps < 1.0) targetMbps = 1.0;

    const double pixelFactor =
        static_cast<double>(w) * h * fps / (3840.0 * 2160.0 * 120.0);
    double baseAt19 = 58.0 * pixelFactor;
    if (!hdrOut) baseAt19 *= 0.6;
    if (baseAt19 <= 0.0) return 19;

    // bitrate(q) = baseAt19 * 1.78^((19-q)/4)
    //   => q = 19 - 4 * log(target/baseAt19) / log(1.78)
    const double q = 19.0
        - 4.0 * std::log(targetMbps / baseAt19) / std::log(1.78);

    int qi = static_cast<int>(q + 0.5);
    if (qi < 12) qi = 12;
    if (qi > 28) qi = 28;
    return qi;
}

// ---------------------------------------------------------------------------
// FFmpegDecoder (legacy raw-frame path; kept for fallback via --legacy-pipe)
// ---------------------------------------------------------------------------

FFmpegDecoder::~FFmpegDecoder() { finish(); }

bool FFmpegDecoder::start(const std::string& input, uint32_t w, uint32_t h,
                          bool hwDecode, bool verbose)
{
    PipePair pOut;
    if (!makePipe(pOut, /*childReads=*/false)) return false;

    HANDLE hStderr = verbose ? nullptr : openStderrCapture(m_stderrLog);

    std::ostringstream cmd;
    cmd << toolPath("ffmpeg.exe")
        << " -nostdin -loglevel error ";

    if (hwDecode)
        cmd << "-hwaccel cuda ";

    cmd << "-i " << quote(input) << " "
        << "-threads 0 -filter_threads 0 "
        << "-f rawvideo -pix_fmt rgba "
        << "pipe:1";
    (void)w; (void)h;

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

bool FFmpegDecoder::readFrame(void* dst, size_t bytes)
{
    if (!m_hStdoutRead) return false;
    return readAll(m_hStdoutRead, dst, bytes);
}

void FFmpegDecoder::finish()
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

void FFmpegDecoder::dumpStderrTail(const char* label)
{
    dumpStderrLog(m_stderrLog, label);
}

// ---------------------------------------------------------------------------
// FFmpegEncoder (legacy raw-frame path)
// ---------------------------------------------------------------------------

FFmpegEncoder::~FFmpegEncoder() { finish(); }

namespace {

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

    if (ext == "mp4" || ext == "m4v" || ext == "mov" || ext == "qt")
        return Container::Mp4Like;
    if (ext == "mkv")  return Container::Matroska;
    if (ext == "ts" || ext == "m2ts" || ext == "mts") return Container::Ts;
    if (ext == "avi")  return Container::Avi;
    if (ext == "webm") return Container::Webm;
    return Container::Unknown;
}

std::string nvencPreset(const std::string& p)
{
    if (p.empty()) return "p5";
    if (p.size() == 2 && p[0] == 'p' && p[1] >= '1' && p[1] <= '7') return p;
    if (p == "ultrafast" || p == "superfast") return "p1";
    if (p == "veryfast")                      return "p2";
    if (p == "faster")                        return "p3";
    if (p == "fast")                          return "p4";
    if (p == "medium")                        return "p5";
    if (p == "slow")                          return "p6";
    if (p == "slower" || p == "veryslow" || p == "placebo") return "p7";
    return p;
}

std::string x26xPreset(const std::string& p)
{
    return p.empty() ? std::string("medium") : p;
}

std::string svtav1Preset(const std::string& p)
{
    if (p.empty()) return "8";
    bool allDigit = !p.empty();
    for (char c : p) if (c < '0' || c > '9') { allDigit = false; break; }
    if (allDigit) return p;

    if (p == "placebo" || p == "veryslow") return "3";
    if (p == "slower")                     return "5";
    if (p == "slow")                       return "6";
    if (p == "medium")                     return "8";
    if (p == "fast")                       return "10";
    if (p == "faster")                     return "11";
    if (p == "veryfast")                   return "12";
    if (p == "superfast" || p == "ultrafast") return "13";
    return p;
}

std::string resolveEncoderName(const std::string& codec, bool useNvenc)
{
    if (useNvenc)
    {
        if (codec == "h264") return "h264_nvenc";
        if (codec == "av1")  return "av1_nvenc";
        return "hevc_nvenc";
    }
    if (codec == "h264") return "libx264";
    if (codec == "av1")  return "libsvtav1";
    return "libx265";
}

std::string pixFmtFor(const std::string& codec, bool useNvenc, bool hdr)
{
    if (!hdr)              return "yuv420p";
    if (codec == "h264")   return "yuv420p";
    if (useNvenc)          return "p010le";
    return "yuv420p10le";
}

} // namespace

bool FFmpegEncoder::start(const std::string& sourceForAudio,
                          const std::string& output,
                          const EncoderOptions& opts,
                          bool verbose)
{
    PipePair pIn;
    if (!makePipe(pIn, /*childReads=*/true)) return false;

    const bool useNvenc   = (opts.backend == "nvenc");
    const Container cont  = detectContainer(output);
    const std::string enc = resolveEncoderName(opts.codec, useNvenc);

    if (cont == Container::Webm && opts.codec != "av1")
    {
        fprintf(stderr, "Error: .webm only supports AV1 (or VP8/VP9). Use --codec av1 "
                        "or pick a different container (mp4/mkv/mov/ts).\n");
        CloseHandle(pIn.readEnd); CloseHandle(pIn.writeEnd);
        return false;
    }
    if (cont == Container::Avi)
    {
        fprintf(stderr, "Warning: .avi is a poor container for modern codecs; "
                        "seeking/playback may be unreliable. Consider .mkv or .mp4.\n");
    }

    std::ostringstream cmd;
    cmd << toolPath("ffmpeg.exe")
        << " -nostdin -loglevel " << (verbose ? "info" : "error") << " -y "
        << "-threads 0 -filter_threads 0 ";

    if (opts.hdrOutput)
    {
        cmd << "-f rawvideo -pixel_format x2bgr10le "
            << "-video_size " << opts.width << "x" << opts.height << " "
            << "-framerate " << opts.fps << " "
            << "-colorspace bt2020nc -color_primaries bt2020 -color_trc smpte2084 "
            << "-i pipe:0 ";
    }
    else
    {
        cmd << "-f rawvideo -pixel_format rgba "
            << "-video_size " << opts.width << "x" << opts.height << " "
            << "-framerate " << opts.fps << " "
            << "-colorspace bt709 -color_primaries bt709 -color_trc bt709 "
            << "-i pipe:0 ";
    }

    if (opts.copyAudio)
        cmd << "-i " << quote(sourceForAudio) << " -map 0:v:0 -map 1:a? -c:a copy ";
    else
        cmd << "-map 0:v:0 ";

    const std::string pixfmt = pixFmtFor(opts.codec, useNvenc, opts.hdrOutput);

    if (useNvenc)
    {
        cmd << "-c:v " << enc
            << " -preset " << nvencPreset(opts.preset)
            << " -tune hq"
            << " -rc vbr -cq " << opts.quality << " -b:v 0"
            << " -pix_fmt " << pixfmt;
        if (opts.hdrOutput && opts.codec == "hevc") cmd << " -profile:v main10";
        if (opts.hdrOutput && opts.codec == "av1")  cmd << " -profile:v main";
    }
    else if (opts.codec == "av1")
    {
        cmd << "-c:v libsvtav1"
            << " -preset " << svtav1Preset(opts.preset)
            << " -crf " << opts.quality
            << " -b:v 0"
            << " -pix_fmt " << pixfmt;
    }
    else
    {
        cmd << "-c:v " << enc
            << " -preset " << x26xPreset(opts.preset)
            << " -crf " << opts.quality
            << " -pix_fmt " << pixfmt;

        if (opts.hdrOutput && opts.codec == "hevc")
        {
            std::ostringstream x265p;
            x265p << "colorprim=bt2020:transfer=smpte2084:colormatrix=bt2020nc"
                  << ":master-display=G(13250,34500)B(7500,3000)R(34000,16000)WP(15635,16450)L(10000000,50)"
                  << ":max-cll=" << opts.maxLuminance << "," << (opts.maxLuminance / 2)
                  << ":hdr-opt=1:repeat-headers=1";
            cmd << " -x265-params \"" << x265p.str() << "\"";
        }
    }

    if (opts.hdrOutput)
    {
        cmd << " -color_primaries bt2020 -color_trc smpte2084 -colorspace bt2020nc";
        if (useNvenc)
        {
            const bool hasMD  = encoderHasOption(enc, "master_display");
            const bool hasCLL = encoderHasOption(enc, "max_cll");
            if (hasMD)
            {
                cmd << " -master_display"
                    << " \"G(13250,34500)B(7500,3000)R(34000,16000)"
                    << "WP(15635,16450)L(10000000,50)\"";
            }
            if (hasCLL)
            {
                cmd << " -max_cll \"" << opts.maxLuminance
                    << "," << (opts.maxLuminance / 2) << "\"";
            }
            if (!hasMD && !hasCLL)
            {
                fprintf(stderr,
                    "Note: bundled ffmpeg's %s doesn't support -master_display / "
                    "-max_cll (HDR10 static metadata).\n",
                    enc.c_str());
            }
        }
    }
    else
    {
        cmd << " -color_primaries bt709 -color_trc bt709 -colorspace bt709";
    }

    if (cont == Container::Mp4Like)
    {
        if (opts.codec == "hevc")
            cmd << " -tag:v hvc1";
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

bool FFmpegEncoder::writeFrame(const void* src, size_t bytes)
{
    if (!m_hStdinWrite) return false;
    return writeAll(m_hStdinWrite, src, bytes);
}

void FFmpegEncoder::finish()
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

void FFmpegEncoder::dumpStderrTail(const char* label)
{
    dumpStderrLog(m_stderrLog, label);
}
