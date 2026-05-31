// Thin wrapper around RTX_Video_API (CUDA host-pointer path).
//
//   Mode::Hdr     : SDR RGBA 8-bit   -> HDR ABGR10 10-bit,  same WxH
//   Mode::Vsr     : SDR RGBA 8-bit   -> SDR ARGB  8-bit,    upscaled
//   Mode::VsrHdr  : SDR RGBA 8-bit   -> HDR ABGR10 10-bit,  upscaled
//
// All I/O uses host memory with pitch = 4*width.
#pragma once

#include <cstdint>

class RtxConverter
{
public:
    enum class Mode { Hdr, Vsr, VsrHdr };

    struct Params {
        // TrueHDR
        uint32_t contrast   = 100;  // 0..200
        uint32_t saturation = 100;  // 0..200
        uint32_t middleGray = 50;   // 10..100
        uint32_t maxLum     = 1000; // 400..2000
        // VSR
        uint32_t vsrQuality = 4;    // 1..4 (higher = better, slower; default: max)
    };

    RtxConverter();
    ~RtxConverter();

    // Legacy path: creates its own CUDA context internally (owned = true).
    bool initialize(Mode mode,
                    uint32_t inWidth,  uint32_t inHeight,
                    uint32_t outWidth, uint32_t outHeight,
                    int gpuIndex = 0);

    // GPU-only path: reuse the caller's CUDA context (we don't own it).
    // The caller (GpuPipeline) creates one context and shares it between
    // NvDecoder, NvEncoderCuda, and us so all VRAM allocations are
    // addressable by each component without cross-context copies.
    bool initializeWithContext(Mode mode,
                               uint32_t inWidth,  uint32_t inHeight,
                               uint32_t outWidth, uint32_t outHeight,
                               void* existingCuContext,
                               void* cuStream = nullptr,
                               int   gpuIndex = 0);

    // Host-pointer path (pitch=4*width). Pulls data over PCIe each frame.
    bool convertFrame(const void* hostIn, void* hostOut, const Params& p);

    // Device-pointer path. Both pointers are CUdeviceptr (uint64_t on 64-bit)
    // aliased to `void*` here to keep the public header CUDA-free.
    // Both buffers must be RGBA8 (or A2B10G10R10 for HDR output) with
    // stride = 4*width. Returns true on success.
    bool convertFrameDevicePtr(unsigned long long devIn,
                               unsigned long long devOut,
                               const Params& p);

    void shutdown();

    // Introspection
    Mode     mode()        const { return m_mode; }
    uint32_t inWidth()     const { return m_inW;  }
    uint32_t inHeight()    const { return m_inH;  }
    uint32_t outWidth()    const { return m_outW; }
    uint32_t outHeight()   const { return m_outH; }
    bool     outputIsHdr() const { return m_mode != Mode::Vsr; } // VSR-only keeps SDR
    size_t   inputFrameBytes()  const { return static_cast<size_t>(m_inW)  * m_inH  * 4; }
    size_t   outputFrameBytes() const { return static_cast<size_t>(m_outW) * m_outH * 4; }

private:
    Mode     m_mode         = Mode::Hdr;
    uint32_t m_inW = 0, m_inH = 0, m_outW = 0, m_outH = 0;
    void*    m_cuContext    = nullptr;
    void*    m_cuStream     = nullptr;
    bool     m_rtxAlive     = false;
    bool     m_ownsContext  = false;
};
