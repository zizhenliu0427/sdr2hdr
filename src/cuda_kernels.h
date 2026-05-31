// CUDA kernels for the GPU-only sdr2hdr pipeline.
//
// All pointers are CUdeviceptr (plain byte pointers in device VRAM). The
// pitch values are in BYTES, not pixels. Kernels run on an externally
// owned CUstream; they do not synchronise on their own.
#pragma once

#include <cuda.h>
#include <cuda_runtime.h>
#include <cstdint>

namespace sdr2hdr { namespace kernels {

// ---------------------------------------------------------------------------
// NV12  ->  RGBA8       (decoder output -> RTX SDK input)
// ---------------------------------------------------------------------------
// srcNv12    : Y plane at srcNv12, UV plane at srcNv12 + srcPitch*height
//              (NvDecoder stores NV12 as one contiguous CUdeviceptr with Y
//              followed immediately by interleaved UV at half height).
// bt601      : if true, use BT.601 luma coefficients; otherwise BT.709.
// fullRange  : false -> limited range (16..235/240); true -> full range.
// dstRgba    : R,G,B,A bytes (alpha always 255).
cudaError_t launchNv12ToRgba8(
    const void*  srcNv12, int srcPitch,
    void*        dstRgba, int dstPitch,
    int          width,   int height,
    bool         bt601,
    bool         fullRange,
    cudaStream_t stream);

// ---------------------------------------------------------------------------
// P010 (10-bit NV12) -> RGBA8
// ---------------------------------------------------------------------------
// Needed when the source bitstream is 10-bit (e.g. HEVC Main10 HDR10 already).
// Currently not used (sdr2hdr assumes SDR input), kept for future.
cudaError_t launchP010ToRgba8(
    const void*  srcP010, int srcPitch,
    void*        dstRgba, int dstPitch,
    int          width,   int height,
    bool         bt2020,
    bool         fullRange,
    cudaStream_t stream);

// ---------------------------------------------------------------------------
// ABGR2101010 (RTX SDK HDR output) -> P016 / P010
// ---------------------------------------------------------------------------
// RTX SDK writes the tone-mapped HDR frame as packed 32-bit A2B10G10R10
// (little-endian: bits [0..9]=R, [10..19]=G, [20..29]=B, [30..31]=A=3).
// NVENC's HEVC Main10 input wants P010:  Y plane (16-bit little-endian,
// 10 MSBs carry the value), followed by interleaved UV (16-bit each).
//
// This kernel applies BT.2020 NCL limited-range matrix (R,G,B encoded as PQ
// already -- no transfer conversion needed).
cudaError_t launchAbgr2101010ToP010(
    const void*  srcAbgr10,   int srcPitch,
    void*        dstYPlane,   int dstYPitch,
    void*        dstUVPlane,  int dstUVPitch,
    int          width,       int height,
    cudaStream_t stream);

// ---------------------------------------------------------------------------
// RGBA8 (RTX SDK SDR output, e.g. VSR-only) -> NV12
// ---------------------------------------------------------------------------
// BT.709 NCL limited-range.
cudaError_t launchRgba8ToNv12(
    const void*  srcRgba,    int srcPitch,
    void*        dstYPlane,  int dstYPitch,
    void*        dstUVPlane, int dstUVPitch,
    int          width,      int height,
    cudaStream_t stream);

}} // namespace sdr2hdr::kernels
