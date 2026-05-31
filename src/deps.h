// Runtime dependency bootstrap for sdr2hdr.
//
// sdr2hdr needs two external pieces at runtime that we deliberately do NOT
// commit to the source repository (license + size reasons):
//   1. ffmpeg.exe / ffprobe.exe  -- third-party, LGPL/GPL. Freely
//      redistributable, so we CAN fetch a prebuilt static build on demand.
//   2. nvngx_truehdr.dll / nvngx_vsr.dll -- NVIDIA RTX Video SDK model files,
//      gated behind NVIDIA's developer login + EULA. We CANNOT auto-download
//      these; the best we can do is detect their absence and tell the user
//      exactly where to get them.
#pragma once

#include <string>

namespace sdr2hdr { namespace deps {

// Ensure ffmpeg.exe + ffprobe.exe are usable (either next to the exe or on
// PATH). If they're missing, download a prebuilt static ffmpeg and install
// ffmpeg.exe / ffprobe.exe into the exe directory. Returns true if both are
// available after the call. `interactive` is currently informational only.
//
// Opt out of the auto-download by setting the env var SDR2HDR_NO_AUTODOWNLOAD.
bool ensureFfmpeg(bool interactive);

// Check whether the NVIDIA NGX feature DLLs are present next to the exe.
// These cannot be auto-downloaded; if missing, prints a bilingual message
// pointing at the RTX Video SDK download page. Returns true if both present.
bool checkNgxDlls();

}} // namespace sdr2hdr::deps
