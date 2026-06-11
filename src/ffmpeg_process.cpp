#include "ffmpeg_process.h"
#include "mp4_retime.h"
#include "win32_utils.h"

#include <algorithm>
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

uint32_t gcd32(uint32_t a, uint32_t b)
{
    while (b) { const uint32_t t = a % b; a = b; b = t; }
    return a;
}

// Parse ffprobe rational like "24000/1001" or plain "120".
bool parseFpsString(const std::string& s, uint32_t& num, uint32_t& den, double& fps)
{
    if (s.empty() || s == "0/0" || s == "N/A") return false;
    const auto slash = s.find('/');
    if (slash != std::string::npos)
    {
        num = static_cast<uint32_t>(std::strtoul(s.substr(0, slash).c_str(), nullptr, 10));
        den = static_cast<uint32_t>(std::strtoul(s.substr(slash + 1).c_str(), nullptr, 10));
        if (den == 0) return false;
        const uint32_t g = gcd32(num, den);
        num /= g;
        den /= g;
        fps = static_cast<double>(num) / static_cast<double>(den);
        return fps > 0.0;
    }
    fps = std::atof(s.c_str());
    if (fps <= 0.0) return false;
    num = static_cast<uint32_t>(fps * 1000.0 + 0.5);
    den = 1000;
    const uint32_t g = gcd32(num, den);
    num /= g;
    den /= g;
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// probeVideo (uses ffprobe)
// ---------------------------------------------------------------------------

bool probeVideo(const std::string& input, VideoInfo& out)
{
    // r_frame_rate is the nominal rate the container declares (e.g. "240/1"
    // for PUBG Game DVR). avg_frame_rate is measured from frame_count /
    // duration. VFR sources (Game DVR, OBS, phone capture) often declare a
    // high nominal rate while the real average is much lower -- using r_fps
    // for NVENC + muxer CFR timestamps makes the output video run ~2x fast
    // while audio stays on the original timeline.
    //
    // For CFR sources (r_fps ~= avg_fps within 5%), keep r_frame_rate: it
    // avoids the 25 ppm drift that avg_frame_rate rounding (119997/1000 vs
    // 120/1) can introduce on long 4K120 captures.
    std::string cmd =
        toolPath("ffprobe.exe") + " -v error -select_streams v:0 "
        "-show_entries stream=codec_name,width,height,r_frame_rate,avg_frame_rate,nb_frames,pix_fmt,bit_rate,color_transfer,color_primaries"
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

    uint32_t rNum = 0, rDen = 0, aNum = 0, aDen = 0;
    const bool haveR = parseFpsString(srfr, rNum, rDen, out.rFps);
    const bool haveA = parseFpsString(safr, aNum, aDen, out.avgFps);

    out.isVfr = false;
    if (haveR && haveA && out.rFps > 0.0 && out.avgFps > 0.0)
    {
        const double hi = out.rFps > out.avgFps ? out.rFps : out.avgFps;
        const double relDiff = std::fabs(out.rFps - out.avgFps) / hi;
        out.isVfr = relDiff > 0.05;
    }

    out.fps = 30.0;
    out.fpsNum = 30;
    out.fpsDen = 1;
    if (out.isVfr && haveA)
    {
        out.fps    = out.avgFps;
        out.fpsNum = aNum;
        out.fpsDen = aDen;
    }
    else if (haveR)
    {
        out.fps    = out.rFps;
        out.fpsNum = rNum;
        out.fpsDen = rDen;
    }
    else if (haveA)
    {
        out.fps    = out.avgFps;
        out.fpsNum = aNum;
        out.fpsDen = aDen;
    }

    out.frameCount = 0;
    if (!snb.empty() && snb != "N/A")
    {
        out.frameCount = _atoi64(snb.c_str());
    }

    // Format duration (seconds). Used for frame-count estimation and for
    // catching containers that stamp r/avg fps but whose packet timeline is
    // slower (common in MKV Game DVR / SVP exports).
    double durationSec = 0.0;
    {
        std::string sdur = getVal("duration");
        if (sdur.empty() || sdur == "N/A")
            sdur = getVal("TAG:DURATION");
        if (!sdur.empty() && sdur != "N/A")
        {
            if (sdur.find(':') != std::string::npos)
            {
                int hh = 0, mm = 0; double ss = 0.0;
                if (std::sscanf(sdur.c_str(), "%d:%d:%lf", &hh, &mm, &ss) == 3)
                    durationSec = hh * 3600.0 + mm * 60.0 + ss;
            }
            else
            {
                durationSec = std::atof(sdur.c_str());
            }
        }
    }

    // MKV / WebM often omit nb_frames; count packets once so we can compare
    // frames/duration against the declared fps.
    if (out.frameCount <= 0 && durationSec > 0.0)
    {
        const std::string cntCmd =
            toolPath("ffprobe.exe") + " -v error -count_packets "
            "-select_streams v:0 -show_entries stream=nb_read_packets "
            "-of default=noprint_wrappers=1:nokey=1 " + quote(input);
        std::string cntText;
        DWORD cntExit = 0;
        if (runAndCapture(cntCmd, cntText, cntExit) && cntExit == 0)
        {
            const int64_t n = _atoi64(trim(cntText).c_str());
            if (n > 0)
                out.frameCount = n;
        }
    }
    else if (out.frameCount <= 0 && durationSec > 0.0 && out.fps > 0.0)
    {
        out.frameCount = static_cast<int64_t>(durationSec * out.fps + 0.5);
    }

    // When metadata fps disagrees with frames/duration by >3%, trust the
    // measured rate for NVENC + muxer timing (fixes ~2 s A/V drift on SVP
    // MKV that declares 125 fps but plays over ~55 s for 6606 frames).
    if (out.frameCount > 1 && durationSec > 0.5 && haveR && out.rFps > 0.0)
    {
        const double measured =
            static_cast<double>(out.frameCount) / durationSec;
        const double relDiff = std::fabs(out.rFps - measured) / out.rFps;
        if (relDiff > 0.03)
        {
            out.fps    = measured;
            out.fpsNum = static_cast<uint32_t>(out.frameCount * 1000ULL);
            out.fpsDen = static_cast<uint32_t>(durationSec * 1000.0 + 0.5);
            const uint32_t g = gcd32(out.fpsNum, out.fpsDen);
            out.fpsNum /= g;
            out.fpsDen /= g;
            if (relDiff > 0.05)
                out.isVfr = true;
        }
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

    // HDR transfer detection: PQ (smpte2084) or HLG (arib-std-b67). Treat a
    // BT.2020 primary at >=10-bit as HDR too (some captures omit transfer).
    out.colorTransfer = getVal("color_transfer");
    const std::string prim = getVal("color_primaries");
    out.isHdr =
        out.colorTransfer == "smpte2084" ||
        out.colorTransfer == "arib-std-b67" ||
        (prim == "bt2020" && out.bitDepth >= 10);

    return out.width > 0 && out.height > 0;
}

bool parseFrameCrcPts(const std::string& path, FramePtsInfo& out)
{
    out.pts.clear();
    out.tbNum = 0;
    out.tbDen = 0;

    // framecrc layout:
    //   #tb 0: 1/90000
    //   ...more # headers...
    //   0,     <dts>,      <pts>,  <duration>, <size>, 0x<crc>
    std::string text;
    {
        FILE* f = _wfopen(toWide(path).c_str(), L"rb");
        if (!f) return false;
        char buf[1 << 16];
        size_t got;
        while ((got = fread(buf, 1, sizeof(buf), f)) > 0)
            text.append(buf, got);
        fclose(f);
    }

    out.pts.reserve(text.size() / 40);
    size_t missing = 0;
    size_t lineStart = 0;
    while (lineStart < text.size())
    {
        size_t lineEnd = text.find('\n', lineStart);
        if (lineEnd == std::string::npos) lineEnd = text.size();
        const std::string line = trim(text.substr(lineStart, lineEnd - lineStart));
        lineStart = lineEnd + 1;
        if (line.empty()) continue;

        if (line[0] == '#')
        {
            // "#tb 0: 1/90000"
            if (line.compare(0, 6, "#tb 0:") == 0)
            {
                const std::string value = trim(line.substr(6));
                const size_t slash = value.find('/');
                if (slash != std::string::npos)
                {
                    out.tbNum = static_cast<uint32_t>(
                        std::strtoul(value.substr(0, slash).c_str(), nullptr, 10));
                    out.tbDen = static_cast<uint32_t>(
                        std::strtoul(value.substr(slash + 1).c_str(), nullptr, 10));
                }
            }
            continue;
        }

        // "0,      -3938,          0,     1969,     8701, 0x7e799a02"
        //  stream  dts             pts
        const size_t c1 = line.find(',');
        if (c1 == std::string::npos || trim(line.substr(0, c1)) != "0")
            continue;                                  // not video stream 0
        const size_t c2 = line.find(',', c1 + 1);
        if (c2 == std::string::npos) continue;
        const size_t c3 = line.find(',', c2 + 1);
        const std::string ptsTok =
            trim(line.substr(c2 + 1,
                             (c3 == std::string::npos ? line.size() : c3) - c2 - 1));
        if (ptsTok.empty() || ptsTok == "N/A") { ++missing; continue; }
        out.pts.push_back(_atoi64(ptsTok.c_str()));
    }

    if (missing > 0)
    {
        // A packet without pts means we cannot build a complete display-order
        // timeline; the caller falls back to CFR rather than guessing.
        fprintf(stderr,
                "parseFrameCrcPts: %zu packet(s) without pts -- VFR passthrough "
                "disabled for this source.\n", missing);
        return false;
    }

    // Packets come in decode order; sorting yields display order, which is
    // what the decoder hands the pipeline frame by frame.
    std::sort(out.pts.begin(), out.pts.end());

    return out.tbNum > 0 && out.tbDen > 0 && !out.pts.empty();
}

namespace {

constexpr double kMuxMaxAvStartDeltaSec    = 0.5;
constexpr double kMuxMaxAvEndDeltaSec      = 1.0;
constexpr double kMuxAudioClusterFileRatio = 0.50;

double parseProbeDouble(const std::string& s)
{
    const std::string t = trim(s);
    if (t.empty() || t == "N/A") return -1.0;
    return std::atof(t.c_str());
}

// Largest packet pts_time at or after `fromSec` (reads up to 5000 packets).
double probeMaxPacketPtsFrom(const std::string& path,
                             const char* streamSel,
                             double fromSec)
{
    std::ostringstream cmd;
    cmd << toolPath("ffprobe.exe")
        << " -v error -select_streams " << streamSel
        << " -show_entries packet=pts_time -read_intervals "
        << fromSec << "%+#5000 -of csv=p=0 " << quote(path);

    std::string text;
    DWORD exit = 0;
    if (!runAndCapture(cmd.str(), text, exit) || exit != 0)
        return -1.0;

    double maxPts = -1.0;
    size_t lineStart = 0;
    while (lineStart < text.size())
    {
        size_t lineEnd = text.find('\n', lineStart);
        if (lineEnd == std::string::npos) lineEnd = text.size();
        const double pts = parseProbeDouble(text.substr(lineStart, lineEnd - lineStart));
        if (pts >= 0.0 && pts > maxPts)
            maxPts = pts;
        lineStart = lineEnd + 1;
    }
    return maxPts;
}

} // namespace

bool verifyMuxOutput(const std::string& output, bool verbose)
{
    std::string fmtCmd =
        toolPath("ffprobe.exe") + " -v error -show_entries format=size,duration "
        "-of csv=p=0 " + quote(output);
    std::string fmtText;
    DWORD fmtExit = 0;
    if (!runAndCapture(fmtCmd, fmtText, fmtExit) || fmtExit != 0)
    {
        fprintf(stderr, "verifyMuxOutput: ffprobe failed on '%s'.\n", output.c_str());
        return false;
    }

    double formatDuration = -1.0;
    int64_t fileSize = 0;
    {
        const std::string line = trim(fmtText);
        const size_t comma = line.find(',');
        if (comma != std::string::npos)
        {
            formatDuration = parseProbeDouble(line.substr(0, comma));
            fileSize = _atoi64(trim(line.substr(comma + 1)).c_str());
        }
    }

    std::string streamCmd =
        toolPath("ffprobe.exe") + " -v error "
        "-show_entries stream=codec_type,start_time,duration -of csv=p=0 "
        + quote(output);
    std::string streamText;
    DWORD streamExit = 0;
    if (!runAndCapture(streamCmd, streamText, streamExit) || streamExit != 0)
    {
        fprintf(stderr, "verifyMuxOutput: ffprobe stream probe failed on '%s'.\n",
                output.c_str());
        return false;
    }

    double videoStart = -1.0;
    double audioStart = -1.0;
    bool hasVideo = false;
    bool hasAudio = false;

    size_t lineStart = 0;
    while (lineStart < streamText.size())
    {
        size_t lineEnd = streamText.find('\n', lineStart);
        if (lineEnd == std::string::npos) lineEnd = streamText.size();
        const std::string line = trim(streamText.substr(lineStart, lineEnd - lineStart));
        lineStart = lineEnd + 1;
        if (line.empty()) continue;

        const size_t c1 = line.find(',');
        const size_t c2 = (c1 == std::string::npos) ? std::string::npos : line.find(',', c1 + 1);
        if (c1 == std::string::npos) continue;

        const std::string codec = trim(line.substr(0, c1));
        const double start = parseProbeDouble(
            trim(line.substr(c1 + 1,
                              (c2 == std::string::npos ? line.size() : c2) - c1 - 1)));

        if (codec == "video" && !hasVideo)
        {
            hasVideo = true;
            videoStart = start;
        }
        else if (codec == "audio" && !hasAudio)
        {
            hasAudio = true;
            audioStart = start;
        }
    }

    if (!hasVideo)
    {
        fprintf(stderr, "verifyMuxOutput: no video stream in '%s'.\n", output.c_str());
        return false;
    }

    bool ok = true;

    if (hasAudio)
    {
        const double startDelta = std::fabs(videoStart - audioStart);
        if (videoStart >= 0.0 && audioStart >= 0.0 &&
            startDelta > kMuxMaxAvStartDeltaSec)
        {
            fprintf(stderr,
                    "verifyMuxOutput: A/V start misaligned by %.3f s "
                    "(video %.3f s, audio %.3f s; max %.1f s).\n"
                    "  Symptom: silent video or audio-only at the beginning.\n",
                    startDelta, videoStart, audioStart, kMuxMaxAvStartDeltaSec);
            ok = false;
        }

        if (formatDuration > 0.0)
        {
            const double fromSec = (formatDuration > 2.0) ? (formatDuration - 2.0) : 0.0;
            const double videoEnd = probeMaxPacketPtsFrom(output, "v:0", fromSec);
            const double audioEnd = probeMaxPacketPtsFrom(output, "a:0", fromSec);
            if (videoEnd >= 0.0 && audioEnd >= 0.0)
            {
                const double endDelta = std::fabs(videoEnd - audioEnd);
                if (endDelta > kMuxMaxAvEndDeltaSec)
                {
                    fprintf(stderr,
                            "verifyMuxOutput: A/V end misaligned by %.3f s "
                            "(video ~%.3f s, audio ~%.3f s; max %.1f s).\n",
                            endDelta, videoEnd, audioEnd, kMuxMaxAvEndDeltaSec);
                    ok = false;
                }
            }
            else if (verbose)
            {
                fprintf(stderr,
                        "verifyMuxOutput: skipped end alignment check "
                        "(could not read tail packets).\n");
            }
        }

        if (fileSize > 0)
        {
            std::string posCmd =
                toolPath("ffprobe.exe") + " -v error -select_streams a:0 "
                "-show_entries packet=pos -read_intervals \"%+#3\" -of csv=p=0 "
                + quote(output);
            std::string posText;
            DWORD posExit = 0;
            if (runAndCapture(posCmd, posText, posExit) && posExit == 0)
            {
                std::vector<int64_t> positions;
                size_t ps = 0;
                while (ps < posText.size())
                {
                    size_t pe = posText.find('\n', ps);
                    if (pe == std::string::npos) pe = posText.size();
                    const std::string tok = trim(posText.substr(ps, pe - ps));
                    ps = pe + 1;
                    if (!tok.empty() && tok != "N/A")
                        positions.push_back(_atoi64(tok.c_str()));
                }
                if (positions.size() >= 2)
                {
                    const double ratio =
                        static_cast<double>(positions[1]) /
                        static_cast<double>(fileSize);
                    if (ratio > kMuxAudioClusterFileRatio)
                    {
                        fprintf(stderr,
                                "verifyMuxOutput: audio not interleaved "
                                "(2nd audio packet at %.1f%% of file; max %.0f%%).\n"
                                "  Symptom: no sound until seeking near the end.\n",
                                ratio * 100.0, kMuxAudioClusterFileRatio * 100.0);
                        ok = false;
                    }
                }
            }
        }
    }

    if (verbose && ok)
    {
        if (hasAudio)
        {
            fprintf(stderr,
                    "verifyMuxOutput: OK (video %.3f s, audio %.3f s, "
                    "duration %.3f s, size %lld bytes).\n",
                    videoStart >= 0.0 ? videoStart : 0.0,
                    audioStart >= 0.0 ? audioStart : 0.0,
                    formatDuration >= 0.0 ? formatDuration : 0.0,
                    static_cast<long long>(fileSize));
        }
        else
        {
            fprintf(stderr,
                    "verifyMuxOutput: OK (video-only, start %.3f s, "
                    "duration %.3f s, size %lld bytes).\n",
                    videoStart >= 0.0 ? videoStart : 0.0,
                    formatDuration >= 0.0 ? formatDuration : 0.0,
                    static_cast<long long>(fileSize));
        }
    }

    if (!ok)
    {
        fprintf(stderr,
                "verifyMuxOutput: re-encode from the original source if possible.\n"
                "  Manual remux with ffmpeg -c copy may salvage a broken export.\n");
    }

    return ok;
}

bool remuxCopyAudio(const std::string& videoOnly,
                    const std::string& audioSource,
                    const std::string& output,
                    const std::string& codec,
                    uint32_t fpsNum,
                    uint32_t fpsDen,
                    bool hdr10,
                    bool verbose,
                    const FramePtsInfo* framePts)
{
    std::string esFmt = "hevc";
    if (codec == "h264" || codec == "avc")
        esFmt = "h264";
    else if (codec == "hevc" || codec == "h265")
        esFmt = "hevc";

    // Elementary stream + external audio in one ffmpeg pass leaves audio
    // clustered at EOF in Matroska. ES callers get containerized into a
    // video-only file first; the GPU pipeline now pipe-muxes straight into a
    // video-only MP4 during encode, which skips that whole extra disk pass.
    std::string containerVideo = videoOnly;
    std::string tempContainer;
    const bool esInput =
        (videoOnly.size() > 5 &&
         videoOnly.compare(videoOnly.size() - 5, 5, ".hevc") == 0) ||
        (videoOnly.size() > 5 &&
         videoOnly.compare(videoOnly.size() - 5, 5, ".h264") == 0);
    const bool mp4Input =
        (videoOnly.size() > 4 &&
         videoOnly.compare(videoOnly.size() - 4, 4, ".mp4") == 0);

    // VFR passthrough (#32): rewrite the video-only MP4's sample table with
    // the source's per-frame timestamps; the merge pass below stream-copies
    // them into the final container. Any failure falls back to CFR timing.
    bool vfrApplied = false;
    const bool wantVfr =
        framePts && framePts->pts.size() >= 2 && framePts->tbDen > 0;

    if (esInput)
    {
        const bool toMp4 = wantVfr;   // patch needs MP4; otherwise keep MKV
        tempContainer = videoOnly + (toMp4 ? ".mux.mp4" : ".mux.mkv");
        std::ostringstream cmd1;
        cmd1 << toolPath("ffmpeg.exe")
             << " -nostdin -loglevel " << (verbose ? "info" : "error") << " -y "
             << "-f " << esFmt << " ";
        if (fpsNum && fpsDen)
            cmd1 << "-framerate " << fpsNum << "/" << fpsDen << " ";
        cmd1 << "-i " << quote(videoOnly) << " -c copy " << quote(tempContainer);

        std::string ignored;
        DWORD exit1 = 0;
        if (!runAndCapture(cmd1.str(), ignored, exit1) || exit1 != 0)
        {
            fprintf(stderr, "remuxCopyAudio: failed to containerize elementary stream.\n"
                            "  video: %s\n", videoOnly.c_str());
            return false;
        }
        containerVideo = tempContainer;
        if (toMp4)
            vfrApplied = applyMp4FramePts(tempContainer, *framePts, verbose);
    }
    else if (mp4Input && wantVfr)
    {
        // Video-only MP4 produced by the pipe muxer during encode.
        vfrApplied = applyMp4FramePts(videoOnly, *framePts, verbose);
    }

    if (wantVfr && !vfrApplied)
        fprintf(stderr,
                "remuxCopyAudio: VFR passthrough failed; "
                "falling back to CFR %u/%u fps.\n", fpsNum, fpsDen);

    // When VFR passthrough is active the re-stamped video starts at t=0 (stts
    // can only encode deltas), but the source's video stream may start later
    // than its earliest stream (e.g. ffmpeg's MKV muxer shifts video to keep
    // dts non-negative). The merge pass re-applies that offset via -itsoffset
    // so the copied audio lines up exactly.
    double vfrStartOffsetSec = 0.0;
    if (vfrApplied)
    {
        printf("  mux: VFR timestamps from source applied to %zu frames\n",
               framePts->pts.size());
        fflush(stdout);

        // Source video start relative to the file start (= what the merge
        // pass normalises the copied audio against).
        const double videoStart =
            double(framePts->pts.front()) * framePts->tbNum / framePts->tbDen;
        double fileStart = 0.0;
        {
            std::string t;
            DWORD e = 0;
            if (runAndCapture(
                    toolPath("ffprobe.exe") +
                    " -v error -show_entries format=start_time "
                    "-of csv=p=0 " + quote(audioSource), t, e) && e == 0)
            {
                const double v = parseProbeDouble(trim(t));
                if (v >= 0.0) fileStart = v;
            }
        }
        vfrStartOffsetSec = videoStart - fileStart;
        if (std::fabs(vfrStartOffsetSec) < 0.001)
            vfrStartOffsetSec = 0.0;
    }

    std::ostringstream cmd;
    cmd << toolPath("ffmpeg.exe")
        << " -nostdin -loglevel " << (verbose ? "info" : "error") << " -y ";
    if (vfrStartOffsetSec != 0.0)
        cmd << "-itsoffset " << vfrStartOffsetSec << " ";
    cmd << "-i " << quote(containerVideo)
        << " -i " << quote(audioSource)
        << " -map 0:v:0 -map 1:a:0 -c copy";

    if (hdr10)
        cmd << " -color_primaries bt2020 -color_trc smpte2084 -colorspace bt2020nc";
    else
        cmd << " -color_primaries bt709 -color_trc bt709 -colorspace bt709";

    const auto dot = output.find_last_of('.');
    if (dot != std::string::npos)
    {
        std::string ext;
        for (size_t i = dot + 1; i < output.size(); ++i)
            ext.push_back(static_cast<char>(::tolower(output[i])));
        if (ext == "mp4" || ext == "m4v" || ext == "mov")
        {
            if (esFmt == "h264")
                cmd << " -tag:v avc1";
            else
                cmd << " -tag:v hvc1";
            // No +faststart -- it rewrites the whole multi-GB output once
            // more just to front-load moov (see bitstream_pipe.cpp).
        }
    }

    cmd << " " << quote(output);

    std::string ignored;
    DWORD exit = 0;
    if (!runAndCapture(cmd.str(), ignored, exit) || exit != 0)
    {
        fprintf(stderr, "remuxCopyAudio: failed to merge audio from source.\n"
                        "  video: %s\n  audio source: %s\n  output: %s\n",
                containerVideo.c_str(), audioSource.c_str(), output.c_str());
        if (!tempContainer.empty())
            DeleteFileW(toWide(tempContainer).c_str());
        return false;
    }

    if (!tempContainer.empty())
        DeleteFileW(toWide(tempContainer).c_str());

    if (!verifyMuxOutput(output, verbose))
    {
        fprintf(stderr, "remuxCopyAudio: output removed after failed A/V sanity check.\n");
        DeleteFileW(toWide(output).c_str());
        return false;
    }
    return true;
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

    std::ostringstream fr;
    if (opts.fpsNum && opts.fpsDen)
        fr << opts.fpsNum << "/" << opts.fpsDen;
    else
        fr << opts.fps;

    if (opts.hdrOutput)
    {
        cmd << "-f rawvideo -pixel_format x2bgr10le "
            << "-video_size " << opts.width << "x" << opts.height << " "
            << "-framerate " << fr.str() << " "
            << "-colorspace bt2020nc -color_primaries bt2020 -color_trc smpte2084 "
            << "-i pipe:0 ";
    }
    else
    {
        cmd << "-f rawvideo -pixel_format rgba "
            << "-video_size " << opts.width << "x" << opts.height << " "
            << "-framerate " << fr.str() << " "
            << "-colorspace bt709 -color_primaries bt709 -color_trc bt709 "
            << "-i pipe:0 ";
    }

    if (opts.copyAudio)
        cmd << "-i " << quote(sourceForAudio) << " -map 0:v:0 -map 1:a:0 -c:a copy ";
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
