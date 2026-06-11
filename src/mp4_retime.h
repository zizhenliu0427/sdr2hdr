// In-place VFR re-timing of a video-only MP4 (#32 VFR PTS passthrough).
//
// The GPU pipeline encodes IPPP (encode order == display order) and emits a
// raw elementary stream, so the only place timing exists is the container.
// ffmpeg's CLI cannot apply per-frame timestamps when muxing a raw ES, so we
// let it containerize the ES at CFR first and then rewrite the resulting
// MP4's sample table: the stts box gets one delta per frame derived from the
// source's display-order timestamps, and every duration field that depends
// on it (mdhd / tkhd / mvhd / elst) is updated to match.
//
// Only the moov box changes. ffmpeg writes moov after mdat when -movflags
// +faststart is not given, so the 20+ GB of sample data is untouched -- the
// file is truncated at the moov offset and a rebuilt moov is appended.
//
// The later audio-merge pass (ffmpeg -c copy) carries these per-frame
// timestamps into the final MP4/MKV unchanged.
#pragma once

#include <string>

struct FramePtsInfo;   // ffmpeg_process.h

// Rewrite the single video track of `mp4Path` so sample i is presented at
// framePts.pts[i] (normalised to start at 0, expressed at 90 kHz). Fails --
// without modifying the file -- when the layout is not the expected
// "video-only, moov-last, no ctts" shape or the sample count does not match
// framePts.pts.size(); the caller then keeps the CFR timing.
bool applyMp4FramePts(const std::string& mp4Path,
                      const FramePtsInfo& framePts,
                      bool verbose);
