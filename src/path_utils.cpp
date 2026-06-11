#include "path_utils.h"
#include "engine.h"

#include "win32_utils.h"

#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <system_error>

namespace fs = std::filesystem;

namespace sdr2hdr {

fs::path pathFromUtf8(const std::string& s)
{
    if (s.empty()) return {};
    return fs::path(win32::toWide(s));
}

std::string utf8FromPath(const fs::path& p)
{
    return win32::fromWide(p.wstring().c_str());
}

std::string extOf(const std::string& p)
{
    auto slash = p.find_last_of("\\/");
    auto dot   = p.find_last_of('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash))
        return {};
    std::string e;
    for (size_t i = dot + 1; i < p.size(); ++i)
        e.push_back(static_cast<char>(std::tolower(p[i])));
    return e;
}

std::string replaceExt(const std::string& p, const std::string& newExt)
{
    auto slash = p.find_last_of("\\/");
    auto dot   = p.find_last_of('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash))
        return p + "." + newExt;
    return p.substr(0, dot + 1) + newExt;
}

bool isVideoExt(const std::string& ext)
{
    static const char* known[] = {
        "mp4","m4v","mov","qt","mkv","ts","m2ts","mts","avi","webm",
        "wmv","flv","avchd","mpg","mpeg","vob","3gp","3g2"
    };
    for (auto* s : known)
        if (ext == s) return true;
    return false;
}

bool parseSize(const std::string& s, uint32_t& w, uint32_t& h)
{
    auto x = s.find('x');
    if (x == std::string::npos) x = s.find('X');
    if (x == std::string::npos) return false;
    w = static_cast<uint32_t>(std::atoi(s.substr(0, x).c_str()));
    h = static_cast<uint32_t>(std::atoi(s.substr(x + 1).c_str()));
    return w > 0 && h > 0;
}

std::string autoSuffix(const ProcessOptions& opts)
{
    const bool hdr = (opts.mode == RtxConverter::Mode::Hdr ||
                      opts.mode == RtxConverter::Mode::VsrHdr);
    const bool vsr = (opts.mode == RtxConverter::Mode::Vsr ||
                      opts.mode == RtxConverter::Mode::VsrHdr);

    std::string s;
    if (vsr)
    {
        if (opts.targetHeight)
            s += "_" + std::to_string(opts.targetHeight) + "p";
        else if (opts.outputSizeSet)
            s += "_" + std::to_string(opts.outW) + "x" + std::to_string(opts.outH);
        else
        {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "_%gx", opts.scale);
            s += buf;
        }
    }
    if (hdr) s += "_hdr";
    if (s.empty()) s = "_out";
    return s;
}

std::string resolveOutputPath(const std::string& input,
                              const std::string& outTarget,
                              bool outIsDir,
                              bool explicitOut,
                              const ProcessOptions& opts)
{
    if (explicitOut && !outIsDir)
    {
        std::string inExt  = extOf(input);
        std::string outExt = extOf(outTarget);
        if (!inExt.empty() && inExt != outExt)
            return replaceExt(outTarget, inExt);
        return outTarget;
    }

    fs::path inPath = pathFromUtf8(input);
    std::string stem = utf8FromPath(inPath.stem());
    std::string ext  = utf8FromPath(inPath.extension());
    std::string auto_name = stem + autoSuffix(opts) + ext;

    fs::path outDir;
    if (explicitOut && outIsDir)
        outDir = pathFromUtf8(outTarget);
    else
        outDir = inPath.parent_path();

    if (outDir.empty()) outDir = pathFromUtf8(".");

    std::error_code ec;
    fs::create_directories(outDir, ec);
    return utf8FromPath(outDir / pathFromUtf8(auto_name));
}

} // namespace sdr2hdr
