#include "deps.h"
#include "win32_utils.h"
#include "i18n.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>
#include <fstream>
#include <iterator>
#include <filesystem>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <urlmon.h>
#include <bcrypt.h>
#pragma comment(lib, "urlmon.lib")
#pragma comment(lib, "bcrypt.lib")

namespace fs = std::filesystem;

using sdr2hdr::win32::exeDir;
using sdr2hdr::win32::fileExists;
using sdr2hdr::win32::toWide;
using i18n::tr;

namespace sdr2hdr { namespace deps {

namespace {

// gyan.dev's "release-essentials" build is a stable URL and includes libx264 /
// libx265 / libsvtav1 (everything sdr2hdr's software-encode paths need).
const wchar_t* kFfmpegUrl =
    L"https://www.gyan.dev/ffmpeg/builds/ffmpeg-release-essentials.zip";

// True if `exeName` resolves via the standard search path (PATH + current dir).
bool onPath(const wchar_t* exeName)
{
    wchar_t buf[MAX_PATH];
    DWORD n = SearchPathW(nullptr, exeName, nullptr, MAX_PATH, buf, nullptr);
    return n > 0 && n < MAX_PATH;
}

// True if `exeName` sits right next to our own executable.
bool localPresent(const char* exeName)
{
    return fileExists(exeDir() + "\\" + exeName);
}

// Spawn a command line, wait for it, return its exit code. No console window.
bool runWait(const std::wstring& cmdline, DWORD& exitCode)
{
    std::vector<wchar_t> buf(cmdline.begin(), cmdline.end());
    buf.push_back(L'\0');

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        return false;

    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return true;
}

// Compute the lowercase-hex SHA-256 of a file using Windows CNG (BCrypt).
bool sha256File(const std::wstring& path, std::string& outHex)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;

    BCRYPT_ALG_HANDLE  alg  = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
        return false;

    DWORD hashLen = 0, cb = 0;
    BCryptGetProperty(alg, BCRYPT_HASH_LENGTH,
                      reinterpret_cast<PUCHAR>(&hashLen), sizeof(hashLen), &cb, 0);

    if (BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) < 0)
    {
        BCryptCloseAlgorithmProvider(alg, 0);
        return false;
    }

    bool ok = true;
    std::vector<char> buf(1 << 16);
    while (f)
    {
        f.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        std::streamsize got = f.gcount();
        if (got > 0 &&
            BCryptHashData(hash, reinterpret_cast<PUCHAR>(buf.data()),
                           static_cast<ULONG>(got), 0) < 0)
        {
            ok = false;
            break;
        }
    }

    std::vector<unsigned char> digest(hashLen ? hashLen : 32);
    if (ok && BCryptFinishHash(hash, digest.data(),
                               static_cast<ULONG>(digest.size()), 0) < 0)
        ok = false;

    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(alg, 0);
    if (!ok) return false;

    static const char* hx = "0123456789abcdef";
    outHex.clear();
    outHex.reserve(digest.size() * 2);
    for (unsigned char b : digest)
    {
        outHex.push_back(hx[b >> 4]);
        outHex.push_back(hx[b & 0x0F]);
    }
    return true;
}

// Parse the first 64 hex chars out of a ".sha256" sidecar file (the gyan.dev
// files are "<hash> *<filename>"; some tools emit just the bare hash).
bool readSha256File(const std::wstring& path, std::string& outHex)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::string content((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());

    std::string hex;
    for (char c : content)
    {
        char lc = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
        if ((lc >= '0' && lc <= '9') || (lc >= 'a' && lc <= 'f'))
        {
            hex.push_back(lc);
            if (hex.size() == 64) break;
        }
        else if (!hex.empty())
        {
            break;   // hit a separator after the hash started
        }
    }
    if (hex.size() != 64) return false;
    outHex = hex;
    return true;
}

} // namespace

bool ensureFfmpeg(bool interactive)
{
    (void)interactive;

    const bool haveFf = localPresent("ffmpeg.exe")  || onPath(L"ffmpeg.exe");
    const bool haveFp = localPresent("ffprobe.exe") || onPath(L"ffprobe.exe");
    if (haveFf && haveFp) return true;

    if (std::getenv("SDR2HDR_NO_AUTODOWNLOAD"))
    {
        fprintf(stderr, tr(
            "ffmpeg/ffprobe not found and auto-download is disabled "
            "(SDR2HDR_NO_AUTODOWNLOAD is set).\n"
            "Install ffmpeg and put ffmpeg.exe/ffprobe.exe next to the program "
            "or on PATH.\n",
            "未找到 ffmpeg/ffprobe，且已禁用自动下载 (设置了 SDR2HDR_NO_AUTODOWNLOAD)。\n"
            "请安装 ffmpeg，并把 ffmpeg.exe/ffprobe.exe 放到程序同目录或加入 PATH。\n"));
        return false;
    }

    printf(tr(
        "\nffmpeg / ffprobe not found -- they are required for decoding & encoding.\n"
        "Downloading a prebuilt static ffmpeg (~90 MB) into:\n  %s\n"
        "This happens only once. Source: gyan.dev (GPL build).\n\n",
        "\n未找到 ffmpeg / ffprobe -- 它们是解码与编码所必需的。\n"
        "正在下载预编译的静态 ffmpeg (~90 MB) 到:\n  %s\n"
        "仅需下载一次。来源: gyan.dev (GPL 构建)。\n\n"),
        exeDir().c_str());

    wchar_t tmpDir[MAX_PATH];
    if (GetTempPathW(MAX_PATH, tmpDir) == 0)
        return false;

    const std::wstring zip     = std::wstring(tmpDir) + L"sdr2hdr_ffmpeg.zip";
    const std::wstring extract = std::wstring(tmpDir) + L"sdr2hdr_ffmpeg_extract";

    printf(tr("  [1/4] Downloading...\n", "  [1/4] 正在下载...\n"));
    fflush(stdout);
    HRESULT hr = URLDownloadToFileW(nullptr, kFfmpegUrl, zip.c_str(), 0, nullptr);
    if (FAILED(hr))
    {
        fprintf(stderr, tr(
            "  Download failed (hr=0x%08lX). Check your internet connection, or\n"
            "  install ffmpeg manually and put ffmpeg.exe/ffprobe.exe next to the program.\n",
            "  下载失败 (hr=0x%08lX)。请检查网络连接，或手动安装 ffmpeg\n"
            "  并把 ffmpeg.exe/ffprobe.exe 放到程序同目录。\n"),
            static_cast<unsigned long>(hr));
        return false;
    }

    // Integrity check: fetch the publisher's companion .sha256 and compare.
    // A mismatch aborts (corrupt / tampered download). If the checksum file
    // itself can't be fetched we degrade gracefully and skip the check rather
    // than blocking the user on a transient network hiccup.
    {
        printf(tr("  [2/4] Verifying SHA-256...\n", "  [2/4] 正在校验 SHA-256...\n"));
        fflush(stdout);
        const std::wstring shaUrl  = std::wstring(kFfmpegUrl) + L".sha256";
        const std::wstring shaFile = std::wstring(tmpDir) + L"sdr2hdr_ffmpeg.zip.sha256";

        std::string expected, actual;
        HRESULT shr = URLDownloadToFileW(nullptr, shaUrl.c_str(), shaFile.c_str(), 0, nullptr);
        std::error_code shec;
        if (SUCCEEDED(shr) && readSha256File(shaFile, expected))
        {
            if (!sha256File(zip, actual) ||
                _stricmp(actual.c_str(), expected.c_str()) != 0)
            {
                fprintf(stderr, tr(
                    "  SHA-256 mismatch -- the download may be corrupted or tampered with.\n"
                    "    expected: %s\n    actual:   %s\n  Aborting; nothing was installed.\n",
                    "  SHA-256 不匹配 -- 下载内容可能损坏或被篡改。\n"
                    "    期望: %s\n    实际: %s\n  已中止，未安装任何文件。\n"),
                    expected.c_str(),
                    actual.empty() ? "(hashing failed)" : actual.c_str());
                fs::remove(fs::path(zip), shec);
                fs::remove(fs::path(shaFile), shec);
                return false;
            }
            printf(tr("        checksum OK.\n", "        校验通过。\n"));
        }
        else
        {
            fprintf(stderr, tr(
                "        (Could not fetch the published checksum; skipping integrity check.)\n",
                "        (无法获取官方校验值；跳过完整性校验。)\n"));
        }
        fs::remove(fs::path(shaFile), shec);
    }

    printf(tr("  [3/4] Extracting...\n", "  [3/4] 正在解压...\n"));
    fflush(stdout);
    std::error_code ec;
    fs::path extractPath(extract);
    fs::remove_all(extractPath, ec);
    fs::create_directories(extractPath, ec);

    // tar.exe (bsdtar) ships with Windows 10 1803+ and unpacks .zip happily.
    DWORD exitCode = 0;
    std::wstring cmd = L"tar.exe -xf \"" + zip + L"\" -C \"" + extract + L"\"";
    if (!runWait(cmd, exitCode) || exitCode != 0)
    {
        fprintf(stderr, tr(
            "  Extraction failed (tar exit=%lu). Unzip it manually and copy\n"
            "  bin\\ffmpeg.exe and bin\\ffprobe.exe next to the program:\n    %ls\n",
            "  解压失败 (tar 退出码=%lu)。请手动解压并把其中的\n"
            "  bin\\ffmpeg.exe 和 bin\\ffprobe.exe 复制到程序同目录:\n    %ls\n"),
            static_cast<unsigned long>(exitCode), zip.c_str());
        fs::remove(fs::path(zip), ec);
        return false;
    }

    printf(tr("  [4/4] Installing...\n", "  [4/4] 正在安装...\n"));
    fflush(stdout);

    // The zip nests the binaries under ffmpeg-<ver>-essentials_build/bin/, so
    // walk the tree and grab the two executables wherever they landed.
    const std::wstring dstDir = toWide(exeDir());
    int copied = 0;
    for (auto it = fs::recursive_directory_iterator(extractPath, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec))
    {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;

        const std::wstring fn = it->path().filename().wstring();
        if (_wcsicmp(fn.c_str(), L"ffmpeg.exe") == 0 ||
            _wcsicmp(fn.c_str(), L"ffprobe.exe") == 0)
        {
            fs::path dst = fs::path(dstDir) / fn;
            fs::copy_file(it->path(), dst, fs::copy_options::overwrite_existing, ec);
            if (!ec) ++copied;
        }
    }

    fs::remove_all(extractPath, ec);
    fs::remove(fs::path(zip), ec);

    if (copied < 2)
    {
        fprintf(stderr, tr(
            "  Could not find ffmpeg.exe/ffprobe.exe inside the downloaded archive.\n"
            "  Please install ffmpeg manually.\n",
            "  在下载的压缩包中未找到 ffmpeg.exe/ffprobe.exe。\n"
            "  请手动安装 ffmpeg。\n"));
        return false;
    }

    printf(tr("  ffmpeg is ready.\n\n", "  ffmpeg 已就绪。\n\n"));
    return true;
}

bool checkNgxDlls()
{
    // Found if next to the exe OR resolvable via the standard DLL search path
    // (PATH + current dir) — same "local OR global" rule used for ffmpeg.
    const bool haveHdr = localPresent("nvngx_truehdr.dll") || onPath(L"nvngx_truehdr.dll");
    const bool haveVsr = localPresent("nvngx_vsr.dll")     || onPath(L"nvngx_vsr.dll");
    if (haveHdr && haveVsr) return true;

    fprintf(stderr, tr(
        "\n[!] NVIDIA RTX Video model DLL(s) are missing next to the program:\n",
        "\n[!] 程序同目录缺少 NVIDIA RTX Video 模型 DLL:\n"));
    if (!haveHdr)
        fprintf(stderr, tr(
            "      - nvngx_truehdr.dll  (needed for --hdr / --vsr-hdr)\n",
            "      - nvngx_truehdr.dll  (--hdr / --vsr-hdr 需要)\n"));
    if (!haveVsr)
        fprintf(stderr, tr(
            "      - nvngx_vsr.dll      (needed for --vsr / --vsr-hdr)\n",
            "      - nvngx_vsr.dll      (--vsr / --vsr-hdr 需要)\n"));
    fprintf(stderr, tr(
        "    These cannot be downloaded automatically -- they ship with NVIDIA's\n"
        "    RTX Video SDK, which is gated behind NVIDIA's developer login + EULA.\n"
        "    Get the SDK here:\n"
        "      https://developer.nvidia.com/rtx-video-sdk\n"
        "    then copy bin\\Windows\\x64\\rel\\nvngx_*.dll next to this program.\n\n",
        "    它们无法自动下载 -- 随 NVIDIA RTX Video SDK 一起提供，\n"
        "    而该 SDK 需要 NVIDIA 开发者登录并同意 EULA。\n"
        "    SDK 下载地址:\n"
        "      https://developer.nvidia.com/rtx-video-sdk\n"
        "    然后把 bin\\Windows\\x64\\rel\\nvngx_*.dll 复制到本程序同目录。\n\n"));
    return false;
}

}} // namespace sdr2hdr::deps
