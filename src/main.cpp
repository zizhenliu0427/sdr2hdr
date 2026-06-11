// sdr2hdr.exe : RTX Video SDK front-end with CLI, batch and interactive modes.
//
// Three launch paths:
//   1. Interactive wizard   : no args  OR  --interactive
//   2. Single-file CLI      : sdr2hdr.exe in.mp4 out.mp4 --hdr
//   3. Batch CLI            : sdr2hdr.exe a.mp4 b.mp4 c.mp4 -o out_dir --hdr
//
// Modes:
//   --hdr        SDR -> HDR (TrueHDR),      keeps original resolution
//   --vsr        SDR -> SDR (super-res),    upscaled
//   --vsr-hdr    upscale + SDR -> HDR combined
//
// Pipeline per file:
//   ffmpeg(NVDEC) --pipe--> sdr2hdr.exe (CUDA + RTX Video SDK) --pipe--> ffmpeg(NVENC)

#include "ffmpeg_process.h"
#include "rtx_converter.h"
#include "gpu_pipeline.h"
#include "deps.h"
#include "i18n.h"
#include "engine.h"
#include "path_utils.h"

// Hybrid-graphics (iGPU + dGPU) machines: ask the drivers to bind this
// process to the discrete GPU directly. Without it, NVIDIA's hybrid CUDA
// shim (nvcudart_hybrid64.dll) interposes device enumeration for windowed
// processes, and NGX's GetLUIDFromCudaDevice then smashes its own stack
// inside NVSDK_NGX_CUDA_Init -> 0xC0000409 crash at conversion start.
// Console processes are never routed through the shim, which is why the
// CLI build never crashed. Standard exports, read straight from the exe's
// export table by both vendors' drivers.
extern "C" {
__declspec(dllexport) unsigned long NvOptimusEnablement = 0x00000001;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}


#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>
#include <chrono>
#include <iostream>
#include <filesystem>
#include <algorithm>

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
  #include <commdlg.h>
  #include <shlobj.h>
  #include <shellapi.h>
  #pragma comment(lib, "comdlg32.lib")
  #pragma comment(lib, "shell32.lib")
  #pragma comment(lib, "ole32.lib")
#endif

namespace fs = std::filesystem;

using sdr2hdr::pathFromUtf8;
using sdr2hdr::utf8FromPath;
using sdr2hdr::extOf;
using sdr2hdr::replaceExt;
using sdr2hdr::isVideoExt;
using sdr2hdr::parseSize;
using sdr2hdr::autoSuffix;
using sdr2hdr::resolveOutputPath;

namespace {
using i18n::tr;
using i18n::trs;

// --------------------------------------------------------------------------
// Options: everything EXCEPT input/output paths (those are per-file Job data).
// --------------------------------------------------------------------------

using Options = sdr2hdr::ProcessOptions;

// --cancel-event <name>: internal flag used by the GUI, which runs each
// conversion in a worker process (crash isolation from the NVIDIA NGX/hybrid
// -graphics bug -- see ngx_session.cpp). Signalling the named event cancels
// the conversion cooperatively, so temp files and partial output are cleaned
// up exactly like the in-process cancel path.
std::atomic<bool> g_cliCancel{false};
std::string       g_cancelEventName;

void startCancelEventWatcher()
{
    if (g_cancelEventName.empty()) return;
    std::wstring wname(g_cancelEventName.begin(), g_cancelEventName.end());
    HANDLE h = OpenEventW(SYNCHRONIZE, FALSE, wname.c_str());
    if (!h) return;
    std::thread([h]() {
        WaitForSingleObject(h, INFINITE);
        g_cliCancel.store(true);
        CloseHandle(h);
    }).detach();
}

// Global flag: was the exe launched in a way that warrants a pause-on-exit?
bool g_interactiveLaunch = false;

// Global flag: did the user already pick UI language explicitly via
// `--lang en|zh` (or SDR2HDR_LANG env var)? If so, the wizard skips its own
// language prompt -- otherwise it shows one at the very start with the
// auto-detected OS language pre-selected as default.
bool g_langExplicit = false;

// --------------------------------------------------------------------------
// Path / file utilities (see path_utils.h; pickers below still need wide helpers)
// --------------------------------------------------------------------------

// --------------------------------------------------------------------------
// Native Windows pickers (comdlg32 / shell32)
// --------------------------------------------------------------------------

#ifdef _WIN32

std::string wideToUtf8(const wchar_t* w)
{
    if (!w || !*w) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string s(static_cast<size_t>(len - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), len, nullptr, nullptr);
    return s;
}

std::wstring utf8ToWide(const std::string& s)
{
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (len <= 0) return {};
    std::wstring w(static_cast<size_t>(len - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), len);
    return w;
}

// Video file filter used by the open dialog.
const wchar_t* kVideoFilter =
    L"Video files (*.mp4;*.m4v;*.mov;*.mkv;*.ts;*.m2ts;*.mts;*.avi;*.webm;*.wmv;*.flv;*.mpg;*.mpeg)\0"
    L"*.mp4;*.m4v;*.mov;*.mkv;*.ts;*.m2ts;*.mts;*.avi;*.webm;*.wmv;*.flv;*.mpg;*.mpeg\0"
    L"All files (*.*)\0*.*\0";

// Multi-select Explorer file picker. Returns true if the user picked >=1 file.
bool pickInputFilesDialog(std::vector<std::string>& out)
{
    // OFN_ALLOWMULTISELECT requires a large buffer: first path is the
    // directory, followed by null-separated file names, double-null terminated.
    std::vector<wchar_t> buf(65536, L'\0');

    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = GetConsoleWindow();
    ofn.lpstrFilter = kVideoFilter;
    ofn.lpstrFile   = buf.data();
    ofn.nMaxFile    = static_cast<DWORD>(buf.size());
    ofn.lpstrTitle  = L"Select input video(s)  --  Ctrl/Shift + click for multi-select";
    ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST
              | OFN_ALLOWMULTISELECT | OFN_NOCHANGEDIR | OFN_HIDEREADONLY;

    if (!GetOpenFileNameW(&ofn))
        return false;   // cancelled or error

    // Parse output:
    //   single : "C:\dir\file.mp4\0\0"
    //   multi  : "C:\dir\0file1.mp4\0file2.mp4\0\0"
    const wchar_t* p = buf.data();
    std::wstring first = p;
    p += first.size() + 1;

    if (*p == L'\0')
    {
        out.push_back(wideToUtf8(first.c_str()));
    }
    else
    {
        std::wstring dir = first;
        while (*p != L'\0')
        {
            std::wstring name = p;
            p += name.size() + 1;
            std::wstring full = dir;
            if (!full.empty() && full.back() != L'\\' && full.back() != L'/')
                full += L'\\';
            full += name;
            out.push_back(wideToUtf8(full.c_str()));
        }
    }
    return !out.empty();
}

// Folder picker (for batch output directory). BIF_NEWDIALOGSTYLE gives the
// modern "choose folder" dialog; requires CoInitialize.
bool pickFolderDialog(const std::string& title, std::string& out)
{
    std::wstring wTitle = utf8ToWide(title);

    BROWSEINFOW bi{};
    bi.hwndOwner = GetConsoleWindow();
    bi.lpszTitle = wTitle.c_str();
    bi.ulFlags   = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE | BIF_USENEWUI;

    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return false;

    wchar_t path[MAX_PATH] = {0};
    BOOL ok = SHGetPathFromIDListW(pidl, path);
    CoTaskMemFree(pidl);
    if (!ok) return false;

    out = wideToUtf8(path);
    return !out.empty();
}

// Save-as picker for single output file.
bool pickSaveFileDialog(const std::string& defaultPath,
                        const std::string& title,
                        std::string& out)
{
    std::wstring wDefault = utf8ToWide(defaultPath);
    std::wstring wTitle   = utf8ToWide(title);

    // nMaxFile must include space for the full path.
    std::vector<wchar_t> buf(4096, L'\0');
    if (!wDefault.empty())
    {
        wcsncpy_s(buf.data(), buf.size(), wDefault.c_str(), _TRUNCATE);
    }

    // Derive default extension from the suggested path.
    std::wstring defExt;
    if (auto dot = wDefault.find_last_of(L'.'); dot != std::wstring::npos)
        defExt = wDefault.substr(dot + 1);

    OPENFILENAMEW ofn{};
    ofn.lStructSize  = sizeof(ofn);
    ofn.hwndOwner    = GetConsoleWindow();
    ofn.lpstrFilter  = kVideoFilter;
    ofn.lpstrFile    = buf.data();
    ofn.nMaxFile     = static_cast<DWORD>(buf.size());
    ofn.lpstrTitle   = wTitle.empty() ? L"Select output file" : wTitle.c_str();
    ofn.lpstrDefExt  = defExt.empty() ? nullptr : defExt.c_str();
    ofn.Flags = OFN_EXPLORER | OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT
              | OFN_NOCHANGEDIR | OFN_HIDEREADONLY;

    if (!GetSaveFileNameW(&ofn))
        return false;

    out = wideToUtf8(buf.data());
    return !out.empty();
}

#else
// Non-Windows stubs (this tool is Windows-only today; keep compile-safe).
bool pickInputFilesDialog(std::vector<std::string>&) { return false; }
bool pickFolderDialog(const std::string&, std::string&) { return false; }
bool pickSaveFileDialog(const std::string&, const std::string&, std::string&) { return false; }
#endif

const char* modeName(RtxConverter::Mode m)
{
    switch (m) {
        case RtxConverter::Mode::Hdr:    return "TrueHDR";
        case RtxConverter::Mode::Vsr:    return "VSR";
        case RtxConverter::Mode::VsrHdr: return "VSR + TrueHDR";
    }
    return "?";
}

// --------------------------------------------------------------------------
// Usage / argument parsing
// --------------------------------------------------------------------------

void printUsage(const char* exe)
{
    const char* fmt = tr(
        "sdr2hdr.exe -- RTX Video SDK command-line tool (HDR / VSR / VSR+HDR)\n"
        "\n"
        "Interactive (wizard):\n"
        "  %s                      Launch interactive wizard\n"
        "  %s --interactive        Force wizard even with CLI args\n"
        "\n"
        "CLI single-file:\n"
        "  %s <input> <output> --<mode> [options]\n"
        "\n"
        "CLI batch (multiple inputs):\n"
        "  %s <input1> [input2 ...] -o <output_dir> --<mode> [options]\n"
        "  (omit -o to write each output alongside its input)\n"
        "\n"
        "Modes (pick exactly one):\n"
        "  --hdr              SDR -> HDR (same resolution)\n"
        "  --vsr              Video Super Resolution upscale (stays SDR)\n"
        "  --vsr-hdr          VSR upscale AND SDR -> HDR\n"
        "\n"
        "VSR options (applicable to --vsr / --vsr-hdr):\n"
        "  --vsr-quality N    VSR quality 1-4                (default 4, max quality)\n"
        "  Output size -- pick ONE of these (default is --4k):\n"
        "    --1080p          Upscale to 1920x1080\n"
        "    --4k             Upscale to UHD 4K   (height=2160)   <-- default\n"
        "    --8k             Upscale to UHD 8K   (height=4320)\n"
        "    --scale F        Output = input * F\n"
        "    --output-size WxH Explicit output size\n"
        "\n"
        "TrueHDR options (applicable to --hdr / --vsr-hdr):\n"
        "  --max-lum N        Target peak nits, 400-2000     (default 1000)\n"
        "  --contrast N       0-200                          (default 100)\n"
        "  --saturation N     0-200                          (default 100)\n"
        "  --middle-grey N    10-100                         (default 50)\n"
        "                     (alias: --middle-gray)\n"
        "\n"
        "Encoder / pipeline options:\n"
        "  --codec C          'hevc' (default) | 'h264' | 'av1'\n"
        "  --backend B        'nvenc' (default) | 'software'\n"
        "  --quality N        Quality factor (nvenc -cq / software -crf), default 19\n"
        "                     Use --quality auto to derive CQP per file from the\n"
        "                     probed source bitrate (high-bitrate source -> tighter q).\n"
        "  --preset P         Encoder preset (auto-mapped between encoders)\n"
        "  --no-hw-decode     Disable NVDEC decoding (legacy pipeline only)\n"
        "  --no-audio         Skip source audio copy\n"
        "  --no-vfr-pts       Disable VFR timestamp passthrough; force CFR at the\n"
        "                     average fps (mid-file A/V drift on VFR sources)\n"
        "  --ngx-sessions N   Parallel RTX AI sessions, 1-4 (default 1; >1 currently\n"
        "                     rejected by the NVIDIA driver, falls back to 1)\n"
        "  --gpu-only         Keep frames in VRAM end-to-end (default, fastest)\n"
        "  --legacy           Old pipeline: ffmpeg pixel pipes + host-pointer RTX\n"
        "  --verbose          Show ffmpeg stderr\n"
        "  --lang L           UI language: 'auto' (default) | 'en' | 'zh'\n"
        "  -o PATH            Explicit output path (file if single input, else directory)\n"
        "  -i, --interactive  Launch wizard even when CLI args are present\n"
        "  -h, --help         This help\n"
        "\n"
        "Examples:\n"
        "  %s                                                    (wizard)\n"
        "  %s in.mp4 out.mp4 --hdr\n"
        "  %s a.mp4 b.mp4 c.mp4 --hdr                            (batch, outputs next to inputs)\n"
        "  %s *.mp4 -o D:\\out --vsr --4k                         (batch to directory)\n"
        "  %s in.mp4 out.mp4 --vsr-hdr --4k --max-lum 1000       (upscale + HDR)\n",
        // Chinese version
        "sdr2hdr.exe -- RTX Video SDK 命令行工具 (HDR / VSR / VSR+HDR)\n"
        "\n"
        "交互模式 (向导):\n"
        "  %s                      启动交互向导\n"
        "  %s --interactive        即使带参数也强制进入向导\n"
        "\n"
        "命令行单文件:\n"
        "  %s <输入> <输出> --<模式> [选项]\n"
        "\n"
        "命令行批处理 (多文件):\n"
        "  %s <输入1> [输入2 ...] -o <输出目录> --<模式> [选项]\n"
        "  (省略 -o 则输出写到各输入的同级目录)\n"
        "\n"
        "模式 (只能选一个):\n"
        "  --hdr              SDR -> HDR (保持分辨率)\n"
        "  --vsr              视频超分放大 (保持 SDR)\n"
        "  --vsr-hdr          VSR 放大 + SDR -> HDR\n"
        "\n"
        "VSR 选项 (适用于 --vsr / --vsr-hdr):\n"
        "  --vsr-quality N    VSR 质量 1-4                  (默认 4，最高质量)\n"
        "  输出尺寸 -- 以下四选一 (默认 --4k):\n"
        "    --1080p          放大到 1920x1080\n"
        "    --4k             放大到 UHD 4K   (高度=2160)   <-- 默认\n"
        "    --8k             放大到 UHD 8K   (高度=4320)\n"
        "    --scale F        输出 = 输入 * F\n"
        "    --output-size WxH 显式指定输出尺寸\n"
        "\n"
        "TrueHDR 选项 (适用于 --hdr / --vsr-hdr):\n"
        "  --max-lum N        目标峰值尼特 400-2000          (默认 1000)\n"
        "  --contrast N       对比度 0-200                  (默认 100)\n"
        "  --saturation N     饱和度 0-200                  (默认 100)\n"
        "  --middle-grey N    中灰 10-100                   (默认 50)\n"
        "                     (别名: --middle-gray)\n"
        "\n"
        "编码器 / 管线选项:\n"
        "  --codec C          'hevc' (默认) | 'h264' | 'av1'\n"
        "  --backend B        'nvenc' (默认) | 'software'\n"
        "  --quality N        质量因子 (nvenc -cq / software -crf), 默认 19\n"
        "                     用 --quality auto 时按探测到的源码率自适应每个文件的\n"
        "                     CQP (源码率越高 -> 选越紧的 q)。\n"
        "  --preset P         编码预设 (跨编码器自动映射)\n"
        "  --no-hw-decode     关闭 NVDEC 硬解 (仅 legacy 管线有效)\n"
        "  --no-audio         不复制源音频\n"
        "  --no-vfr-pts       关闭 VFR 时间戳透传，强制按平均帧率 CFR 打点\n"
        "                     (VFR 源的视频中段可能音画漂移)\n"
        "  --ngx-sessions N   并行 RTX AI 会话数 1-4 (默认 1；>1 目前会被\n"
        "                     英伟达驱动拒绝并自动回退到 1)\n"
        "  --gpu-only         画面全程驻留显存 (默认，最快)\n"
        "  --legacy           旧管线：ffmpeg 像素管道 + 主机指针 RTX\n"
        "  --verbose          显示 ffmpeg 标准错误\n"
        "  --lang L           界面语言：'auto' (默认) | 'en' | 'zh'\n"
        "  -o PATH            显式输出路径 (单输入时为文件，多输入时为目录)\n"
        "  -i, --interactive  即使带命令行参数也启动向导\n"
        "  -h, --help         显示此帮助\n"
        "\n"
        "示例:\n"
        "  %s                                                    (向导)\n"
        "  %s in.mp4 out.mp4 --hdr\n"
        "  %s a.mp4 b.mp4 c.mp4 --hdr                            (批处理，输出与输入同目录)\n"
        "  %s *.mp4 -o D:\\out --vsr --4k                         (批处理到目录)\n"
        "  %s in.mp4 out.mp4 --vsr-hdr --4k --max-lum 1000       (放大 + HDR)\n");
    printf(fmt, exe, exe, exe, exe, exe, exe, exe, exe, exe);
}

enum class ParseResult { Ok, Help, Error };

ParseResult parseArgs(int argc, char** argv,
                      std::vector<std::string>& positional,
                      std::string& outputTarget,
                      bool& outputExplicit,
                      bool& wantWizard,
                      Options& opts)
{
    // --help anywhere
    for (int i = 1; i < argc; ++i)
    {
        std::string a = argv[i];
        if (a == "-h" || a == "--help") { printUsage(argv[0]); return ParseResult::Help; }
    }

    int modeCount = 0;

    for (int i = 1; i < argc; ++i)
    {
        std::string a = argv[i];
        auto nextU = [&](uint32_t& v) {
            if (i + 1 >= argc) { fprintf(stderr, "Missing value for %s\n", a.c_str()); return false; }
            v = static_cast<uint32_t>(std::atoi(argv[++i])); return true;
        };
        auto nextI = [&](int& v) {
            if (i + 1 >= argc) { fprintf(stderr, "Missing value for %s\n", a.c_str()); return false; }
            v = std::atoi(argv[++i]); return true;
        };
        auto nextD = [&](double& v) {
            if (i + 1 >= argc) { fprintf(stderr, "Missing value for %s\n", a.c_str()); return false; }
            v = std::atof(argv[++i]); return true;
        };
        auto nextS = [&](std::string& v) {
            if (i + 1 >= argc) { fprintf(stderr, "Missing value for %s\n", a.c_str()); return false; }
            v = argv[++i]; return true;
        };

        if      (a == "--hdr")         { opts.mode = RtxConverter::Mode::Hdr;    opts.modeSet = true; ++modeCount; }
        else if (a == "--vsr")         { opts.mode = RtxConverter::Mode::Vsr;    opts.modeSet = true; ++modeCount; }
        else if (a == "--vsr-hdr" ||
                 a == "--vsrhdr"  ||
                 a == "--both")        { opts.mode = RtxConverter::Mode::VsrHdr; opts.modeSet = true; ++modeCount; }
        else if (a == "--vsr-quality") { if (!nextU(opts.vsrQuality)) return ParseResult::Error; }
        else if (a == "--scale")       { if (!nextD(opts.scale)) return ParseResult::Error; opts.scaleSet = true; }
        else if (a == "--output-size") {
            std::string v;
            if (!nextS(v) || !parseSize(v, opts.outW, opts.outH)) {
                fprintf(stderr, "Invalid --output-size (want WxH, e.g. 3840x2160)\n");
                return ParseResult::Error;
            }
            opts.outputSizeSet = true;
        }
        else if (a == "--1080p")       { opts.targetHeight = 1080; }
        else if (a == "--4k"    ||
                 a == "--2160p")       { opts.targetHeight = 2160; }
        else if (a == "--8k"    ||
                 a == "--4320p")       { opts.targetHeight = 4320; }
        else if (a == "--contrast")    { if (!nextU(opts.contrast))   return ParseResult::Error; }
        else if (a == "--saturation")  { if (!nextU(opts.saturation)) return ParseResult::Error; }
        else if (a == "--middle-grey" ||
                 a == "--middle-gray")  { if (!nextU(opts.middleGray)) return ParseResult::Error; }
        else if (a == "--max-lum")     { if (!nextU(opts.maxLum))     return ParseResult::Error; }
        else if (a == "--backend" ||
                 a == "--encoder")     {
            if (!nextS(opts.backend)) return ParseResult::Error;
            if (opts.backend == "x265" || opts.backend == "cpu") opts.backend = "software";
            if (opts.backend != "nvenc" && opts.backend != "software") {
                fprintf(stderr, "Invalid --backend '%s' (want 'nvenc' or 'software')\n", opts.backend.c_str());
                return ParseResult::Error;
            }
        }
        else if (a == "--codec")       {
            if (!nextS(opts.codec)) return ParseResult::Error;
            if (opts.codec == "h265") opts.codec = "hevc";
            if (opts.codec == "avc")  opts.codec = "h264";
            if (opts.codec != "hevc" && opts.codec != "h264" && opts.codec != "av1") {
                fprintf(stderr, "Invalid --codec '%s' (want 'hevc' | 'h264' | 'av1')\n", opts.codec.c_str());
                return ParseResult::Error;
            }
        }
        else if (a == "--quality" || a == "--crf" || a == "--cq")
        {
            // Accept either an integer CQP or the literal word "auto".
            // We peek before nextI() to avoid its parse-error logging.
            if (i + 1 >= argc) {
                fprintf(stderr, "Missing value for %s\n", a.c_str());
                return ParseResult::Error;
            }
            std::string v = argv[i + 1];
            if (v == "auto" || v == "match" || v == "match-source")
            {
                opts.qualityAuto = true;
                opts.quality     = -1;
                ++i;
            }
            else
            {
                if (!nextI(opts.quality)) return ParseResult::Error;
                opts.qualityAuto = false;
            }
        }
        else if (a == "--preset")      { if (!nextS(opts.preset))     return ParseResult::Error; }
        else if (a == "--no-hw-decode"){ opts.hwDecode  = false; }
        else if (a == "--no-audio")    { opts.copyAudio = false; }
        else if (a == "--no-vfr-pts")  { opts.vfrPts    = false; }
        else if (a == "--cancel-event")
        {
            if (!nextS(g_cancelEventName)) return ParseResult::Error;
        }
        else if (a == "--ngx-sessions")
        {
            if (!nextI(opts.ngxSessions)) return ParseResult::Error;
            if (opts.ngxSessions < 1 || opts.ngxSessions > 4) {
                fprintf(stderr, "Invalid --ngx-sessions %d (want 1-4)\n", opts.ngxSessions);
                return ParseResult::Error;
            }
        }
        else if (a == "--verbose")     { opts.verbose   = true;  }
        else if (a == "--gpu-only")    { opts.gpuOnly   = true;  }
        else if (a == "--legacy" || a == "--no-gpu-only")
                                       { opts.gpuOnly   = false; }
        else if (a == "-i" || a == "--interactive") { wantWizard = true; }
        else if (a == "--lang")        {
            // Already applied early (before printUsage), but we still accept +
            // skip the value here so later positional-input parsing ignores it.
            std::string _v;
            if (!nextS(_v)) return ParseResult::Error;
        }
        else if (a == "-o" || a == "--output")      {
            if (!nextS(outputTarget)) return ParseResult::Error;
            outputExplicit = true;
        }
        else if (!a.empty() && a[0] == '-')
        {
            fprintf(stderr, "Unknown option: %s\n", a.c_str());
            printUsage(argv[0]);
            return ParseResult::Error;
        }
        else
        {
            positional.push_back(a);
        }
    }

    if (modeCount > 1)
    {
        fprintf(stderr, "Multiple modes specified; pick exactly one.\n");
        return ParseResult::Error;
    }

    int sizeSources = (opts.outputSizeSet ? 1 : 0)
                    + (opts.targetHeight   ? 1 : 0)
                    + (opts.scaleSet       ? 1 : 0);
    if (sizeSources > 1)
    {
        fprintf(stderr, "Conflicting VSR size options -- pick only one of "
                        "--output-size / --1080p / --4k / --8k / --scale.\n");
        return ParseResult::Error;
    }

    // TrueHDR param clamping.
    auto clamp = [](uint32_t v, uint32_t lo, uint32_t hi) { return v < lo ? lo : (v > hi ? hi : v); };
    opts.contrast   = clamp(opts.contrast,   0,   200);
    opts.saturation = clamp(opts.saturation, 0,   200);
    opts.middleGray = clamp(opts.middleGray, 10,  100);
    opts.maxLum     = clamp(opts.maxLum,     400, 2000);
    if (opts.vsrQuality < 1) opts.vsrQuality = 1;
    if (opts.vsrQuality > 4) opts.vsrQuality = 4;
    if (opts.scale < 1.0)    opts.scale = 1.0;

    return ParseResult::Ok;
}

// --------------------------------------------------------------------------
// Interactive wizard (std::cin + std::cout)
// --------------------------------------------------------------------------

void trimInPlace(std::string& s)
{
    while (!s.empty() && (s.back() == ' '  || s.back() == '\t' ||
                          s.back() == '\r' || s.back() == '\n')) s.pop_back();
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    s.erase(0, i);
    // Strip optional surrounding quotes (from drag-drop)
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"')
        s = s.substr(1, s.size() - 2);
}

std::string promptLine(const std::string& prompt, const std::string& def = "")
{
    std::cout << "  " << prompt;
    if (!def.empty()) std::cout << " [" << def << "]";
    std::cout << ": ";
    std::cout.flush();
    std::string line;
    if (!std::getline(std::cin, line)) return def;
    trimInPlace(line);
    return line.empty() ? def : line;
}

int promptMenu(const std::string& title, const std::vector<std::string>& opts, int defaultIdx = 0)
{
    std::cout << "\n" << title << "\n";
    for (size_t i = 0; i < opts.size(); ++i)
    {
        std::cout << "    " << (i + 1) << ") " << opts[i];
        if ((int)i == defaultIdx) std::cout << tr("   <-- default", "   <-- 默认");
        std::cout << "\n";
    }
    while (true)
    {
        std::string s = promptLine(tr("choose", "请选择"), std::to_string(defaultIdx + 1));
        int c = std::atoi(s.c_str());
        if (c >= 1 && c <= (int)opts.size()) return c - 1;
        std::cout << tr("    Please enter a number 1-", "    请输入 1 到 ")
                  << opts.size() << tr(".\n", " 之间的数字。\n");
    }
}

int promptInt(const std::string& prompt, int def, int lo, int hi)
{
    while (true)
    {
        std::string s = promptLine(prompt, std::to_string(def));
        int v = std::atoi(s.c_str());
        if (v >= lo && v <= hi) return v;
        std::cout << tr("    Value must be between ", "    数值必须在 ")
                  << lo << tr(" and ", " 到 ") << hi
                  << tr(".\n", " 之间。\n");
    }
}

void pressEnterToContinue()
{
    std::cout << tr("\nPress Enter to close...", "\n按 Enter 关闭...");
    std::cout.flush();
    std::string s;
    std::getline(std::cin, s);
}

// Step 0 of the interactive flow: pick the UI language. This MUST run as the
// very first interaction -- before dependency checks (ffmpeg download) and the
// wizard -- so every subsequent prompt and status message appears in the
// chosen language. The OS-detected language is pre-selected as the default.
// Skipped by the caller when the user already pinned a language via --lang or
// the SDR2HDR_LANG env var (g_langExplicit).
void selectLanguageInteractive()
{
    const int detectedDefault = (i18n::currentLang() == i18n::Lang::Zh) ? 1 : 0;
    std::cout
      << "\n[0] Language / 语言\n"
      << "    1) English"
      << (detectedDefault == 0 ? "   <-- default (OS)\n" : "\n")
      << "    2) 中文 (Chinese)"
      << (detectedDefault == 1 ? "   <-- 默认 (系统)\n" : "\n");
    while (true)
    {
        std::cout << "  choose / 请选择 ["
                  << (detectedDefault + 1) << "]: ";
        std::cout.flush();
        std::string s;
        if (!std::getline(std::cin, s)) break;
        trimInPlace(s);
        int c = s.empty() ? (detectedDefault + 1) : std::atoi(s.c_str());
        if (c == 1) { i18n::langRef() = i18n::Lang::En; break; }
        if (c == 2) { i18n::langRef() = i18n::Lang::Zh; break; }
        std::cout << "    Please enter 1 or 2. / 请输入 1 或 2。\n";
    }
}

// Wizard: ask user for everything that's not already set.
// `inputs`, `outputTarget`, `outputIsExplicit` may come pre-populated from argv;
// anything missing is asked here.
bool runWizard(std::vector<std::string>& inputs,
               std::string& outputTarget,
               bool& outputExplicit,
               Options& opts)
{
    std::cout <<
      "\n"
      "================================================================\n"
      << tr("  sdr2hdr  --  NVIDIA RTX Video SDK tool (HDR / VSR / VSR+HDR)\n",
            "  sdr2hdr  --  NVIDIA RTX Video SDK 工具 (HDR / VSR / VSR+HDR)\n")
      << "================================================================\n";

    // NOTE: UI-language selection (former "Step 0") now runs earlier, in main(),
    // BEFORE the dependency checks, so any ffmpeg-download messages already
    // appear in the user's chosen language. See selectLanguageInteractive().

    // ---- Step 1: collect input files -------------------------------------
    if (inputs.empty())
    {
        std::cout << tr(
          "\n[1/5] Input video files\n"
          "      How would you like to pick the files?\n",
          "\n[1/5] 输入视频\n"
          "      选择文件的方式:\n");
        int m = promptMenu(
            "",
            { tr("Browse... (open Windows file picker -- Ctrl/Shift for multi-select)",
                 "浏览... (打开 Windows 文件选择框 -- Ctrl/Shift 多选)"),
              tr("Type or drag-drop paths one per line",
                 "逐行输入或拖放路径") },
            0);

        if (m == 0)
        {
            std::cout << tr(
              "    Opening file picker (check taskbar if hidden)...\n",
              "    正在打开文件选择框 (若被遮挡请查看任务栏)...\n");
            if (!pickInputFilesDialog(inputs))
            {
                std::cout << tr(
                  "    No files selected. Falling back to manual entry.\n",
                  "    未选择文件，改为手动输入。\n");
                m = 1;
            }
        }

        if (m == 1)
        {
            std::cout << tr(
              "      Drag videos into this window or paste paths; empty line to finish.\n",
              "      拖放视频到此窗口或粘贴路径，空行结束。\n");
            while (true)
            {
                std::cout << tr("    file #", "    文件 #") << (inputs.size() + 1) << ": ";
                std::cout.flush();
                std::string line;
                if (!std::getline(std::cin, line)) break;
                trimInPlace(line);
                if (line.empty())
                {
                    if (inputs.empty())
                    {
                        std::cout << tr("    Need at least one file.\n",
                                        "    至少需要一个文件。\n");
                        continue;
                    }
                    break;
                }
                {
                    fs::path lp = pathFromUtf8(line);
                    std::error_code ec;
                    if (!fs::exists(lp, ec) || !fs::is_regular_file(lp, ec))
                    {
                        std::cout << tr("    File not found: ", "    找不到文件: ")
                                  << line << "\n";
                        continue;
                    }
                }
                inputs.push_back(line);
            }
        }
    }

    if (inputs.empty()) return false;

    std::cout << "\n  " << inputs.size()
              << tr(" file(s) queued:\n", " 个文件待处理:\n");
    for (size_t i = 0; i < inputs.size(); ++i)
    {
        VideoInfo info;
        if (probeVideo(inputs[i], info))
        {
            std::cout << "    [" << (i + 1) << "] " << inputs[i]
                      << "   " << info.width << "x" << info.height
                      << "  " << info.fps << " fps"
                      << "  codec=" << (info.codecName.empty() ? "?" : info.codecName)
                      << "  " << info.bitDepth << "-bit"
                      << "\n";
        }
        else
        {
            std::cout << "    [" << (i + 1) << "] " << inputs[i]
                      << tr("   (probe failed)\n", "   (探测失败)\n");
        }
    }

    // ---- Step 2: pick mode -----------------------------------------------
    if (!opts.modeSet)
    {
        int m = promptMenu(
            tr("[2/5] What do you want to do?",
               "[2/5] 要做什么?"),
            { tr("Convert SDR -> HDR (keep resolution)",
                 "SDR -> HDR 转换 (保持分辨率)"),
              tr("Upscale only  (VSR, stays SDR)",
                 "仅放大 (VSR，保持 SDR)"),
              tr("Upscale + SDR -> HDR combined",
                 "放大 + SDR -> HDR (组合)") },
            0);
        opts.mode = (m == 0) ? RtxConverter::Mode::Hdr
                  : (m == 1) ? RtxConverter::Mode::Vsr
                             : RtxConverter::Mode::VsrHdr;
        opts.modeSet = true;
    }

    const bool needVsr = (opts.mode != RtxConverter::Mode::Hdr);
    const bool needHdr = (opts.mode != RtxConverter::Mode::Vsr);

    // ---- Step 3a: VSR sizing ---------------------------------------------
    if (needVsr && !opts.outputSizeSet && !opts.targetHeight && !opts.scaleSet)
    {
        int m = promptMenu(
            tr("[3/5] Upscale target",
               "[3/5] 放大目标"),
            { tr("1080p (height=1080)",
                 "1080p (高度=1080)"),
              tr("4K    (height=2160)",
                 "4K    (高度=2160)"),
              tr("8K    (height=4320)",
                 "8K    (高度=4320)"),
              tr("2x scale",
                 "2 倍放大"),
              tr("Custom: explicit WxH",
                 "自定义: 指定宽x高") },
            1);
        switch (m) {
            case 0: opts.targetHeight = 1080; break;
            case 1: opts.targetHeight = 2160; break;
            case 2: opts.targetHeight = 4320; break;
            case 3: opts.scale = 2.0; opts.scaleSet = true; break;
            case 4: {
                std::string s = promptLine(tr("    WxH (e.g. 2880x2160)",
                                              "    宽x高 (如 2880x2160)"));
                if (!parseSize(s, opts.outW, opts.outH)) {
                    std::cout << tr("    Invalid size.\n",
                                    "    尺寸无效。\n");
                    return false;
                }
                opts.outputSizeSet = true;
                break;
            }
        }

        int q = promptInt(tr("VSR quality 1-4 (higher = better & slower)",
                             "VSR 质量 1-4 (数字越大质量越高但越慢)"), 4, 1, 4);
        opts.vsrQuality = static_cast<uint32_t>(q);
    }

    // ---- Step 3b: HDR target luminance -----------------------------------
    if (needHdr)
    {
        int m = promptMenu(
            tr("[3b/5] HDR target peak luminance (matches your display)",
               "[3b/5] HDR 峰值亮度 (匹配你的显示器)"),
            { tr("400 nits  - entry-level HDR",
                 "400 尼特  - 入门级 HDR"),
              tr("600 nits  - mid-range laptops",
                 "600 尼特  - 中端笔记本"),
              tr("1000 nits - high-end HDR monitors / Apple XDR",
                 "1000 尼特 - 高端 HDR 显示器 / Apple XDR"),
              tr("2000 nits - top-tier HDR monitors",
                 "2000 尼特 - 顶级 HDR 显示器"),
              tr("Custom (400-2000)",
                 "自定义 (400-2000)") },
            2);
        switch (m) {
            case 0: opts.maxLum = 400;  break;
            case 1: opts.maxLum = 600;  break;
            case 2: opts.maxLum = 1000; break;
            case 3: opts.maxLum = 2000; break;
            case 4: opts.maxLum = static_cast<uint32_t>(
                promptInt(tr("nits", "尼特"), 1000, 400, 2000)); break;
        }
    }

    // ---- Step 4: output codec --------------------------------------------
    {
        std::vector<std::string> codecs = {
            tr("HEVC / H.265   (default, great balance, HDR10 compatible)",
               "HEVC / H.265   (默认，均衡且兼容 HDR10)"),
            tr("AV1           (best compression, NVENC needs RTX 40+)",
               "AV1           (压缩最佳，NVENC 需要 RTX 40+)"),
            tr("H.264 / AVC   (max compatibility, SDR only)",
               "H.264 / AVC   (兼容性最佳，仅 SDR)")
        };
        int def = 0;
        if (needHdr && opts.codec == "h264") opts.codec = "hevc"; // reset illegal combo

        int m = promptMenu(tr("[4a/5] Output codec",
                              "[4a/5] 输出编码"), codecs, def);
        if      (m == 0) opts.codec = "hevc";
        else if (m == 1) opts.codec = "av1";
        else             opts.codec = "h264";

        if (needHdr && opts.codec == "h264")
        {
            std::cout << tr("  H.264 cannot carry HDR10 -- switching to HEVC.\n",
                            "  H.264 无法承载 HDR10 -- 已切换为 HEVC。\n");
            opts.codec = "hevc";
        }
    }

    // ---- Step 4b: output quality / bitrate target ------------------------
    //
    // We probe the first input's stream bitrate so each tier can display its
    // expected output Mbps next to the CQP value. "Auto" maps to the per-file
    // recommendQuality() recalculation at processing time, so batches with
    // mixed sources still get sane CQPs.
    if (opts.quality < 0 && !opts.qualityAuto)
    {
        VideoInfo refInfo;
        bool      haveRef = !inputs.empty() && probeVideo(inputs[0], refInfo);
        const bool wantsHdrOut =
            (opts.mode == RtxConverter::Mode::Hdr ||
             opts.mode == RtxConverter::Mode::VsrHdr);
        // Output size used for bitrate prediction: VSR enlarges the frame, so
        // use the eventual output size rather than the source size.
        uint32_t outW = haveRef ? refInfo.width  : 0;
        uint32_t outH = haveRef ? refInfo.height : 0;
        if (opts.outputSizeSet && opts.outW && opts.outH) { outW = opts.outW; outH = opts.outH; }
        else if (opts.targetHeight && haveRef && refInfo.height)
        {
            outH = opts.targetHeight;
            outW = static_cast<uint32_t>(
                static_cast<double>(refInfo.width) * outH / refInfo.height + 0.5);
            outW &= ~1u;
            outH &= ~1u;
        }
        else if (opts.scaleSet && haveRef)
        {
            outW = static_cast<uint32_t>(refInfo.width  * opts.scale + 0.5) & ~1u;
            outH = static_cast<uint32_t>(refInfo.height * opts.scale + 0.5) & ~1u;
        }

        auto mbpsLabel = [&](int q) -> std::string {
            if (!haveRef || outW == 0 || outH == 0 || refInfo.fps <= 0.0)
                return "q=" + std::to_string(q);
            double mbps = predictBitrateMbps(q, outW, outH, refInfo.fps, wantsHdrOut);
            char buf[64];
            std::snprintf(buf, sizeof(buf), "q=%d  ~%.0f Mbps", q, mbps);
            return std::string(buf);
        };

        const int qReco = haveRef
            ? recommendQuality(refInfo.bitRateBps, outW, outH, refInfo.fps,
                               wantsHdrOut, /*keepRatio=*/0.75)
            : 17;

        std::cout << tr("\n[4b/5] Output quality\n",
                        "\n[4b/5] 输出质量\n");
        if (haveRef && refInfo.bitRateBps > 0)
        {
            char src[96];
            std::snprintf(src, sizeof(src),
                          "  Source: %.0f Mbps  (%ux%u %s, %.0f fps)\n",
                          refInfo.bitRateBps / 1.0e6,
                          refInfo.width, refInfo.height,
                          refInfo.codecName.empty() ? "?" : refInfo.codecName.c_str(),
                          refInfo.fps);
            std::cout << tr(src, src);
        }

        std::vector<std::string> qmenu = {
            trs("Auto  - match source bitrate  (",
                "自动  - 按源码率匹配          (") + mbpsLabel(qReco) + ")",
            trs("High  - visually transparent  (",
                "高    - 视觉无损              (") + mbpsLabel(15) + ")",
            trs("Default (recommended)         (",
                "默认 (推荐)                   (") + mbpsLabel(19) + ")",
            trs("Compact - smaller file        (",
                "紧凑  - 文件更小              (") + mbpsLabel(22) + ")",
            trs("Custom q (12-28)",
                "自定义 q (12-28)"),
        };
        int qchoice = promptMenu("", qmenu, /*default=*/2);
        if      (qchoice == 0) { opts.qualityAuto = true;  opts.quality = -1; }
        else if (qchoice == 1) { opts.qualityAuto = false; opts.quality = 15; }
        else if (qchoice == 2) { opts.qualityAuto = false; opts.quality = 19; }
        else if (qchoice == 3) { opts.qualityAuto = false; opts.quality = 22; }
        else
        {
            std::string s = promptLine(tr("q value", "q 值"), "19");
            int v = std::atoi(s.c_str());
            if (v < 12) v = 12;
            if (v > 28) v = 28;
            opts.qualityAuto = false;
            opts.quality     = v;
        }
    }

    // ---- Step 5: output target -------------------------------------------
    if (!outputExplicit)
    {
        if (inputs.size() == 1)
        {
            std::string suggested = resolveOutputPath(inputs[0], "", false, false, opts);

            std::cout << tr("\n[5/5] Output file\n",
                            "\n[5/5] 输出文件\n");
            int m = promptMenu(
                "",
                { trs("Use suggested: ", "使用建议路径: ") + suggested,
                  trs("Browse... (Save As dialog)",
                      "浏览... (另存为对话框)"),
                  trs("Type a different path",
                      "手动输入其他路径") },
                0);
            if (m == 0)
            {
                outputTarget = suggested;
            }
            else if (m == 1)
            {
                if (!pickSaveFileDialog(suggested,
                        tr("Save output video as...",
                           "输出视频另存为..."), outputTarget))
                    outputTarget = suggested;
            }
            else
            {
                outputTarget = promptLine(tr("path", "路径"), suggested);
            }
            outputExplicit = true;
        }
        else
        {
            fs::path defDir = pathFromUtf8(inputs[0]).parent_path();
            if (defDir.empty()) defDir = pathFromUtf8(".");
            std::string suggested = utf8FromPath(defDir);

            std::cout << tr("\n[5/5] Output directory (batch mode)\n",
                            "\n[5/5] 输出目录 (批处理模式)\n");
            int m = promptMenu(
                "",
                { trs("Use input folder:  ", "使用输入目录:  ") + suggested,
                  trs("Browse... (pick folder)",
                      "浏览... (选择目录)"),
                  trs("Type a different path",
                      "手动输入其他路径") },
                0);
            if (m == 0)
            {
                outputTarget = suggested;
            }
            else if (m == 1)
            {
                if (!pickFolderDialog(tr("Select output directory",
                                         "选择输出目录"), outputTarget))
                    outputTarget = suggested;
            }
            else
            {
                outputTarget = promptLine(tr("directory", "目录"), suggested);
            }
            outputExplicit = true;
        }
    }

    // ---- Confirmation summary --------------------------------------------
    std::cout <<
      "\n----------------------------------------------------------------\n"
      << tr("  Ready to process:\n", "  准备开始:\n")
      << tr("    Files   : ", "    文件数  : ") << inputs.size() << "\n"
      << tr("    Mode    : ", "    模式    : ") << modeName(opts.mode) << "\n";
    if (needHdr)
        std::cout << tr("    Peak    : ", "    峰值亮度: ") << opts.maxLum
                  << tr(" nits\n", " 尼特\n");
    if (needVsr) {
        if (opts.targetHeight)
            std::cout << tr("    Target  : ", "    目标    : ")
                      << opts.targetHeight << "p\n";
        else if (opts.outputSizeSet)
            std::cout << tr("    Target  : ", "    目标    : ")
                      << opts.outW << "x" << opts.outH << "\n";
        else
            std::cout << tr("    Scale   : ", "    倍率    : ")
                      << opts.scale << "x\n";
    }
    std::cout <<
      tr("    Codec   : ", "    编码    : ") << opts.codec << " (" << opts.backend << ")\n";
    if (opts.qualityAuto)
        std::cout << tr("    Quality : auto (match source per file)\n",
                        "    质量    : auto (按源码率自适应)\n");
    else if (opts.quality >= 0)
        std::cout << tr("    Quality : q=", "    质量    : q=") << opts.quality << "\n";
    std::cout <<
      tr("    Output  : ", "    输出    : ") << outputTarget
      << (inputs.size() > 1 ? tr("  [dir]", "  [目录]") : "") << "\n"
      "----------------------------------------------------------------\n";

    std::string ok = promptLine(tr("Start? (Y/n)", "开始? (Y/n)"), "Y");
    if (!ok.empty() && (ok[0] == 'n' || ok[0] == 'N'))
        return false;
    return true;
}

// --------------------------------------------------------------------------
// processOne: CLI wrapper around sdr2hdr::processFile (keeps console headers).
// --------------------------------------------------------------------------

int processOne(const std::string& input, const std::string& outputIn, const Options& opts)
{
    std::string output = outputIn;
    {
        std::string inExt  = extOf(input);
        std::string outExt = extOf(output);
        if (!inExt.empty() && inExt != outExt)
        {
            output = replaceExt(output, inExt);
            printf("Note: output container normalised to match input (.%s -> .%s)\n",
                   outExt.c_str(), inExt.c_str());
        }
    }
    {
        std::string curExt = extOf(output);
        if (curExt == "webm" && opts.codec != "av1")
        {
            output = replaceExt(output, "mp4");
            printf(tr("Note: .webm cannot carry %s; switching output container to .mp4\n"
                      "      (use --codec av1 to keep .webm)\n",
                      "提示: .webm 不支持 %s 编码，已将输出容器切换为 .mp4\n"
                      "      (如需保留 .webm 请使用 --codec av1)\n"),
                   opts.codec.c_str());
        }
    }

    VideoInfo info;
    if (!probeVideo(input, info))
    {
        fprintf(stderr, "Failed to probe input video: %s\n", input.c_str());
        return 2;
    }
    if (info.isVfr)
    {
        fprintf(stderr,
                "Note: variable frame rate detected (nominal %.2f fps, "
                "average %.2f fps). Using average for A/V sync.\n",
                info.rFps, info.avgFps);
    }

    uint32_t outW = info.width, outH = info.height;
    if (opts.mode == RtxConverter::Mode::Vsr || opts.mode == RtxConverter::Mode::VsrHdr)
    {
        if (opts.outputSizeSet) { outW = opts.outW; outH = opts.outH; }
        else if (opts.targetHeight)
        {
            outH = opts.targetHeight & ~1u;
            outW = static_cast<uint32_t>(info.width * static_cast<double>(opts.targetHeight)
                                                   / static_cast<double>(info.height) + 0.5) & ~1u;
        }
        else if (opts.scaleSet)
        {
            outW = static_cast<uint32_t>(info.width  * opts.scale + 0.5) & ~1u;
            outH = static_cast<uint32_t>(info.height * opts.scale + 0.5) & ~1u;
        }
        else
        {
            outH = 2160;
            outW = static_cast<uint32_t>(info.width * 2160.0 / info.height + 0.5) & ~1u;
        }
    }

    printf("Input:  %s  %ux%u  %.3f fps  ~%lld frames  codec=%s  %d-bit%s\n",
           input.c_str(), info.width, info.height, info.fps,
           static_cast<long long>(info.frameCount),
           info.codecName.empty() ? "?" : info.codecName.c_str(),
           info.bitDepth,
           info.pixFmt.empty() ? "" : (" (" + info.pixFmt + ")").c_str());
    printf("Mode:   %s    Output: %s  %ux%u%s\n",
           modeName(opts.mode), output.c_str(), outW, outH,
           (opts.mode == RtxConverter::Mode::Vsr) ? "  (SDR)" : "  (HDR10)");

    auto r = sdr2hdr::processFile(input, output, opts, {}, &g_cliCancel);
    if (!r.ok)
    {
        fprintf(stderr, "%s\n",
                r.errorDetail.empty() ? "Processing failed." : r.errorDetail.c_str());
        if (r.exitCode == 6)
            fprintf(stderr, "Tip: retry with --legacy to bypass the in-process pipeline.\n");
        return r.exitCode ? r.exitCode : 1;
    }

    if (opts.gpuOnly && opts.backend == "nvenc")
    {
        double fps = r.seconds > 0 ? r.framesProcessed / r.seconds : 0.0;
        printf("Done. %llu frames in %.1f s (%.1f fps)  [GPU-only]\n",
               static_cast<unsigned long long>(r.framesProcessed), r.seconds, fps);
    }
    return 0;
}

} // namespace

// --------------------------------------------------------------------------
// cliMain: the full command-line / interactive-wizard entry point.
//
// Exposed via engine.h so it can be driven from two places:
//   1. The standalone console build (src/cli_entry.cpp -> main -> cliMain).
//   2. The merged GUI binary, whose wWinMain forwards here when the app is
//      launched with command-line arguments instead of double-clicked.
// --------------------------------------------------------------------------

int sdr2hdr::cliMain(int argc, char** argv)
{
#ifdef _WIN32
    // 1) Make the console use UTF-8 so printf/cout of non-ASCII (e.g. Chinese
    //    path) bytes renders correctly instead of mojibake. We keep all our
    //    std::string paths in UTF-8 internally.
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    // 2) DPI awareness: prefer per-monitor v2 for sharp dialogs on scaled
    //    (HiDPI / 4K / 150% / 200%) displays. Fall back to system DPI aware
    //    if the newer API isn't available on this Windows build.
    {
        HMODULE u32 = GetModuleHandleW(L"user32.dll");
        using SetCtxFn = BOOL (WINAPI*)(void*);
        auto setCtx = u32 ? reinterpret_cast<SetCtxFn>(
                                GetProcAddress(u32, "SetProcessDpiAwarenessContext"))
                          : nullptr;
        // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 == -4
        if (!setCtx || !setCtx(reinterpret_cast<void*>(-4)))
            SetProcessDPIAware();
    }

    // 3) Replace ANSI argv with UTF-8 decoded from the Unicode command line
    //    so Chinese (or any non-ANSI) CLI arguments also round-trip cleanly.
    std::vector<std::string> argvUtf8;
    std::vector<char*>       argvPtrs;
    {
        int wargc = 0;
        LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
        if (wargv && wargc > 0)
        {
            argvUtf8.reserve(static_cast<size_t>(wargc));
            for (int i = 0; i < wargc; ++i)
                argvUtf8.push_back(wideToUtf8(wargv[i]));
            argvPtrs.reserve(argvUtf8.size() + 1);
            for (auto& s : argvUtf8) argvPtrs.push_back(s.data());
            argvPtrs.push_back(nullptr);
            argc = wargc;
            argv = argvPtrs.data();
            LocalFree(wargv);
        }
    }

    // 4) Needed for SHBrowseForFolder's modern dialog style (BIF_NEWDIALOGSTYLE).
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
#endif

    // 5) Choose UI language BEFORE any user-facing output (including --help
    //    and parse-error messages). We do a quick scan of argv for
    //    `--lang <tag>` so the flag works independently of parseArgs order.
    //    Precedence (low -> high) inside initLang():
    //      English default -> OS UI language -> SDR2HDR_LANG env -> --lang.
    {
        const char* cliLang = nullptr;
        for (int i = 1; i + 1 < argc; ++i)
        {
            if (std::strcmp(argv[i], "--lang") == 0 && argv[i + 1])
            {
                cliLang = argv[i + 1];
                break;
            }
        }
        i18n::initLang(cliLang);
        // Treat any explicit signal (CLI flag or env var, except the literal
        // "auto") as "user already chose"; the wizard will then skip its own
        // language picker. Bare auto-detection from the OS UI language is
        // NOT considered explicit.
        const char* envLang = std::getenv("SDR2HDR_LANG");
        auto isExplicit = [](const char* s) {
            return s && *s && std::strcmp(s, "auto") != 0;
        };
        g_langExplicit = isExplicit(cliLang) || isExplicit(envLang);
    }

    std::vector<std::string> positional;
    std::string outputTarget;
    bool outputExplicit = false;
    bool wantWizard     = false;
    Options opts;

    // Top-level try/catch so unexpected exceptions (filesystem errors,
    // bad_alloc, ...) surface to the user instead of silently closing the
    // console window. Without this, an `std::filesystem::filesystem_error`
    // thrown during resolveOutputPath (path encoding, network timeout, ...)
    // just terminates the process between wizard steps with no message.
    try {

    ParseResult pr = parseArgs(argc, argv, positional, outputTarget, outputExplicit, wantWizard, opts);
    if (pr == ParseResult::Help)  return 0;
    if (pr == ParseResult::Error) return 1;

    startCancelEventWatcher();

    // Detect interactive launch (so we pause at end):
    //   - no args at all (double-click),
    //   - explicit --interactive,
    //   - or args are ONLY inputs (drag-drop scenario: no mode picked, no -o).
    const bool dragDropOnly = (argc > 1) && !opts.modeSet && !outputExplicit &&
                              !positional.empty() &&
                              (int)positional.size() == (argc - 1);
    if (argc == 1 || wantWizard || dragDropOnly)
    {
        g_interactiveLaunch = true;
    }

    // Language selection ALWAYS comes first: for an interactive launch (and
    // when the user hasn't pinned a language via --lang / SDR2HDR_LANG), ask
    // for the UI language before anything else, so the dependency-check and
    // ffmpeg-download messages below already render in the chosen language.
    if (g_interactiveLaunch && !g_langExplicit)
        selectLanguageInteractive();

    // THEN make sure the external runtime dependencies are in place before we
    // touch ffprobe (the wizard probes inputs) or the RTX SDK. ffmpeg/ffprobe
    // can be fetched on demand; the NVIDIA NGX DLLs can only be reported missing.
    if (!sdr2hdr::deps::ensureFfmpeg(g_interactiveLaunch))
    {
        if (g_interactiveLaunch) pressEnterToContinue();
        return 1;
    }
    if (!sdr2hdr::deps::checkNgxDlls())
    {
        if (g_interactiveLaunch) pressEnterToContinue();
        return 1;
    }

    std::vector<std::string> inputs;

    // Legacy single-file pair: exactly 2 positionals, second is a non-existent path.
    if (!g_interactiveLaunch && positional.size() == 2 && !outputExplicit)
    {
        std::error_code _ec;
        if (!fs::exists(pathFromUtf8(positional[1]), _ec))
        {
            inputs.push_back(positional[0]);
            outputTarget   = positional[1];
            outputExplicit = true;
        }
        else
        {
            inputs = positional;
        }
    }
    else
    {
        inputs = positional;
    }

    // Wizard collects / confirms anything missing.
    if (g_interactiveLaunch)
    {
        if (!runWizard(inputs, outputTarget, outputExplicit, opts))
        {
            std::cout << tr("Cancelled.\n", "已取消。\n");
            if (g_interactiveLaunch) pressEnterToContinue();
            return 1;
        }
    }

    if (inputs.empty())
    {
        fprintf(stderr, "No input files. Run with --help for usage.\n");
        if (g_interactiveLaunch) pressEnterToContinue();
        return 1;
    }
    if (!opts.modeSet)
    {
        fprintf(stderr, "No mode specified. Pass --hdr / --vsr / --vsr-hdr (or run with no args for wizard).\n");
        if (g_interactiveLaunch) pressEnterToContinue();
        return 1;
    }
    // Resolve the global quality default. Skipped when --quality auto is in
    // play, because processOne() recomputes a per-file CQP from the probed
    // source bitrate; we want opts.quality to stay at -1 so the per-file
    // override path runs instead of locking in a single CLI default.
    if (opts.quality < 0 && !opts.qualityAuto)
        opts.quality = (opts.backend == "nvenc") ? 19 : 18;

    // H.264 + HDR is not a real HDR combo.
    const bool wantsHdr = (opts.mode == RtxConverter::Mode::Hdr ||
                           opts.mode == RtxConverter::Mode::VsrHdr);
    if (wantsHdr && opts.codec == "h264")
    {
        fprintf(stderr, "Error: --codec h264 is SDR-only.\n"
                        "       Use --codec hevc (default) or --codec av1 for HDR.\n");
        if (g_interactiveLaunch) pressEnterToContinue();
        return 1;
    }

    // Decide if outputTarget is a directory (for multi-input) or file (single).
    bool outIsDir = false;
    if (outputExplicit)
    {
        if (inputs.size() > 1)
            outIsDir = true;                        // forced dir for batch
        else
        {
            std::error_code _ec;
            outIsDir = fs::is_directory(pathFromUtf8(outputTarget), _ec);
        }
    }

    int failed = 0;
    auto batchStart = std::chrono::steady_clock::now();

    for (size_t i = 0; i < inputs.size(); ++i)
    {
        if (inputs.size() > 1)
        {
            printf("\n================ [%zu / %zu] %s ================\n",
                   i + 1, inputs.size(), inputs[i].c_str());
        }

        std::string outFile = resolveOutputPath(inputs[i], outputTarget, outIsDir,
                                                outputExplicit, opts);

        // Avoid overwriting the input with the output (in case suffix collides
        // or user pointed output dir == input dir with no mode-specific suffix).
        std::error_code _ecAbs;
        fs::path outAbs = fs::absolute(pathFromUtf8(outFile),   _ecAbs);
        fs::path inAbs  = fs::absolute(pathFromUtf8(inputs[i]), _ecAbs);
        if (outAbs == inAbs)
        {
            fprintf(stderr, "Refusing to overwrite input file: %s\n", inputs[i].c_str());
            ++failed;
            continue;
        }

        int rc = processOne(inputs[i], outFile, opts);
        if (rc != 0) ++failed;
    }

    if (inputs.size() > 1)
    {
        auto batchEnd = std::chrono::steady_clock::now();
        double secs = std::chrono::duration<double>(batchEnd - batchStart).count();
        printf(tr("\n================ Batch complete ================\n",
                  "\n================ 批处理完成 ================\n"));
        printf(tr("  %zu succeeded, %d failed, %.1f s total\n",
                  "  %zu 个成功, %d 个失败, 共 %.1f 秒\n"),
               inputs.size() - failed, failed, secs);
    }

    if (g_interactiveLaunch) pressEnterToContinue();
    return failed > 0 ? 1 : 0;

    } catch (const std::exception& e) {
        fprintf(stderr, tr("\nUnhandled error: %s\n",
                           "\n未处理的错误: %s\n"), e.what());
        if (g_interactiveLaunch) pressEnterToContinue();
        return 2;
    } catch (...) {
        fprintf(stderr, tr("\nUnhandled error (unknown exception).\n",
                           "\n未处理的错误 (未知异常)。\n"));
        if (g_interactiveLaunch) pressEnterToContinue();
        return 2;
    }
}
