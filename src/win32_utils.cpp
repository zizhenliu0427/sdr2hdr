#include "win32_utils.h"

#include <cstdio>
#include <cstring>
#include <map>
#include <vector>

namespace sdr2hdr { namespace win32 {

// ---------------------------------------------------------------------------
// UTF-8 <-> UTF-16
// ---------------------------------------------------------------------------

std::wstring toWide(const std::string& utf8)
{
    if (utf8.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(static_cast<size_t>(n - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, w.data(), n);
    return w;
}

std::string fromWide(const wchar_t* w)
{
    if (!w || !*w) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string s(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

// ---------------------------------------------------------------------------
// Paths / tool discovery
// ---------------------------------------------------------------------------

std::string exeDir()
{
    static std::string cached;
    if (!cached.empty()) return cached;
    wchar_t buf[MAX_PATH] = {};
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0) return {};
    std::wstring p(buf, n);
    auto slash = p.find_last_of(L"\\/");
    if (slash != std::wstring::npos) p.resize(slash);
    cached = fromWide(p.c_str());
    return cached;
}

bool fileExists(const std::string& p)
{
    DWORD a = GetFileAttributesW(toWide(p).c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

const std::string& toolPath(const char* exeName)
{
    // Cache per-executable so we only hit the disk once.
    static std::string ffmpegPath;
    static std::string ffprobePath;
    std::string& cache = (std::strcmp(exeName, "ffmpeg.exe") == 0) ? ffmpegPath : ffprobePath;
    if (!cache.empty()) return cache;

    std::string local = exeDir() + "\\" + exeName;
    if (fileExists(local))
        cache = "\"" + local + "\"";
    else
        cache = exeName;
    return cache;
}

std::string quote(const std::string& s)
{
    std::string r = "\"";
    r.reserve(s.size() + 2);
    for (char c : s) r.push_back(c);
    r.push_back('"');
    return r;
}

// ---------------------------------------------------------------------------
// Pipes, process launch, blocking IO
// ---------------------------------------------------------------------------

bool makePipe(PipePair& pipe, bool childReads)
{
    SECURITY_ATTRIBUTES sa{};
    sa.nLength        = sizeof(sa);
    sa.bInheritHandle = TRUE;
    if (!CreatePipe(&pipe.readEnd, &pipe.writeEnd, &sa, 0))
    {
        fprintf(stderr, "CreatePipe failed: %lu\n", GetLastError());
        return false;
    }
    HANDLE ours = childReads ? pipe.writeEnd : pipe.readEnd;
    if (!SetHandleInformation(ours, HANDLE_FLAG_INHERIT, 0))
    {
        fprintf(stderr, "SetHandleInformation failed: %lu\n", GetLastError());
        return false;
    }
    return true;
}

bool launch(const std::string& cmdline,
            HANDLE hStdin, HANDLE hStdout, HANDLE hStderr,
            PROCESS_INFORMATION& pi)
{
    STARTUPINFOW si{};
    si.cb         = sizeof(si);
    si.dwFlags    = STARTF_USESTDHANDLES;
    si.hStdInput  = hStdin  ? hStdin  : GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = hStdout ? hStdout : GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError  = hStderr ? hStderr : GetStdHandle(STD_ERROR_HANDLE);

    std::wstring wcmd = toWide(cmdline);
    std::vector<wchar_t> wbuf(wcmd.begin(), wcmd.end());
    wbuf.push_back(L'\0');

    BOOL ok = CreateProcessW(
        nullptr, wbuf.data(),
        nullptr, nullptr,
        TRUE, 0, nullptr, nullptr,
        &si, &pi);
    if (!ok) {
        fprintf(stderr, "CreateProcess failed (%lu): %s\n", GetLastError(), cmdline.c_str());
        return false;
    }
    return true;
}

bool readAll(HANDLE h, void* dst_, size_t n)
{
    uint8_t* dst = static_cast<uint8_t*>(dst_);
    while (n > 0)
    {
        DWORD got = 0;
        if (!ReadFile(h, dst, static_cast<DWORD>(n), &got, nullptr))
        {
            DWORD e = GetLastError();
            if (e == ERROR_BROKEN_PIPE || e == ERROR_HANDLE_EOF) return false;
            fprintf(stderr, "ReadFile failed: %lu\n", e);
            return false;
        }
        if (got == 0) return false;
        dst += got;
        n   -= got;
    }
    return true;
}

bool writeAll(HANDLE h, const void* src_, size_t n)
{
    const uint8_t* src = static_cast<const uint8_t*>(src_);
    while (n > 0)
    {
        DWORD put = 0;
        if (!WriteFile(h, src, static_cast<DWORD>(n), &put, nullptr))
        {
            fprintf(stderr, "WriteFile failed: %lu\n", GetLastError());
            return false;
        }
        if (put == 0) return false;
        src += put;
        n   -= put;
    }
    return true;
}

size_t readSome(HANDLE h, void* dst, size_t capacity)
{
    if (!h || !capacity) return 0;
    DWORD got = 0;
    BOOL ok = ReadFile(h, dst, static_cast<DWORD>(capacity), &got, nullptr);
    if (!ok)
    {
        DWORD e = GetLastError();
        if (e == ERROR_BROKEN_PIPE || e == ERROR_HANDLE_EOF) return 0;
        fprintf(stderr, "ReadFile(pipe) failed: %lu\n", e);
        return 0;
    }
    return static_cast<size_t>(got);
}

// ---------------------------------------------------------------------------
// Capture helpers
// ---------------------------------------------------------------------------

bool runAndCapture(const std::string& cmdline, std::string& out, DWORD& exitCode,
                   bool mergeStderr)
{
    PipePair p;
    if (!makePipe(p, /*childReads=*/false)) return false;

    PROCESS_INFORMATION pi{};
    HANDLE stderrHandle = mergeStderr ? p.writeEnd : nullptr;
    if (!launch(cmdline, nullptr, p.writeEnd, stderrHandle, pi))
    {
        CloseHandle(p.readEnd); CloseHandle(p.writeEnd);
        return false;
    }
    CloseHandle(p.writeEnd);

    char buf[4096];
    for (;;)
    {
        DWORD got = 0;
        BOOL ok = ReadFile(p.readEnd, buf, sizeof(buf), &got, nullptr);
        if (!ok || got == 0) break;
        out.append(buf, got);
    }
    CloseHandle(p.readEnd);

    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return true;
}

HANDLE openStderrCapture(std::string& outLogPath)
{
    outLogPath.clear();

    wchar_t tmpDir[MAX_PATH];
    DWORD n = GetTempPathW(MAX_PATH, tmpDir);
    if (n == 0 || n >= MAX_PATH) return INVALID_HANDLE_VALUE;

    wchar_t tmpName[MAX_PATH];
    if (GetTempFileNameW(tmpDir, L"s2h", 0, tmpName) == 0)
        return INVALID_HANDLE_VALUE;

    SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
    HANDLE h = CreateFileW(tmpName,
                           GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           &sa,
                           CREATE_ALWAYS,
                           FILE_ATTRIBUTE_TEMPORARY,
                           nullptr);
    if (h == INVALID_HANDLE_VALUE) return INVALID_HANDLE_VALUE;

    outLogPath = fromWide(tmpName);
    return h;
}

void dumpStderrLog(const std::string& logPath, const char* label)
{
    if (logPath.empty()) return;

    std::wstring wpath = toWide(logPath);
    HANDLE h = CreateFileW(wpath.c_str(),
                           GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;

    LARGE_INTEGER sz{};
    GetFileSizeEx(h, &sz);
    constexpr DWORD kMaxTail = 4096;
    DWORD toRead = (sz.QuadPart > kMaxTail) ? kMaxTail : static_cast<DWORD>(sz.QuadPart);

    if (sz.QuadPart > kMaxTail)
    {
        LARGE_INTEGER off{};
        off.QuadPart = sz.QuadPart - kMaxTail;
        SetFilePointerEx(h, off, nullptr, FILE_BEGIN);
    }

    std::vector<char> buf(toRead + 1, '\0');
    DWORD got = 0;
    ReadFile(h, buf.data(), toRead, &got, nullptr);
    CloseHandle(h);

    if (got == 0) return;

    fprintf(stderr, "\n--- %s stderr tail (%lu bytes) -------------------------\n",
            label, got);
    fwrite(buf.data(), 1, got, stderr);
    if (got > 0 && buf[got - 1] != '\n') fputc('\n', stderr);
    fprintf(stderr, "-----------------------------------------------------------\n");
}

bool encoderHasOption(const std::string& encoderName, const std::string& optName)
{
    static std::map<std::string, std::string> helpCache;
    auto itHelp = helpCache.find(encoderName);
    if (itHelp == helpCache.end())
    {
        std::string cmd = toolPath("ffmpeg.exe") +
                          " -hide_banner -h encoder=" + encoderName;
        std::string out;
        DWORD exit = 0;
        runAndCapture(cmd, out, exit, /*mergeStderr=*/true);
        helpCache[encoderName] = out;
        itHelp = helpCache.find(encoderName);
    }
    const std::string needle = "-" + optName;
    const std::string& help = itHelp->second;
    size_t p = 0;
    while ((p = help.find(needle, p)) != std::string::npos)
    {
        bool leftOk  = (p == 0 || help[p-1] == ' ' || help[p-1] == '\n' || help[p-1] == '\t');
        size_t e = p + needle.size();
        bool rightOk = (e == help.size() || help[e] == ' ' || help[e] == '\n'
                     || help[e] == '\t' || help[e] == '\r');
        if (leftOk && rightOk) return true;
        p = e;
    }
    return false;
}

}} // namespace sdr2hdr::win32
