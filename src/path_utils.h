#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace sdr2hdr {

struct ProcessOptions;

// UTF-8 path helpers (MSVC std::filesystem treats narrow strings as ACP, not UTF-8).
std::filesystem::path pathFromUtf8(const std::string& s);
std::string utf8FromPath(const std::filesystem::path& p);

std::string extOf(const std::string& p);
std::string replaceExt(const std::string& p, const std::string& newExt);
bool isVideoExt(const std::string& ext);
bool parseSize(const std::string& s, uint32_t& w, uint32_t& h);

std::string autoSuffix(const ProcessOptions& opts);
std::string resolveOutputPath(const std::string& input,
                              const std::string& outTarget,
                              bool outIsDir,
                              bool explicitOut,
                              const ProcessOptions& opts);

} // namespace sdr2hdr
