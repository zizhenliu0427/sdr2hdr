#pragma once

#include "rtx_converter.h"
#include "ffmpeg_process.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

namespace sdr2hdr {

// Mirrors CLI Options (main.cpp wizard / batch flags).
struct ProcessOptions
{
    RtxConverter::Mode mode = RtxConverter::Mode::Hdr;
    bool modeSet = false;

    uint32_t vsrQuality   = 4;
    double   scale        = 2.0;
    uint32_t outW         = 0;
    uint32_t outH         = 0;
    uint32_t targetHeight = 0;
    bool     scaleSet     = false;
    bool     outputSizeSet = false;

    uint32_t contrast   = 100;
    uint32_t saturation = 100;
    uint32_t middleGray = 50;
    uint32_t maxLum     = 1000;

    std::string backend  = "nvenc";
    std::string codec    = "hevc";
    int         quality  = -1;
    bool        qualityAuto = false;
    std::string preset   = "";
    bool        copyAudio = true;
    bool        hwDecode  = true;
    bool        verbose   = false;
    bool        gpuOnly   = true;
    // VFR PTS passthrough (#32); --no-vfr-pts disables (GPU pipeline only).
    bool        vfrPts    = true;
    // Parallel NGX sessions (#11); 0 = auto. GPU pipeline only.
    int         ngxSessions = 0;
};

struct ProcessProgress
{
    uint64_t framesDone  = 0;
    uint64_t framesTotal = 0;
    double   elapsedSec  = 0.0;
    double   fps         = 0.0;
    double   percent     = 0.0;
    // Post-encode finalize step (teardown + audio mux + verify). Frames are
    // done; the file is still being written. UI should show "Finalizing…".
    bool     finalizing  = false;
};

using ProcessProgressCallback = std::function<void(const ProcessProgress&)>;

struct ProcessResult
{
    bool        ok = false;
    int         exitCode = 0;
    uint64_t    framesProcessed = 0;
    double      seconds = 0.0;
    std::string errorDetail;
};

// Run the full pipeline for one (input, output) pair. Blocks until done.
// onProgress may be invoked from a worker thread (~4 Hz); keep callbacks fast.
// cancelFlag: set to true to request early abort (best-effort).
ProcessResult processFile(const std::string& input,
                          const std::string& output,
                          const ProcessOptions& opts,
                          ProcessProgressCallback onProgress = {},
                          std::atomic<bool>* cancelFlag = nullptr);

// Resolve global quality default when neither --quality nor auto is set.
void applyDefaultQuality(ProcessOptions& opts);

// Full command-line entry point (defined in main.cpp). Shared by the standalone
// console build and the merged GUI binary, which forwards to it when launched
// with command-line arguments instead of showing the window.
int cliMain(int argc, char** argv);

} // namespace sdr2hdr
