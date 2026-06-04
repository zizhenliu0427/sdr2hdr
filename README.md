# sdr2hdr — RTX Video SDK command-line tool

> **Language / 语言**: **English** · [中文](README.zh.md)

A command-line tool that applies NVIDIA RTX Video AI — RTX HDR (TrueHDR) and RTX VSR super-resolution — to videos and saves the processed result to a file. Unlike real-time playback filters (e.g. in Chrome or PotPlayer), sdr2hdr enables offline processing so you can keep the enhanced output.


| Mode          | Purpose                                     | Colour space  | Resolution            |
| ------------- | ------------------------------------------- | ------------- | --------------------- |
| `--hdr`       | RTX HDR (**TrueHDR**): convert SDR to HDR   | HDR10         | same as input         |
| `--vsr`       | **VSR**: AI video super-resolution upscale  | SDR (BT.709)  | upscale (default 4K)  |
| `--vsr-hdr`   | VSR **+** HDR combined                      | HDR10         | upscale (default 4K)  |


Internally the stages are chained over anonymous pipes as `ffmpeg (NVDEC) → sdr2hdr.exe (CUDA + RTX Video SDK) → ffmpeg (NVENC)`, a frame-level pipeline with no intermediate files on disk.

## Get it without building (prebuilt release)

If you just want to **use** the tool and don't care about the source code:

1. Open the repository's **Releases** page and download the latest `sdr2hdr_vX.Y.zip`.
2. Unzip it anywhere.
3. **Double-click `sdr2hdr.exe`** (interactive wizard), **drag video files onto it**, or run it from a terminal (see [Usage](#usage)).

Requirements: an **NVIDIA RTX GPU** + a recent driver (**≥ r570.20**). Nothing else — no ffmpeg, no CUDA Toolkit, no Visual Studio.

> **What's in the archive.** A ready-to-run release bundles `ffmpeg.exe` / `ffprobe.exe` (third-party, LGPL/GPL) and NVIDIA's `nvngx_truehdr.dll` / `nvngx_vsr.dll` model files so the tool works out of the box.
>
> **Licensing note.** Those bundled files are third-party redistributables: the NVIDIA NGX DLLs are governed by the [RTX Video SDK licence](https://developer.nvidia.com/rtx-video-sdk) (commercial redistribution needs an NVIDIA Application ID) and ffmpeg by LGPL/GPL.
>
> **If a release ships `sdr2hdr.exe` only:**
> - **ffmpeg / ffprobe** are fetched automatically on first run — if they aren't found next to the exe or on `PATH`, the tool downloads a prebuilt static ffmpeg (~90 MB, from gyan.dev) over HTTPS, **verifies the publisher's SHA-256**, and installs it into its own folder. This happens once. Disable it with the env var `SDR2HDR_NO_AUTODOWNLOAD` (then install ffmpeg yourself).
> - **`nvngx_truehdr.dll` / `nvngx_vsr.dll`** cannot be auto-downloaded (they're gated behind NVIDIA's SDK login + EULA). If they're missing, the tool prints the [RTX Video SDK](https://developer.nvidia.com/rtx-video-sdk) link; download the SDK and copy `bin\Windows\x64\rel\nvngx_*.dll` next to the exe.

## Container (wrapper format) is preserved

The tool **automatically aligns the output extension to the input**, so the source audio/subtitle tracks pass straight through via `-c:a copy` without muxer errors like "input is FLAC audio but MP4 doesn't accept it".


| Input      | What you typed | What actually gets written       |
| ---------- | -------------- | -------------------------------- |
| `in.mkv`   | `out.mp4`      | **`out.mkv`** (auto-corrected, a Note is printed) |
| `in.mp4`   | `out.mov`      | **`out.mp4`**                    |
| `in.ts`    | `out.mp4`      | **`out.ts`**                     |
| `in.mp4`   | `out.mp4`      | `out.mp4` (same extension, silent) |


Supported formats: `.mp4` / `.m4v` / `.mov` / `.mkv` / `.ts` / `.m2ts`.
`.webm` does not support HEVC and will be rejected outright. `.avi` technically works but playback is flaky, so you'll get a warning and the tool carries on.

> Want to force a different container? The simplest approach is to re-wrap losslessly after the tool finishes:
> `ffmpeg -i out.mkv -c copy out.mp4`

## APIs and hardware acceleration


| Stage                | API                                  | Default     | Can be switched                                                  |
| -------------------- | ------------------------------------ | ----------- | ---------------------------------------------------------------- |
| RTX Video SDK backend| **CUDA** (`rtx_video_api_cuda_`*)    | fixed       | The SDK also ships DX11 / DX12 / Vulkan — unused here; CUDA is cleanest for a windowless CLI tool |
| Video decode         | **NVDEC** (ffmpeg `-hwaccel cuda`)   | ✅ on       | `--no-hw-decode` falls back to software decoding                 |
| Video encode         | **NVENC**                            | ✅ on       | `--backend software` switches to CPU encoding                    |
| Output codec         | **HEVC (H.265)**                     | ✅ default  | `--codec h264` / `--codec av1`                                   |


### Output codec (`--codec`) cheat sheet


| `--codec`     | HDR10      | 10-bit | `--backend nvenc` (hardware)       | `--backend software` (software) |
| ------------- | ---------- | ------ | ---------------------------------- | ------------------------------- |
| `hevc` (default) | ✅      | ✅      | `hevc_nvenc` (RTX 20+ all cards)   | `libx265`                       |
| `h264`        | ❌ (rejected) | ❌   | `h264_nvenc`                       | `libx264`                       |
| `av1`         | ✅          | ✅     | `av1_nvenc` (**RTX 40+ Ada only**) | `libsvtav1`                     |


**Rules of thumb**:

- Everyday use / HDR needed → **`hevc`** (default): the balance of compatibility and efficiency
- Maximum compatibility (older devices / older players) → **`h264`**, but SDR-only (cannot be combined with `--hdr` / `--vsr-hdr`)
- Smallest files / RTX 40+ GPU on hand → **`av1`**, HDR supported

### Other notes

- **Input codec does not affect output**: whether the input is H.264, HEVC, AV1, VP9, or anything else NVDEC recognises, the tool always writes the target codec selected by `--codec`. The startup log prints "Input codec: xxx" for reference.
- **H.264 does not carry HDR**: the spec has some colourspace fields, but no mainstream player actually decodes H.264 as HDR, so the combination `--codec h264` + `--hdr` / `--vsr-hdr` is always rejected.
- **AV1 software fallback (CPU encoding)** uses `libsvtav1` (not `libaom-av1`); it's a lot faster.

> The **fully zero-copy pipeline** (NVDEC → CUDA → RTX SDK → CUDA → NVENC) is implemented in the `--gpu-only` path (on by default); frames stay resident in VRAM end-to-end. Use `--legacy` if you want to fall back to the older ffmpeg pixel-pipe pipeline for comparison.

## About ffmpeg

The tool **does not** statically link ffmpeg, because:

- libav* is LGPL / GPL, so bundling brings licensing headaches
- Statically linking would bloat the executable by 40+ MB

Current approach: `sdr2hdr.exe` **first looks for** `ffmpeg.exe` **/** `ffprobe.exe` **in its own directory, then falls back to PATH**.
The build script (`CMakeLists.txt`) auto-detects ffmpeg on the system and, if found, copies it alongside the output. That means the post-build `build\Release\` folder is **self-contained and distributable** — zip it and send it to someone; their machine needs no ffmpeg installation.

```
build/Release/
├── sdr2hdr.exe          ← the program itself (~100 KB)
├── ffmpeg.exe           ← encode / decode (~135 MB)
├── ffprobe.exe          ← probe input metadata (~135 MB)
├── nvngx_truehdr.dll    ← TrueHDR model
└── nvngx_vsr.dll        ← VSR model
```

> If ffmpeg is already installed on your system and you'd rather not bundle it, configure with `-DBUNDLE_FFMPEG=OFF`.
> If you'd like CMake to fail outright when ffmpeg is missing, use `-DBUNDLE_FFMPEG=ON`.

## Interface language

The tool ships both **English** and **Chinese** UIs (wizard and `--help`). By default it follows the Windows UI language; runtime status logs stay in English.

```powershell
.\sdr2hdr.exe --lang en     # force English
.\sdr2hdr.exe --lang zh     # force Chinese
.\sdr2hdr.exe --lang auto   # follow the system (default behaviour)

# You can also pin it via an environment variable:
$env:SDR2HDR_LANG = "en"
```

Precedence: `--lang` > `SDR2HDR_LANG` env var > OS UI language > English fallback.

## Build environment


| Software / hardware        | Version                                       |
| -------------------------- | --------------------------------------------- |
| Windows                    | 10 (20H1+) / 11                               |
| NVIDIA GPU                 | RTX series (GTX cards do not support TrueHDR / VSR) |
| NVIDIA driver              | **≥ r570.20** (hard requirement for the CUDA path) |
| Visual Studio              | 2019 (v142) or newer, Desktop C++ workload    |
| CUDA Toolkit               | **≥ 12.8**                                    |
| CMake                      | ≥ 3.18                                        |
| FFmpeg                     | any reasonably recent build, **libx265** required |
| env var `NV_RTX_VIDEO_SDK` | points to the SDK root (optional — the script can infer it) |


## Getting the dependencies (build from source)

This repository contains **only the source code**. The third-party NVIDIA SDKs and ffmpeg are **not** redistributed here (licence + size reasons), so you must obtain them yourself — all are free downloads:

1. **NVIDIA RTX Video SDK** — download from <https://developer.nvidia.com/rtx-video-sdk> (free NVIDIA developer account). It provides `include/`, `lib/Windows/x64/`, and `bin/Windows/x64/rel/nvngx_truehdr.dll` + `nvngx_vsr.dll`. Then either:
   - place this tool inside the SDK tree at `<SDK>/tools/sdr2hdr/` (the default layout the build infers automatically), **or**
   - set the env var `NV_RTX_VIDEO_SDK` to the SDK root (the folder containing `bin/ include/ lib/ samples/`).

2. **NVIDIA Video Codec SDK** — download from <https://developer.nvidia.com/video-codec-sdk>. Copy its `Interface/`, `Lib/`, and `Samples/` folders into `tools/sdr2hdr/vendor/nvcodec/` so the layout looks like:

```
tools/sdr2hdr/vendor/nvcodec/
├── Interface/                       (nvcuvid.h, nvEncodeAPI.h, cuviddec.h)
├── Lib/x64/                         (nvcuvid.lib, nvencodeapi.lib)
└── Samples/
    ├── NvCodec/NvDecoder/  NvCodec/NvEncoder/
    └── Utils/
```

3. **CUDA Toolkit ≥ 12.8** — <https://developer.nvidia.com/cuda-downloads>.

4. **FFmpeg** (a recent build **with `libx265`**) — e.g. from <https://www.gyan.dev/ffmpeg/builds/>. Put `ffmpeg.exe` / `ffprobe.exe` on `PATH`; the build script auto-detects and copies them next to the output (or use `-DBUNDLE_FFMPEG=OFF` to skip).

5. **Visual Studio 2019+** with the Desktop C++ workload, and **CMake ≥ 3.18**.

The build will print a clear `FATAL_ERROR` telling you exactly which of the above is missing if a path can't be resolved.

## Building

```powershell
cd tools\sdr2hdr
.\build.ps1                       # Release build + auto-bundle DLLs / ffmpeg
.\build.ps1 -Clean                # build from scratch
.\build.ps1 -Config Debug         # Debug build
```

## Usage

### 1) Interactive wizard

**Double-click `sdr2hdr.exe`**, or run it **with no arguments** from a terminal:

```powershell
.\sdr2hdr.exe
```

A five-step wizard runs:

```
[1/5] Input video files            # Three ways to pick files:
                                   #   1. [Recommended] open the Windows file picker
                                   #      (Ctrl/Shift + click for multi-select)
                                   #   2. manually paste / drag-drop paths
                                   #   3. drop files onto the exe icon (skips this step)
[2/5] What do you want to do?      # HDR / VSR / VSR+HDR
[3/5] Upscale target               # 1080p / 4K (default) / 8K / 2x / custom (VSR modes only)
[3b/5] HDR peak luminance          # 400 / 600 / 1000 / 2000 / custom (HDR modes only)
[4/5] Output codec                 # hevc / av1 / h264
[5/5] Output file / directory      # Three ways to set the output:
                                   #   1. use the suggested path
                                   #   2. open the Windows Save As / folder picker
                                   #   3. type a path manually
```

The wizard then prints a "Ready to process" summary — press **Y** to start, then **Enter** to close the window when it's done.

**Drag-and-drop scenario**: drop one or more video files **onto the `sdr2hdr.exe` icon** and the wizard starts with the file list pre-filled (Step 1 is skipped).

### 2) Single-file CLI

```powershell
.\sdr2hdr.exe in.mp4 out.mp4 --hdr
.\sdr2hdr.exe 1080p.mp4 4k.mp4 --vsr --4k
```

### 3) Batch CLI

Any positional argument is treated as an input; use `-o` to specify the output directory (**omit `-o` and outputs are written next to each input**):

```powershell
# Alongside each input: a.mp4 -> a_hdr.mp4, b.mp4 -> b_hdr.mp4, c.mp4 -> c_hdr.mp4
.\sdr2hdr.exe a.mp4 b.mp4 c.mp4 --hdr

# All outputs to one directory
.\sdr2hdr.exe clip1.mp4 clip2.mp4 -o D:\hdr_out --hdr

# Wildcard (expanded by PowerShell)
.\sdr2hdr.exe *.mkv -o D:\hdr_out --vsr-hdr --4k

# Force the wizard even when CLI args are present
.\sdr2hdr.exe *.mp4 --interactive
```

**Automatic naming convention** (batch mode, or single-file without an explicit output path):


| Mode                | Example output suffix  |
| ------------------- | ---------------------- |
| `--hdr`             | `input_hdr.mp4`        |
| `--vsr --4k`        | `input_2160p.mp4`      |
| `--vsr --scale 2`   | `input_2x.mp4`         |
| `--vsr-hdr --8k`    | `input_4320p_hdr.mp4`  |


A summary is printed when the batch finishes:

```
================ Batch complete ================
  3 succeeded, 0 failed, 142.3 s total
```

## Full usage

```
sdr2hdr.exe <input> <output> --<mode> [options]

Modes (exactly one):
  --hdr              SDR -> HDR (same resolution)
  --vsr              AI upscale (stays SDR)
  --vsr-hdr          upscale + SDR -> HDR

VSR options (apply to --vsr / --vsr-hdr):
  --vsr-quality N    1-4 (default 4, max quality; higher = better / slower)

  Output size -- pick ONE of these (default is --4k):
    --1080p          1080p output (height = 1080, width follows aspect ratio)
    --4k             4K output (height = 2160, default)
    --8k             8K output (height = 4320)
    --scale F        custom factor: output = input * F
    --output-size WxH explicit output dimensions

TrueHDR options (apply to --hdr / --vsr-hdr):
  --contrast N       0-200      (default 100)
  --saturation N     0-200      (default 100)
  --middle-grey N    10-100     (default 50)     (alias: --middle-gray)
  --max-lum N        400-2000 nits (default 1000)

Encoding / pipeline options:
  --codec C          output codec: 'hevc' (default) | 'h264' | 'av1'
                       hevc: broad compatibility, supports HDR10
                       h264: maximum compatibility, SDR-only (not valid with HDR modes)
                       av1 : best compression; NVENC AV1 needs RTX 40+
  --backend B        'nvenc' (hardware, default) or 'software'
                       nvenc    -> h264_nvenc / hevc_nvenc / av1_nvenc
                       software -> libx264 / libx265 / libsvtav1
  --quality N        quality factor (nvenc -cq / software -crf, lower is better)
                       defaults: nvenc=19, software=18
  --preset P         encoder preset:
                       . nvenc     : p1..p7
                       . libx264/5 : ultrafast..placebo
                       . libsvtav1 : numeric 0..13 (0 = slowest)
                       . generic names (fast/medium/slow) auto-map to each encoder's preset
  --no-hw-decode     disable NVDEC hardware decoding, use software decode instead
  --no-audio         do not copy the source audio
  --gpu-only         native in-VRAM pipeline (default, fastest)
  --legacy           fall back to the older pipeline: ffmpeg pixel pipes + host-pointer RTX
  --verbose          show ffmpeg's stderr (helps with debugging)
  --lang L           UI language: 'auto' (default) | 'en' | 'zh'
  -o PATH            explicit output path (a file for single input, a directory for multiple)
  -i, --interactive  start the wizard even when CLI args are present
  -h, --help         show help
```

### Typical invocations for each mode

```powershell
# 1) HDR conversion only
.\sdr2hdr.exe sample.mp4 sample_hdr.mp4 --hdr

# 1-1) High-quality HDR targeting a 1000-nit display (NVENC 'slow' preset)
.\sdr2hdr.exe sample.mp4 sample_hdr.mp4 --hdr --max-lum 1000 --quality 17 --preset slow

# 1-2) Ultimate quality via software x265
.\sdr2hdr.exe sample.mp4 sample_hdr.mp4 --hdr --backend software --quality 16 --preset slow

# 1-3) With an RTX 40+, use AV1 for HDR and smaller files
.\sdr2hdr.exe sample.mp4 sample_hdr.mkv --hdr --codec av1 --max-lum 1000

# 1-4) Legacy-player compatibility: H.264 output (SDR-only, so not compatible
#      with --hdr; example here is --vsr)
.\sdr2hdr.exe 720p_h264.mp4 1080p_h264.mp4 --vsr --1080p --codec h264

# 2) AI upscale to 4K (VSR stays SDR; default is already 4K + quality 4)
.\sdr2hdr.exe 1080p.mp4 4k.mp4 --vsr

# 2-1) Upscale to 8K
.\sdr2hdr.exe 4k.mp4 8k.mp4 --vsr --8k

# 2-2) Custom 2.5x scale factor
.\sdr2hdr.exe 720p.mp4 1800p.mp4 --vsr --scale 2.5

# 2-3) Ultrawide 2560x1080 with --4k yields 5120x2160 (21:9 aspect preserved)
.\sdr2hdr.exe widescreen.mp4 4k_wide.mp4 --vsr --4k

# 3) Upscale to 4K + HDR in one pass (--vsr-hdr also defaults to 4K + quality 4)
.\sdr2hdr.exe 1080p.mp4 4k_hdr.mp4 --vsr-hdr --max-lum 1000

# 3-1) Upscale to 8K HDR
.\sdr2hdr.exe 4k.mp4 8k_hdr.mp4 --vsr-hdr --8k
```

### Verifying the output really is HDR

```powershell
ffprobe -select_streams v:0 -show_streams sample_hdr.mp4 | Select-String "color_"
```

HDR modes should show:

```
color_space=bt2020nc
color_transfer=smpte2084
color_primaries=bt2020
```

SDR (VSR-only) mode should show:

```
color_space=bt709
color_transfer=bt709
color_primaries=bt709
```

HDR content **must be played on an HDR display with Windows HDR turned on** to render correctly. On an SDR panel the picture will look dark and washed out — that's expected.

## Distribution

Once the build finishes, **zip the entire `build\Release\` folder** (it already contains `sdr2hdr.exe`, `ffmpeg.exe`, `ffprobe.exe`, and the two `nvngx_*.dll` model files) and ship that. The recipient then:

1. Unzips it
2. **Double-clicks `sdr2hdr.exe`** → interactive wizard; or **drops videos onto the exe**; or
3. Runs from a terminal: `sdr2hdr.exe`

All the recipient needs is **an NVIDIA RTX GPU + a recent driver** — no ffmpeg install, no CUDA Toolkit, no Visual Studio.

> **Bundle ffmpeg in the release — don't rely on the auto-downloader.** Shipping `ffmpeg.exe` / `ffprobe.exe` inside the zip is the recommended path: the recipient is fully offline-capable, and it avoids the "download an exe at runtime, then run it" behaviour that some antivirus engines flag as suspicious. The built-in [auto-download](#get-it-without-building-prebuilt-release) is only a fallback for when those binaries are absent (it fetches over HTTPS and verifies the publisher's SHA-256 before use).

> **Unsigned executable / SmartScreen.** `sdr2hdr.exe` is **not code-signed**, so the first time someone runs a freshly downloaded copy, Windows SmartScreen may show *"Windows protected your PC"*. This is expected for unsigned tools — click **More info → Run anyway**. (It's not a virus warning; signing the binary with an Authenticode certificate would remove the prompt.)

## Q&A


| Problem / error                                                                | Fix                                                                                                               |
| ------------------------------------------------------------------------------ | ----------------------------------------------------------------------------------------------------------------- |
| `rtx_video_api_cuda_create failed`                                             | Driver < r570; upgrade to the latest Studio / Game Ready driver. Or `nvngx_*.dll` isn't next to the exe.          |
| `CUDA does not support the HDR format needed for TrueHDR`                      | Driver is too old.                                                                                                |
| `Failed to probe input video`                                                  | Neither the exe's directory nor PATH contains `ffprobe`.                                                          |
| ffmpeg reports `Impossible to convert between the formats...` / NVDEC decode fails | Input is an NVDEC-unsupported profile (e.g. some 10-bit H.264). Try `--no-hw-decode` to fall back to software decode. |
| `No NVENC capable devices found` / NVENC fails to initialise                   | GPU too old or driver too old. Use `--backend software` to fall back to CPU encoding.                             |
| `Error: --codec h264 is SDR-only`                                              | H.264 cannot carry HDR10. Use `--codec hevc` (default) or `--codec av1`.                                          |
| `av1_nvenc` fails to initialise when using `--codec av1`                       | NVENC AV1 only exists on RTX 40+ (Ada). On older cards use `--backend software` (which uses libsvtav1).           |
| Output looks washed out / dark (HDR mode)                                      | You need an HDR display with Windows HDR enabled to play it back correctly.                                       |
| VSR mode produces the same resolution                                          | Default is already 4K; for other sizes add `--1080p` / `--8k` / `--scale` / `--output-size`.                      |
| VSR output is slightly smaller than requested                                  | yuv420p chroma requires even width/height; the tool rounds down to the nearest even number.                       |
| `Conflicting VSR size options`                                                 | The size options (`--1080p` / `--4k` / `--8k` / `--scale` / `--output-size`) are mutually exclusive — pick one.   |
| Message "output container normalised to match input"                           | Informational: the output extension didn't match the input, so the tool realigned it to the input container.      |
| `Error: .webm does not support HEVC`                                           | WebM only supports VP8 / VP9 / AV1. Use `.mkv` or `.mp4` instead.                                                 |
| Audio track can't pass through `-c:a copy` in an MP4 container                 | Some audio codecs (FLAC, DTS, etc.) aren't allowed in MP4. Add `--no-audio` and remux audio separately later.     |
| `target height <= input height` warning with `--4k` / `--8k`                   | The input is already at or above the target. VSR is an upscaler — same-size or down-size makes no sense for it.  |
| Watermark in the lower-left corner                                             | `dev`-flavour DLLs ship with a debug watermark. For commercial use, ask NVIDIA for the `rel` flavour and an Application ID. |


## Roadmap

### 🎯 v1.1 — WinUI 3 Graphical Interface

A native Windows GUI built on WinUI 3 / Windows App SDK, replacing the console wizard for a more modern, visual workflow.

- [ ] **Project scaffold** — WinUI 3 C++/WinRT project with WinAppSDK, integrating the existing `sdr2hdr` engine as a static library
- [ ] **Main window** — drag-and-drop zone for video files, file picker button, recent-files list
- [ ] **Mode selector** — toggle bar for HDR / VSR / VSR+HDR with live parameter panels (peak luminance slider, upscale target picker, codec/quality/preset selectors)
- [ ] **Batch job queue** — data-grid view showing input file, status (pending / processing / done / failed), progress bar, ETA, fps
- [ ] **Real-time preview** — side-by-side SDR vs HDR thumbnail (periodic frame grab from the SDK)
- [ ] **Settings page** — default output directory, default codec/quality, ffmpeg/ngxs DLL path override, language (EN/ZH)
- [ ] **About / updates** — version info, link to GitHub releases, update-check on startup

### 🔧 v1.2 — Transcode Quality & Reliability

- [ ] **Fix audio-video desync** — see [Known Audio-Video Sync Issues](#known-audio-video-sync-issues) below; root cause is frame-rate drift between re-encoded video and copied audio
- [ ] **VFR (variable frame rate) support** — detect VFR sources and use `-vsync cfr` or per-frame PTS remapping in the muxer
- [ ] **Subtitle track passthrough** — `-map 0:s?` so ASS/SRT/PGS subs survive the remux
- [ ] **Chapter metadata copy** — `-map_metadata 0` to preserve chapters, titles, and tags
- [ ] **HDR10 static metadata SEI** — inject SMPTE 2086 Mastering Display + CEA-861.3 MaxCLL/MaxFALL into NVENC output for strict HDR10 compliance

### 🚀 v1.3 — Advanced Features

- [ ] **Multi-GPU support** — select which GPU to use when multiple RTX cards are present
- [ ] **Pause / resume / cancel** — interruptible processing with checkpoint state
- [ ] **Output preview in player** — "Open in player" button after completion (launches mpv/VLC with HDR flags)
- [ ] **Hardware requirements auto-check** — on startup, validate driver version, GPU capabilities, and SDK DLLs with actionable guidance
- [ ] **Logging & diagnostics** — structured log file per run, exportable for bug reports

### 💭 Future Ideas

- [ ] **Windows on ARM native build** — ARM64 native compilation targeting NVIDIA RTX Spark ARM processors; includes NEON/SVE2 optimised CUDA kernels where applicable, ARM64EC compatibility layer for x64 dependencies (ffmpeg, NVENC SDK), and native WinUI 3 ARM64 GUI
- [ ] Linux support (CUDA + RTX Video SDK via Wine/WSL2 or native port)
- [ ] macOS support (Metal-based HDR pipeline, no RTX SDK dependency)
- [ ] Plugin system for custom post-processing filters (denoise, sharpen, etc.)
- [ ] Distributed processing across multiple machines

---

## Known Audio-Video Sync Issues

The output video may have **audio-video desync** (audio drifts ahead or behind the video). This is a known issue with two root causes:

### Root Cause 1: Frame-rate mismatch in the GPU-only pipeline

In the GPU-only path, NVENC encodes at a fixed frame rate derived from the source's `r_frame_rate` (e.g. `24000/1001` for 23.976 fps). The muxer stamps timestamps using `-framerate num/den`. However:

- **VFR (variable frame rate) sources** — screen recordings, phone videos, and OBS captures often use VFR. The source's `r_frame_rate` is a *nominal* rate (e.g. 30/1), but actual frame durations vary. The muxer assumes CFR (constant frame rate), so audio and video gradually drift apart over the video's duration.
- **`avg_frame_rate` vs `r_frame_rate` rounding** — `ffprobe` reports `avg_frame_rate = 119997/1000` (from frame_count/duration) while `r_frame_rate = 120/1`. Using the wrong one introduces ~25 ppm drift, which accumulates to ~1 second desync over an 11-hour video.

### Root Cause 2: Re-encode timing vs audio copy

The audio is copied directly from the source (`-c:a copy`) with its original timestamps. The video is re-encoded with new timestamps from NVENC. If NVENC drops or duplicates frames (e.g. due to GPU load, thermal throttling, or pipeline back-pressure), the video's frame count diverges from the source, and the audio runs on the original timeline while video runs on the new one.

### Planned Fix

The v1.2 fix will:
1. **Extract per-frame PTS from the source** using `ffprobe -show_frames` or by parsing the container's timing metadata
2. **Inject matching PTS into the NVENC output** before passing to the muxer, so video timestamps stay synchronised with the original audio
3. **Add `-async 1`** to the muxer as a safety net for minor frame-count mismatches
4. **Detect VFR sources** and either convert to CFR at the muxer level or warn the user

### Workaround (current)

If you experience desync, try:
```powershell
# Force CFR conversion in the demuxer (helps with VFR sources)
.\sdr2hdr.exe in.mp4 out.mp4 --hdr --legacy

# Or re-sync audio after processing
ffmpeg -i out.mp4 -i in.mp4 -map 0:v -map 1:a -c copy -shortest out_synced.mp4
```

---

## Known limitations

- The CUDA hostptr path requires `pitch = 4 * width` and **cannot emit FP16**, so HDR output is fixed to 10-bit ABGR10 → HEVC Main10 (plenty for nearly all use cases).
- **Do not** run `--hdr` on footage that is already HDR — TrueHDR only accepts SDR input.
- Audio is currently passed straight through with `-c:a copy`. If the source uses an unusual codec, ffmpeg may not be able to mux it into mp4; add `--no-audio` and remux the audio with another tool afterwards.
