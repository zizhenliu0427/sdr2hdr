// Small Win32 pipe / process / text helpers shared between ffmpeg_process.cpp
// and bitstream_pipe.cpp. Deliberately internal to the sdr2hdr build - no
// stable ABI, no distribution.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdint>
#include <string>

namespace sdr2hdr { namespace win32 {

// UTF-8 <-> UTF-16 string conversion.
std::wstring toWide  (const std::string& utf8);
std::string  fromWide(const wchar_t* w);

// Directory the current exe lives in, UTF-8.
std::string exeDir();

// True if a given UTF-8 path points to a regular file.
bool fileExists(const std::string& p);

// Returns either `"<exeDir>\exeName"` (quoted, if present) or the plain
// `exeName` (so Windows resolves through PATH).
const std::string& toolPath(const char* exeName);

// Double-quote a UTF-8 string (no escaping of internal quotes; paths have
// none on Windows).
std::string quote(const std::string& s);

// Anonymous pipe pair. `childReads==true` => child reads via writeEnd.
struct PipePair { HANDLE readEnd = nullptr; HANDLE writeEnd = nullptr; };
bool makePipe(PipePair& pipe, bool childReads);

// Spawn a child process with inherited std handles. cmdline is UTF-8.
bool launch(const std::string& cmdline,
            HANDLE hStdin, HANDLE hStdout, HANDLE hStderr,
            PROCESS_INFORMATION& pi);

// Blocking read/write; return false on error or partial transfer.
bool readAll (HANDLE h, void*       dst, size_t n);
bool writeAll(HANDLE h, const void* src, size_t n);

// Non-blocking best-effort read: return bytes actually read (>=0, 0 on EOF).
size_t readSome(HANDLE h, void* dst, size_t capacity);

// Run a child command, capture its stdout (and optionally stderr) into a
// std::string, wait for exit, return the exit code. Returns false if spawn
// itself failed.
bool runAndCapture(const std::string& cmdline, std::string& out, DWORD& exitCode,
                   bool mergeStderr = false);

// Create an inheritable temp-file handle for child stderr capture. `outLogPath`
// receives the UTF-8 path so the caller can later re-open & read it.
HANDLE openStderrCapture(std::string& outLogPath);

// Dump last ~4 KB of a stderr log file to our own stderr.
void dumpStderrLog(const std::string& logPath, const char* label);

// Probe whether `ffmpeg -h encoder=<enc>` mentions the -<optName> option.
// Result cached per encoder; first call spawns ffmpeg once.
bool encoderHasOption(const std::string& encoderName, const std::string& optName);

}} // namespace sdr2hdr::win32
