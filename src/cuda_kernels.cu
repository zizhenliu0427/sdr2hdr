// CUDA kernels for the sdr2hdr GPU-only pipeline.
//
// Design notes
//   - Pixel-pair kernels: we process 2x2 quads so NV12/P010 chroma can be
//     subsampled with a single UV write for 4 luma samples. This is the
//     standard NVIDIA sample approach and maps cleanly to warp-level writes.
//   - Block size 16x8 (=128 threads per block) is a good balance for modern
//     SMs. Each thread handles a 2x2 pixel quad, so the effective tile is
//     32x16.
//   - All matrices baked in as constants: no need to touch constant memory
//     at runtime. Users pick between BT.601 / BT.709 / BT.2020 via separate
//     kernel instantiations.
//   - HDR path: RTX SDK emits already-PQ-encoded A2B10G10R10. We don't
//     apply any tone-mapping / gamma curve here - we just do RGB->YUV using
//     BT.2020 NCL limited range.
//
// Tested on RTX 30/40. Doesn't require any CUDA >= 12 features.

#include "cuda_kernels.h"

#include <cstdint>

namespace {

// ---------------------------------------------------------------------------
// Matrix tables
// ---------------------------------------------------------------------------
//
// Precomputed coefficients for Y / U / V from R,G,B in [0..1].
//   Y = kR*R + kG*G + kB*B
//   U = (B - Y) / Kb_norm  + 0.5
//   V = (R - Y) / Kr_norm  + 0.5
// where Kb_norm = 2*(1 - kB), Kr_norm = 2*(1 - kR).
//
// Limited-range offsets map [0..1] -> [16..235]/256 (luma) and
// [0..1] -> [16..240]/256 (chroma), scaled to bit depth.

struct ColorMatrix
{
    float kR, kG, kB;     // luma coefficients
    float ubR, ubG, ubB;  // U = ubR*R + ubG*G + ubB*B + 0.5
    float vrR, vrG, vrB;  // V = vrR*R + vrG*G + vrB*B + 0.5
};

constexpr ColorMatrix kBT709 = {
     0.2126f,  0.7152f,  0.0722f,
    -0.1146f, -0.3854f,  0.5000f,
     0.5000f, -0.4542f, -0.0458f
};

constexpr ColorMatrix kBT2020 = {
     0.2627f,  0.6780f,  0.0593f,
    -0.1396f, -0.3604f,  0.5000f,
     0.5000f, -0.4598f, -0.0402f
};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

__device__ __forceinline__ uint8_t clamp8(float v)
{
    v = fminf(fmaxf(v, 0.0f), 255.0f);
    return static_cast<uint8_t>(v + 0.5f);
}

__device__ __forceinline__ uint16_t clamp10(float v)
{
    // Value in [0..1023] float; result stored as 16-bit with 10 MSBs in use
    // (shifted << 6). Matches P010 layout.
    v = fminf(fmaxf(v, 0.0f), 1023.0f);
    uint16_t u = static_cast<uint16_t>(v + 0.5f);
    return static_cast<uint16_t>(u << 6);
}

// ---------------------------------------------------------------------------
// NV12 -> RGBA8
// ---------------------------------------------------------------------------
//
// NvDecoder writes NV12 as:
//   Y  : height       rows of width  bytes, pitch = srcPitch
//   UV : height/2     rows of width  bytes (U,V interleaved), pitch = srcPitch
// The UV plane starts at srcNv12 + srcPitch * lumaHeight.
//
// Standard BT.709 / BT.601 limited-range inverse matrix (Y in [16..235],
// U,V in [16..240]). Full-range removes the -16 / *(255/219) correction.

template<bool BT601, bool FullRange>
__global__ void kernel_nv12_to_rgba8(
    const uint8_t* __restrict__ Y,
    const uint8_t* __restrict__ UV,
    int srcPitch,
    uint8_t* __restrict__ dstRgba, int dstPitch,
    int width, int height)
{
    const int x = (blockIdx.x * blockDim.x + threadIdx.x) * 2;
    const int y = (blockIdx.y * blockDim.y + threadIdx.y) * 2;
    if (x >= width || y >= height) return;

    // 2x2 luma block
    uint8_t y00 = Y[(y    ) * srcPitch + (x    )];
    uint8_t y01 = Y[(y    ) * srcPitch + (x + 1)];
    uint8_t y10 = Y[(y + 1) * srcPitch + (x    )];
    uint8_t y11 = Y[(y + 1) * srcPitch + (x + 1)];

    // Shared chroma pair
    uint8_t u = UV[(y / 2) * srcPitch + (x / 2) * 2    ];
    uint8_t v = UV[(y / 2) * srcPitch + (x / 2) * 2 + 1];

    float U = (int)u - 128;
    float V = (int)v - 128;

    // Luma normalization
    auto decodeY = [] (uint8_t yv) -> float {
        if constexpr (FullRange) {
            return (float)yv;
        } else {
            return ((float)yv - 16.0f) * (255.0f / 219.0f);
        }
    };

    float Cu = FullRange ? U : (U * (255.0f / 224.0f));
    float Cv = FullRange ? V : (V * (255.0f / 224.0f));

    // Inverse matrix coefficients
    float aR, aB;
    if constexpr (BT601) {
        // BT.601: R = Y + 1.402*Cr ; B = Y + 1.772*Cb
        aR = 1.402f; aB = 1.772f;
    } else {
        // BT.709: R = Y + 1.5748*Cr ; B = Y + 1.8556*Cb
        aR = 1.5748f; aB = 1.8556f;
    }
    // G derived from R,B via Y = kR*R + kG*G + kB*B.
    const float kR = BT601 ? 0.299f  : 0.2126f;
    const float kB = BT601 ? 0.114f  : 0.0722f;
    const float kG = 1.0f - kR - kB;
    // Cg coefficients relative to Cu, Cv:
    //   G = Y - (kR / kG) * Cr_extra - (kB / kG) * Cb_extra
    // where Cr_extra = aR*Cv / 2 ... equivalent closed form below.

    auto store = [&] (int px, int py, float Yf) {
        float R = Yf + aR * Cv;
        float B = Yf + aB * Cu;
        float G = (Yf - kR * R - kB * B) / kG;
        uint8_t* p = dstRgba + py * dstPitch + px * 4;
        p[0] = clamp8(R);
        p[1] = clamp8(G);
        p[2] = clamp8(B);
        p[3] = 255;
    };

    store(x    , y    , decodeY(y00));
    store(x + 1, y    , decodeY(y01));
    store(x    , y + 1, decodeY(y10));
    store(x + 1, y + 1, decodeY(y11));
}

// ---------------------------------------------------------------------------
// P010 -> RGBA8  (for future 10-bit input support)
// ---------------------------------------------------------------------------
template<bool BT2020, bool FullRange>
__global__ void kernel_p010_to_rgba8(
    const uint16_t* __restrict__ Y,
    const uint16_t* __restrict__ UV,
    int srcPitchBytes,
    uint8_t* __restrict__ dstRgba, int dstPitch,
    int width, int height)
{
    const int x = (blockIdx.x * blockDim.x + threadIdx.x) * 2;
    const int y = (blockIdx.y * blockDim.y + threadIdx.y) * 2;
    if (x >= width || y >= height) return;

    const int srcPitchWords = srcPitchBytes / 2;

    // P010 stores 10-bit samples left-justified: value = word >> 6.
    auto fetchY = [&] (int px, int py) -> float {
        uint16_t w = Y[py * srcPitchWords + px];
        float yv = (float)(w >> 6);        // [0..1023]
        if constexpr (FullRange) {
            return yv * (255.0f / 1023.0f);
        } else {
            return (yv - 64.0f) * (255.0f / (940.0f - 64.0f));
        }
    };

    uint16_t u16 = UV[(y / 2) * srcPitchWords + (x / 2) * 2    ];
    uint16_t v16 = UV[(y / 2) * srcPitchWords + (x / 2) * 2 + 1];
    float U = (float)(u16 >> 6) - 512.0f;
    float V = (float)(v16 >> 6) - 512.0f;
    if constexpr (!FullRange)
    {
        U = U * (255.0f / (960.0f - 64.0f));
        V = V * (255.0f / (960.0f - 64.0f));
    }
    else
    {
        U = U * (255.0f / 1023.0f);
        V = V * (255.0f / 1023.0f);
    }

    float aR, aB;
    if constexpr (BT2020) { aR = 1.4746f; aB = 1.8814f; }
    else                  { aR = 1.5748f; aB = 1.8556f; }

    const float kR = BT2020 ? 0.2627f : 0.2126f;
    const float kB = BT2020 ? 0.0593f : 0.0722f;
    const float kG = 1.0f - kR - kB;

    auto store = [&] (int px, int py, float Yf) {
        float R = Yf + aR * V;
        float B = Yf + aB * U;
        float G = (Yf - kR * R - kB * B) / kG;
        uint8_t* p = dstRgba + py * dstPitch + px * 4;
        p[0] = clamp8(R);
        p[1] = clamp8(G);
        p[2] = clamp8(B);
        p[3] = 255;
    };

    store(x    , y    , fetchY(x    , y    ));
    store(x + 1, y    , fetchY(x + 1, y    ));
    store(x    , y + 1, fetchY(x    , y + 1));
    store(x + 1, y + 1, fetchY(x + 1, y + 1));
}

// ---------------------------------------------------------------------------
// ABGR2101010 -> P010 (BT.2020 NCL limited range)
// ---------------------------------------------------------------------------
// Input packed uint32 per pixel, little endian:
//   bits [ 0..9 ]  = R   (10 bits)
//   bits [10..19]  = G   (10 bits)
//   bits [20..29]  = B   (10 bits)
//   bits [30..31]  = A   (usually 3)
//
// Output P010:
//   Y plane   : 16-bit words, value << 6 in [0..65472].
//   UV plane  : 16-bit U then V, 2x2 subsampled, same bit packing.
//
// Luma kR,kG,kB = BT.2020 coefficients. Limited range: Y [64..940], UV [64..960].

__global__ void kernel_abgr10_to_p010_bt2020_limited(
    const uint32_t* __restrict__ src, int srcPitchBytes,
    uint16_t* __restrict__ dstY,      int dstYPitchBytes,
    uint16_t* __restrict__ dstUV,     int dstUVPitchBytes,
    int width, int height)
{
    const int x = (blockIdx.x * blockDim.x + threadIdx.x) * 2;
    const int y = (blockIdx.y * blockDim.y + threadIdx.y) * 2;
    if (x >= width || y >= height) return;

    const int srcPitchWords   = srcPitchBytes  / 4;
    const int dstYPitchWords  = dstYPitchBytes / 2;
    const int dstUVPitchWords = dstUVPitchBytes / 2;

    auto load = [&] (int px, int py, float& R, float& G, float& B) {
        uint32_t p = src[py * srcPitchWords + px];
        R = (float)((p >>  0) & 0x3FFu) / 1023.0f;
        G = (float)((p >> 10) & 0x3FFu) / 1023.0f;
        B = (float)((p >> 20) & 0x3FFu) / 1023.0f;
    };

    float R00, G00, B00; load(x    , y    , R00, G00, B00);
    float R01, G01, B01; load(x + 1, y    , R01, G01, B01);
    float R10, G10, B10; load(x    , y + 1, R10, G10, B10);
    float R11, G11, B11; load(x + 1, y + 1, R11, G11, B11);

    // BT.2020 limited range conversion.
    auto rgbToY = [] (float R, float G, float B) -> float {
        // Y_limited [64..940] mapped to 10-bit code.
        float yLin = kBT2020.kR * R + kBT2020.kG * G + kBT2020.kB * B;
        return 64.0f + yLin * (940.0f - 64.0f);
    };
    auto rgbToUV = [] (float R, float G, float B, float& U, float& V) {
        float u = kBT2020.ubR * R + kBT2020.ubG * G + kBT2020.ubB * B;
        float v = kBT2020.vrR * R + kBT2020.vrG * G + kBT2020.vrB * B;
        // u,v in [-0.5 .. 0.5], offset to [0..1], map to [64..960].
        U = 64.0f + (u + 0.5f) * (960.0f - 64.0f);
        V = 64.0f + (v + 0.5f) * (960.0f - 64.0f);
    };

    dstY[(y    ) * dstYPitchWords + (x    )] = clamp10(rgbToY(R00, G00, B00));
    dstY[(y    ) * dstYPitchWords + (x + 1)] = clamp10(rgbToY(R01, G01, B01));
    dstY[(y + 1) * dstYPitchWords + (x    )] = clamp10(rgbToY(R10, G10, B10));
    dstY[(y + 1) * dstYPitchWords + (x + 1)] = clamp10(rgbToY(R11, G11, B11));

    // 2x2 average for chroma.
    float Ravg = (R00 + R01 + R10 + R11) * 0.25f;
    float Gavg = (G00 + G01 + G10 + G11) * 0.25f;
    float Bavg = (B00 + B01 + B10 + B11) * 0.25f;
    float U, V;
    rgbToUV(Ravg, Gavg, Bavg, U, V);

    const int uvRow = y / 2;
    const int uvCol = x / 2;
    dstUV[uvRow * dstUVPitchWords + uvCol * 2    ] = clamp10(U);
    dstUV[uvRow * dstUVPitchWords + uvCol * 2 + 1] = clamp10(V);
}

// ---------------------------------------------------------------------------
// RGBA8 -> NV12 (BT.709 limited range)
// ---------------------------------------------------------------------------

__global__ void kernel_rgba8_to_nv12_bt709_limited(
    const uint8_t* __restrict__ src,  int srcPitch,
    uint8_t*       __restrict__ dstY, int dstYPitch,
    uint8_t*       __restrict__ dstUV,int dstUVPitch,
    int width, int height)
{
    const int x = (blockIdx.x * blockDim.x + threadIdx.x) * 2;
    const int y = (blockIdx.y * blockDim.y + threadIdx.y) * 2;
    if (x >= width || y >= height) return;

    auto load = [&] (int px, int py, float& R, float& G, float& B) {
        const uint8_t* p = src + py * srcPitch + px * 4;
        R = (float)p[0] / 255.0f;
        G = (float)p[1] / 255.0f;
        B = (float)p[2] / 255.0f;
    };

    float R00, G00, B00; load(x    , y    , R00, G00, B00);
    float R01, G01, B01; load(x + 1, y    , R01, G01, B01);
    float R10, G10, B10; load(x    , y + 1, R10, G10, B10);
    float R11, G11, B11; load(x + 1, y + 1, R11, G11, B11);

    auto rgbToY = [] (float R, float G, float B) -> uint8_t {
        float yLin = kBT709.kR * R + kBT709.kG * G + kBT709.kB * B;
        float Y = 16.0f + yLin * (235.0f - 16.0f);
        return clamp8(Y);
    };
    auto rgbToUV = [] (float R, float G, float B, uint8_t& U, uint8_t& V) {
        float u = kBT709.ubR * R + kBT709.ubG * G + kBT709.ubB * B;
        float v = kBT709.vrR * R + kBT709.vrG * G + kBT709.vrB * B;
        U = clamp8(128.0f + u * (240.0f - 16.0f));
        V = clamp8(128.0f + v * (240.0f - 16.0f));
    };

    dstY[(y    ) * dstYPitch + (x    )] = rgbToY(R00, G00, B00);
    dstY[(y    ) * dstYPitch + (x + 1)] = rgbToY(R01, G01, B01);
    dstY[(y + 1) * dstYPitch + (x    )] = rgbToY(R10, G10, B10);
    dstY[(y + 1) * dstYPitch + (x + 1)] = rgbToY(R11, G11, B11);

    float Ravg = (R00 + R01 + R10 + R11) * 0.25f;
    float Gavg = (G00 + G01 + G10 + G11) * 0.25f;
    float Bavg = (B00 + B01 + B10 + B11) * 0.25f;
    uint8_t U, V;
    rgbToUV(Ravg, Gavg, Bavg, U, V);

    const int uvRow = y / 2;
    const int uvCol = x / 2;
    dstUV[uvRow * dstUVPitch + uvCol * 2    ] = U;
    dstUV[uvRow * dstUVPitch + uvCol * 2 + 1] = V;
}

dim3 calcGrid(int w, int h, dim3 block)
{
    // Each thread handles a 2x2 pixel block, so the grid covers (w/2, h/2).
    const int gx = (w / 2 + block.x - 1) / block.x;
    const int gy = (h / 2 + block.y - 1) / block.y;
    return dim3(gx, gy);
}

} // namespace

namespace sdr2hdr { namespace kernels {

cudaError_t launchNv12ToRgba8(
    const void* srcNv12, int srcPitch,
    void*       dstRgba, int dstPitch,
    int         width,   int height,
    bool        bt601,
    bool        fullRange,
    cudaStream_t stream)
{
    if (width <= 0 || height <= 0) return cudaErrorInvalidValue;

    const uint8_t* Y  = static_cast<const uint8_t*>(srcNv12);
    const uint8_t* UV = Y + static_cast<size_t>(srcPitch) * height;

    dim3 block(16, 8);
    dim3 grid = calcGrid(width, height, block);

    if (bt601 && fullRange)
        kernel_nv12_to_rgba8<true,  true >  <<<grid, block, 0, stream>>>(Y, UV, srcPitch,
            static_cast<uint8_t*>(dstRgba), dstPitch, width, height);
    else if (bt601 && !fullRange)
        kernel_nv12_to_rgba8<true,  false>  <<<grid, block, 0, stream>>>(Y, UV, srcPitch,
            static_cast<uint8_t*>(dstRgba), dstPitch, width, height);
    else if (!bt601 && fullRange)
        kernel_nv12_to_rgba8<false, true >  <<<grid, block, 0, stream>>>(Y, UV, srcPitch,
            static_cast<uint8_t*>(dstRgba), dstPitch, width, height);
    else
        kernel_nv12_to_rgba8<false, false>  <<<grid, block, 0, stream>>>(Y, UV, srcPitch,
            static_cast<uint8_t*>(dstRgba), dstPitch, width, height);

    return cudaGetLastError();
}

cudaError_t launchP010ToRgba8(
    const void* srcP010, int srcPitch,
    void*       dstRgba, int dstPitch,
    int         width,   int height,
    bool        bt2020,
    bool        fullRange,
    cudaStream_t stream)
{
    if (width <= 0 || height <= 0) return cudaErrorInvalidValue;

    const uint16_t* Y  = static_cast<const uint16_t*>(srcP010);
    const uint16_t* UV = reinterpret_cast<const uint16_t*>(
        static_cast<const uint8_t*>(srcP010) + static_cast<size_t>(srcPitch) * height);

    dim3 block(16, 8);
    dim3 grid = calcGrid(width, height, block);

    if (bt2020 && fullRange)
        kernel_p010_to_rgba8<true,  true >  <<<grid, block, 0, stream>>>(Y, UV, srcPitch,
            static_cast<uint8_t*>(dstRgba), dstPitch, width, height);
    else if (bt2020 && !fullRange)
        kernel_p010_to_rgba8<true,  false>  <<<grid, block, 0, stream>>>(Y, UV, srcPitch,
            static_cast<uint8_t*>(dstRgba), dstPitch, width, height);
    else if (!bt2020 && fullRange)
        kernel_p010_to_rgba8<false, true >  <<<grid, block, 0, stream>>>(Y, UV, srcPitch,
            static_cast<uint8_t*>(dstRgba), dstPitch, width, height);
    else
        kernel_p010_to_rgba8<false, false>  <<<grid, block, 0, stream>>>(Y, UV, srcPitch,
            static_cast<uint8_t*>(dstRgba), dstPitch, width, height);

    return cudaGetLastError();
}

cudaError_t launchAbgr2101010ToP010(
    const void* srcAbgr10,  int srcPitch,
    void*       dstYPlane,  int dstYPitch,
    void*       dstUVPlane, int dstUVPitch,
    int         width,      int height,
    cudaStream_t stream)
{
    if (width <= 0 || height <= 0) return cudaErrorInvalidValue;

    dim3 block(16, 8);
    dim3 grid = calcGrid(width, height, block);

    kernel_abgr10_to_p010_bt2020_limited<<<grid, block, 0, stream>>>(
        static_cast<const uint32_t*>(srcAbgr10), srcPitch,
        static_cast<uint16_t*>(dstYPlane),       dstYPitch,
        static_cast<uint16_t*>(dstUVPlane),      dstUVPitch,
        width, height);
    return cudaGetLastError();
}

cudaError_t launchRgba8ToNv12(
    const void* srcRgba,    int srcPitch,
    void*       dstYPlane,  int dstYPitch,
    void*       dstUVPlane, int dstUVPitch,
    int         width,      int height,
    cudaStream_t stream)
{
    if (width <= 0 || height <= 0) return cudaErrorInvalidValue;

    dim3 block(16, 8);
    dim3 grid = calcGrid(width, height, block);

    kernel_rgba8_to_nv12_bt709_limited<<<grid, block, 0, stream>>>(
        static_cast<const uint8_t*>(srcRgba), srcPitch,
        static_cast<uint8_t*>(dstYPlane),     dstYPitch,
        static_cast<uint8_t*>(dstUVPlane),    dstUVPitch,
        width, height);
    return cudaGetLastError();
}

}} // namespace sdr2hdr::kernels
