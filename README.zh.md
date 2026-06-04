# sdr2hdr — RTX Video SDK 命令行工具

> **语言 / Language**: **中文** · [English](README.md)

基于 英伟达 RTX Video SDK 的 AI 视频增强小工具，支持 RTX HDR（SDR 转 HDR）和 RTX VSR（超分辨率）。与浏览器或播放器中的实时滤镜不同，sdr2hdr 会处理并将增强后的视频导出，不受原本只能实时处理播放的限制。


| 模式          | 功能                                | 色域           | 分辨率        |
| ----------- | --------------------------------- | ------------ | ---------- |
| `--hdr`     | RTX HDR (**TrueHDR**)：SDR 画面转 HDR | HDR10        | 同输入        |
| `--vsr`     | **VSR**：AI 超分辨率                   | SDR (BT.709) | 放大（默认 4K） |
| `--vsr-hdr` | VSR **+** HDR                     | HDR10        | 放大（默认 4K） |


内部通过匿名管道串起 `ffmpeg(NVDEC) → sdr2hdr.exe (CUDA + RTX Video SDK) → ffmpeg(NVENC)`，帧级流水线，不落盘中间文件。

## 不编译直接用（下载预编译 Release）

如果你只想**用**这个工具，不关心源码：

1. 打开仓库的 **Releases** 页面，下载最新的 `sdr2hdr_vX.Y.zip`。
2. 解压到任意目录。
3. **双击 `sdr2hdr.exe`**（交互式向导），或**把视频拖到它上面**，或在终端里运行（见 [使用方式](#使用方式)）。

环境要求：**英伟达 RTX 显卡** + 较新驱动（**≥ r570.20**）。其它什么都不用装——不用 ffmpeg、不用 CUDA Toolkit、不用 Visual Studio。

> **压缩包里有什么。** 一个开箱即用的 Release 会打包 `ffmpeg.exe` / `ffprobe.exe`（第三方，LGPL/GPL）和英伟达的 `nvngx_truehdr.dll` / `nvngx_vsr.dll` 模型文件，所以下载即可运行。
>
> **版权说明。** 这些打包文件是第三方再分发内容：英伟达 NGX DLL 受 [RTX Video SDK 许可](https://developer.nvidia.com/rtx-video-sdk)约束（商业分发需要英伟达的 Application ID），ffmpeg 受 LGPL/GPL 约束。
>
> **如果某个 Release 只提供 `sdr2hdr.exe`：**
> - **ffmpeg / ffprobe** 会在首次运行时自动获取——如果在 exe 同目录和 `PATH` 上都找不到，工具会通过 HTTPS 下载一份预编译的静态 ffmpeg（约 90 MB，来自 gyan.dev），**校验官方 SHA-256** 后安装到自己所在目录。只需一次。可用环境变量 `SDR2HDR_NO_AUTODOWNLOAD` 关闭（然后自行安装 ffmpeg）。
> - **`nvngx_truehdr.dll` / `nvngx_vsr.dll`** 无法自动下载（受英伟达 SDK 登录 + EULA 限制）。缺失时工具会打印 [RTX Video SDK](https://developer.nvidia.com/rtx-video-sdk) 链接；请下载 SDK，把 `bin\Windows\x64\rel\nvngx_*.dll` 复制到 exe 同目录。

## 容器（封装格式）保持一致

工具会**自动把输出扩展名对齐到输入**，这样原始的音轨/字幕轨能顺利 `-c:a copy` 直通过去，不会遇到"输入是 FLAC 音频但 MP4 不认"之类的 muxer 报错。


| 输入       | 用户写的输出    | 实际输出                          |
| -------- | --------- | ----------------------------- |
| `in.mkv` | `out.mp4` | **`out.mkv`**（自动纠正，打印一条 Note） |
| `in.mp4` | `out.mov` | **`out.mp4`**                 |
| `in.ts`  | `out.mp4` | **`out.ts`**                  |
| `in.mp4` | `out.mp4` | `out.mp4`（同扩展名，静默）            |


支持的格式：`.mp4` / `.m4v` / `.mov` / `.mkv` / `.ts` / `.m2ts`。
`.webm` 不支持 HEVC，会直接拒绝。`.avi` 虽然技术上能用但播放体验差，会给警告但继续。

> 想强制换容器？最简单的办法：跑完工具后用 ffmpeg 无损重封装一次：
> `ffmpeg -i out.mkv -c copy out.mp4`

## API 与硬件加速


| 步骤               | API                               | 默认   | 可切换                                                 |
| ---------------- | --------------------------------- | ---- | --------------------------------------------------- |
| RTX Video SDK 后端 | **CUDA**（`rtx_video_api_cuda_`*）  | 固定   | SDK 另提供 DX11/DX12/Vulkan，本工具没用——CLI 无窗口环境下 CUDA 最简洁 |
| 视频解码             | **NVDEC**（ffmpeg `-hwaccel cuda`） | ✅ 开  | `--no-hw-decode` 回退软解                               |
| 视频编码             | **NVENC**                         | ✅ 开  | `--backend software` 切到 CPU 编码                      |
| 输出编码             | **HEVC（H.265）**                   | ✅ 默认 | `--codec h264` / `--codec av1`                      |


### 输出编码（`--codec`）对照


| `--codec`  | HDR10   | 10-bit | `--backend nvenc`（硬件）          | `--backend software`（软件） |
| ---------- | ------- | ------ | ------------------------------ | ------------------------ |
| `hevc`（默认） | ✅       | ✅      | `hevc_nvenc`（RTX 20+ 全系支持）     | `libx265`                |
| `h264`     | ❌（会被拒绝） | ❌      | `h264_nvenc`                   | `libx264`                |
| `av1`      | ✅       | ✅      | `av1_nvenc`（**仅 RTX 40+ Ada**） | `libsvtav1`              |


**经验法则**：

- 日常用 / 需要 HDR → **`hevc`**（默认），兼容性 × 效率的平衡
- 要最大兼容性（老设备/老播放器）→ **`h264`**，但只能 SDR（不能配 `--hdr` / `--vsr-hdr`）
- 追求最小文件体积 / 有 RTX 40 以上 GPU → **`av1`**，HDR 也支持

### 其它说明

- **输入编码不会影响输出**：输入是 H.264、HEVC、AV1、VP9 还是别的 NVDEC 认识的格式，工具都统一按 `--codec` 指定的目标编码来输出。启动日志里会打印 "Input codec: xxx" 供参考。
- **H.264 不支持 HDR**：规范上存在一些 colorspace 字段，但实际没有任何主流播放器会把 H.264 按 HDR 解码；因此一律拒绝 `--codec h264` + `--hdr`/`--vsr-hdr` 组合。
- **AV1 的软件回退 (CPU 渲染)**：用 `libsvtav1`（不是 `libaom-av1`），速度明显快得多。

> **完全零拷贝流水线**（NVDEC → CUDA → RTX SDK → CUDA → NVENC）已在 `--gpu-only` 路径下实现（默认开启），帧全程驻留显存。`--legacy` 可切回旧的 ffmpeg 像素管道作为对照。

## 关于 ffmpeg

工具**没有**把 ffmpeg 静态链接进来。因为：

- libav* 是 LGPL/GPL，打包要处理许可证问题
- 静态链进来可执行文件会大 40+ MB

当前方法：`sdr2hdr.exe` **先在自己所在目录找** `ffmpeg.exe` **/** `ffprobe.exe`**，找不到再回退到 PATH**。
构建脚本（`CMakeLists.txt`）会自动探测系统上的 ffmpeg，如果有就复制到输出目录一起打包。因此构建完成后的 `build\Release\` 文件夹是**自包含可分发的**，直接压缩丢给别人就能跑，对方机器上不需要装 ffmpeg。

```
build/Release/
├── sdr2hdr.exe          ← 程序本体 (~100 KB)
├── ffmpeg.exe           ← 编解码 (~135 MB)
├── ffprobe.exe          ← 探测输入信息 (~135 MB)
├── nvngx_truehdr.dll    ← TrueHDR 模型
└── nvngx_vsr.dll        ← VSR 模型
```

> 如果系统上已装了 ffmpeg 但不想打包，配置时加 `-DBUNDLE_FFMPEG=OFF`。
> 如果希望 CMake 在找不到 ffmpeg 时直接报错，加 `-DBUNDLE_FFMPEG=ON`。

## 界面语言

工具同时提供**英文**和**中文**界面（向导和 `--help`），默认按系统 UI 语言自动选择；运行时日志保持英文。

```powershell
.\sdr2hdr.exe --lang zh     # 强制中文
.\sdr2hdr.exe --lang en     # 强制英文
.\sdr2hdr.exe --lang auto   # 跟随系统（默认行为）

# 也可通过环境变量固定：
$env:SDR2HDR_LANG = "zh"
```

优先级：`--lang` > 环境变量 `SDR2HDR_LANG` > 系统 UI 语言 > 英文兜底。

## 编译环境


| 软/硬件                    | 版本                               |
| ----------------------- | -------------------------------- |
| Windows                 | 10 (20H1 以上) / 11                |
| 英伟达 GPU                 | RTX 系列（GTX 不支持 TrueHDR/VSR）      |
| 英伟达 驱动                  | **≥ r570.20**（CUDA 路径硬性要求）       |
| Visual Studio           | 2019 (v142) 或更新，Desktop C++ 工作负载 |
| CUDA Toolkit            | **≥ 12.8**                       |
| CMake                   | ≥ 3.18                           |
| FFmpeg                  | 任意较新版本，需带 **libx265**            |
| 环境变量 `NV_RTX_VIDEO_SDK` | 指向 SDK 根目录（可省略，脚本能自动推断）          |


## 获取依赖（从源码构建）

本仓库**只包含源代码**。第三方的英伟达 SDK 和 ffmpeg **没有**收录在内（许可 + 体积原因），需要你自己获取——全部是免费下载：

1. **英伟达 RTX Video SDK** —— 从 <https://developer.nvidia.com/rtx-video-sdk> 下载（需免费的英伟达开发者账号）。它提供 `include/`、`lib/Windows/x64/` 以及 `bin/Windows/x64/rel/nvngx_truehdr.dll` + `nvngx_vsr.dll`。然后二选一：
   - 把本工具放到 SDK 目录树里的 `<SDK>/tools/sdr2hdr/`（构建脚本默认会自动推断这个布局），**或**
   - 设置环境变量 `NV_RTX_VIDEO_SDK` 指向 SDK 根目录（即包含 `bin/ include/ lib/ samples/` 的那一层）。

2. **英伟达 Video Codec SDK** —— 从 <https://developer.nvidia.com/video-codec-sdk> 下载。把它的 `Interface/`、`Lib/`、`Samples/` 三个文件夹复制到 `tools/sdr2hdr/vendor/nvcodec/`，目录结构如下：

```
tools/sdr2hdr/vendor/nvcodec/
├── Interface/                       (nvcuvid.h, nvEncodeAPI.h, cuviddec.h)
├── Lib/x64/                         (nvcuvid.lib, nvencodeapi.lib)
└── Samples/
    ├── NvCodec/NvDecoder/  NvCodec/NvEncoder/
    └── Utils/
```

3. **CUDA Toolkit ≥ 12.8** —— <https://developer.nvidia.com/cuda-downloads>。

4. **FFmpeg**（较新版本，**需带 `libx265`**）—— 例如 <https://www.gyan.dev/ffmpeg/builds/>。把 `ffmpeg.exe` / `ffprobe.exe` 放到 `PATH` 上；构建脚本会自动探测并复制到输出目录（或用 `-DBUNDLE_FFMPEG=OFF` 跳过）。

5. **Visual Studio 2019+**（Desktop C++ 工作负载）以及 **CMake ≥ 3.18**。

如果某个路径找不到，构建会打印清晰的 `FATAL_ERROR`，明确告诉你上面哪一项缺失。

## 编译

```powershell
cd tools\sdr2hdr
.\build.ps1                       # Release 构建 + 自动打包 DLL/ffmpeg
.\build.ps1 -Clean                # 从头重新构建
.\build.ps1 -Config Debug         # Debug
```

## 使用方式

### 1) 交互式向导

直接**双击 `sdr2hdr.exe`**，或在命令行里**不带参数**运行：

```powershell
.\sdr2hdr.exe
```

会跑一个 5 步向导：

```
[1/5] Input video files            # 三种方式选文件：
                                   #   1. 【推荐】打开 Windows 文件选择器
                                   #      （Ctrl/Shift + 点击多选）
                                   #   2. 手动粘贴/拖拽路径
                                   #   3. 直接把文件拖到 exe 图标上（跳过这步）
[2/5] What do you want to do?      # HDR / VSR / VSR+HDR
[3/5] Upscale target               # 1080p / 4K（默认）/ 8K / 2× / 自定义（仅 VSR 模式）
[3b/5] HDR peak luminance          # 400 / 600 / 1000 / 2000 / 自定义（仅 HDR 模式）
[4/5] Output codec                 # hevc / av1 / h264
[5/5] Output file / directory      # 三种方式指定输出：
                                   #   1. 用自动推荐路径
                                   #   2. 打开 Windows Save As / 文件夹选择器
                                   #   3. 手动输入路径
```

最后打印 "Ready to process" 摘要，按 **Y** 开始跑；跑完按 **Enter** 关窗口。

**拖拽场景**：把一个或多个视频文件**拖到 `sdr2hdr.exe` 图标**上，会自动进入向导（文件已填好，跳过 Step 1）。

### 2) 单文件 CLI

```powershell
.\sdr2hdr.exe in.mp4 out.mp4 --hdr
.\sdr2hdr.exe 1080p.mp4 4k.mp4 --vsr --4k
```

### 3) 批量 CLI

所有位置参数都当作输入；用 `-o` 指定输出目录（**不指定则输出写在各自输入旁边**）：

```powershell
# 同目录输出：a.mp4 → a_hdr.mp4、b.mp4 → b_hdr.mp4、c.mp4 → c_hdr.mp4
.\sdr2hdr.exe a.mp4 b.mp4 c.mp4 --hdr

# 输出到指定目录
.\sdr2hdr.exe clip1.mp4 clip2.mp4 -o D:\hdr_out --hdr

# 通配符（由 PowerShell 展开）
.\sdr2hdr.exe *.mkv -o D:\hdr_out --vsr-hdr --4k

# 强制使用向导（即使命令行已经传参）
.\sdr2hdr.exe *.mp4 --interactive
```

**自动命名规则**（批量模式 / 单文件未显式指定输出路径）：


| 模式                | 输出后缀示例                |
| ----------------- | --------------------- |
| `--hdr`           | `input_hdr.mp4`       |
| `--vsr --4k`      | `input_2160p.mp4`     |
| `--vsr --scale 2` | `input_2x.mp4`        |
| `--vsr-hdr --8k`  | `input_4320p_hdr.mp4` |


跑完会打印一个汇总：

```
================ Batch complete ================
  3 succeeded, 0 failed, 142.3 s total
```

## 用法

```
sdr2hdr.exe <input> <output> --<mode> [options]

Modes (必须选一个):
  --hdr              SDR → HDR（分辨率不变）
  --vsr              AI 超分（保持 SDR）
  --vsr-hdr          超分 + SDR → HDR

VSR 选项（--vsr / --vsr-hdr 生效）:
  --vsr-quality N    1-4 (默认 4，数字越大质量越好/越慢)

  输出尺寸 —— 以下五个中选一个（不选则按 --4k 默认）:
    --1080p          输出 1080p（高 = 1080，宽按原比例，不挤压）
    --4k             输出 4K（高 = 2160，默认）
    --8k             输出 8K（高 = 4320）
    --scale F        自定义倍率：输出 = 输入 × F
    --output-size WxH 直接指定输出尺寸

TrueHDR 选项（--hdr / --vsr-hdr 生效）:
  --contrast N       0-200      (默认 100)
  --saturation N     0-200      (默认 100)
  --middle-grey N    10-100     (默认 50)    (别名: --middle-gray)
  --max-lum N        400-2000 nits (默认 1000)

编码/流水线选项:
  --codec C          输出编码：'hevc'（默认）| 'h264' | 'av1'
                       hevc: 兼容性好，支持 HDR10
                       h264: 最大兼容性，仅 SDR（不能配 HDR 模式）
                       av1 : 压缩最好；NVENC AV1 需要 RTX 40+
  --backend B        'nvenc'（硬件，默认）或 'software'
                       nvenc  -> h264_nvenc / hevc_nvenc / av1_nvenc
                       software -> libx264 / libx265 / libsvtav1
  --quality N        画质因子（nvenc 的 -cq 或软件的 -crf，越小越好）
                       默认：nvenc=19，software=18
  --preset P         编码器 preset：
                       · nvenc     : p1..p7
                       · libx264/5 : ultrafast..placebo
                       · libsvtav1 : 数字 0..13（0 最慢）
                       · 通用名（fast/medium/slow）会自动映射到各编码器的 preset
  --no-hw-decode     关闭 NVDEC 硬件解码，改走软件解码
  --no-audio         不复制原视频音轨
  --gpu-only         显存全程驻留的原生管线（默认开启，最快）
  --legacy           切回旧管线：ffmpeg 像素管道 + 主机指针 RTX
  --verbose          显示 ffmpeg 输出，便于排错
  --lang L           界面语言：'auto'（默认）| 'en' | 'zh'
  -o PATH            显式输出路径（单输入时是文件；多输入时是目录）
  -i, --interactive  即使有 CLI 参数也启动向导
  -h, --help         帮助
```

### 三种模式的典型用法

```powershell
# 1) 只做 HDR 转换
.\sdr2hdr.exe sample.mp4 sample_hdr.mp4 --hdr

# 1-1) 针对 1000 nit 显示器的高画质 HDR（NVENC slow 档）
.\sdr2hdr.exe sample.mp4 sample_hdr.mp4 --hdr --max-lum 1000 --quality 17 --preset slow

# 1-2) 追求极致画质时回退到软件 x265
.\sdr2hdr.exe sample.mp4 sample_hdr.mp4 --hdr --backend software --quality 16 --preset slow

# 1-3) 有 RTX 40 时用 AV1 做 HDR，文件更小
.\sdr2hdr.exe sample.mp4 sample_hdr.mkv --hdr --codec av1 --max-lum 1000

# 1-4) 老播放器兼容：H.264 输出（SDR-only，与 --hdr 不兼容，这里举的是 --vsr）
.\sdr2hdr.exe 720p_h264.mp4 1080p_h264.mp4 --vsr --1080p --codec h264

# 2) AI 超分到 4K（VSR 保持 SDR，默认即 4K + 质量 4）
.\sdr2hdr.exe 1080p.mp4 4k.mp4 --vsr

# 2-1) 超分到 8K
.\sdr2hdr.exe 4k.mp4 8k.mp4 --vsr --8k

# 2-2) 自定义 2.5× 倍率
.\sdr2hdr.exe 720p.mp4 1800p.mp4 --vsr --scale 2.5

# 2-3) 宽屏电影 2560×1080 → --4k 会得到 5120×2160（保持 21:9 宽高比）
.\sdr2hdr.exe widescreen.mp4 4k_wide.mp4 --vsr --4k

# 3) 超分到 4K + HDR，一次搞定（--vsr-hdr 默认也是 4K + 质量 4）
.\sdr2hdr.exe 1080p.mp4 4k_hdr.mp4 --vsr-hdr --max-lum 1000

# 3-1) 超分到 8K HDR
.\sdr2hdr.exe 4k.mp4 8k_hdr.mp4 --vsr-hdr --8k
```

### 验证输出是真的 HDR

```powershell
ffprobe -select_streams v:0 -show_streams sample_hdr.mp4 | Select-String "color_"
```

HDR 模式应看到：

```
color_space=bt2020nc
color_transfer=smpte2084
color_primaries=bt2020
```

SDR（VSR-only）模式应看到：

```
color_space=bt709
color_transfer=bt709
color_primaries=bt709
```

HDR 内容**必须在 HDR 显示器 + Windows HDR 打开**的环境下播放才能正确显示。在 SDR 屏上画面偏暗偏灰属正常。

## 分发

编译完成后**把整个 `build\Release\` 文件夹压成 zip**（里面已包含 `sdr2hdr.exe`、`ffmpeg.exe`、`ffprobe.exe` 和两个 `nvngx_*.dll` 模型文件）发出去即可，对方：

1. 解压
2. **双击 `sdr2hdr.exe`** → 进入交互式向导；或**把视频直接拖到 exe 上**；或
3. 命令行运行：`sdr2hdr.exe`

对方电脑上只要有 **英伟达 RTX GPU + 新驱动**就行，不用装 ffmpeg、不用装 CUDA Toolkit、不用装 VS。

> **Release 里直接打包 ffmpeg，不要依赖自动下载。** 把 `ffmpeg.exe` / `ffprobe.exe` 放进 zip 是推荐做法：对方完全可离线使用，也避免了「运行时下载一个 exe 再执行」这种被部分杀毒软件视为可疑的行为。内置的[自动下载](#不编译直接用下载预编译-release)只是这些文件缺失时的兜底（通过 HTTPS 下载，并在使用前校验官方 SHA-256）。

> **未签名程序 / SmartScreen。** `sdr2hdr.exe` **没有做代码签名**，所以别人首次运行刚下载的副本时，Windows SmartScreen 可能弹出 *"Windows 已保护你的电脑"*。这对未签名工具是正常现象——点 **更多信息 → 仍要运行** 即可。（这不是病毒警告；用 Authenticode 证书给程序签名即可消除该提示。）

## Q&A


| 问题/报错                                                                  | 排查                                                                          |
| ---------------------------------------------------------------------- | --------------------------------------------------------------------------- |
| `rtx_video_api_cuda_create failed`                                     | 驱动 < r570，升级到最新 Studio/Game Ready 驱动；或 `nvngx_*.dll` 不在 exe 同目录             |
| `CUDA does not support the HDR format needed for TrueHDR`              | 驱动太旧                                                                        |
| `Failed to probe input video`                                          | exe 同目录和 PATH 都找不到 ffprobe                                                  |
| ffmpeg 直接报 `Impossible to convert between the formats...` 或 NVDEC 解码失败 | 输入是 NVDEC 不认识的 profile（比如某些 10bit H.264），加 `--no-hw-decode` 退回软解            |
| `No NVENC capable devices found` / NVENC 初始化失败                         | GPU 太老或驱动太旧，加 `--backend software` 走 CPU 编码                                 |
| `Error: --codec h264 is SDR-only`                                      | H.264 没法承载 HDR10，用 `--codec hevc`（默认）或 `--codec av1`                        |
| `--codec av1` 时 `av1_nvenc` 初始化失败                                      | NVENC AV1 只在 RTX 40+（Ada）上有，老卡加 `--backend software`（走 libsvtav1）           |
| 输出画面偏灰/偏暗（HDR 模式）                                                      | 需要在 HDR 显示器 + Windows HDR 打开的环境下播放                                          |
| VSR 模式分辨率没变化                                                           | 默认已是 4K；如需其它尺寸加 `--1080p` / `--8k` / `--scale` / `--output-size`            |
| VSR 输出尺寸略小                                                             | 为 yuv420p 色度要求，宽高会被向下对齐到偶数                                                  |
| `Conflicting VSR size options`                                         | 几个尺寸选项（`--1080p` / `--4k` / `--8k` / `--scale` / `--output-size`）是互斥的，只能选一个 |
| 打印 `output container normalised to match input`                        | 正常信息：输出扩展名和输入不一致，已自动对齐到输入的容器                                                |
| `Error: .webm does not support HEVC`                                   | WebM 容器只支持 VP8/VP9/AV1，换成 `.mkv` 或 `.mp4`                                   |
| 音轨在 MP4 容器里无法直通 `-c:a copy`                                            | 某些音频编码（FLAC/DTS 等）MP4 不收，加 `--no-audio`，后续再用 ffmpeg 合轨                      |
| 用 `--4k` / `--8k` 时提示 "target height ≤ input height"                   | 输入本身已经 ≥ 目标分辨率，VSR 是放大模型，同尺寸/降尺寸用它没意义                                       |
| 左下角水印                                                                  | `dev` 版 DLL 自带调试水印，商用请联系 英伟达 获取 `rel` 版 + Application ID                    |


## 路线图 (Roadmap)

### 🎯 v1.1 — WinUI 3 图形界面

基于 WinUI 3 / Windows App SDK 的原生 Windows GUI，替代控制台向导，提供更现代、直观的操作体验。

- [ ] **项目脚手架** — WinUI 3 C++/WinRT 项目 + WinAppSDK，将现有 `sdr2hdr` 引擎作为静态库集成
- [ ] **主窗口** — 视频文件拖放区域、文件选择按钮、最近文件列表
- [ ] **模式选择器** — HDR / VSR / VSR+HDR 切换栏，配套实时参数面板（峰值亮度滑块、放大目标选择、编码/质量/预设选择器）
- [ ] **批处理队列** — 数据表格视图，显示输入文件、状态（等待/处理中/完成/失败）、进度条、预计剩余时间、fps
- [ ] **实时预览** — SDR vs HDR 缩略图并排对比（定期从 SDK 抓取帧）
- [ ] **设置页面** — 默认输出目录、默认编码/质量、ffmpeg/ngxs DLL 路径覆盖、语言（中/英）
- [ ] **关于 / 更新** — 版本信息、GitHub releases 链接、启动时检查更新

### 🔧 v1.2 — 转码质量与可靠性

- [ ] **修复音画不同步** — 参见下方 [已知音画同步问题](#已知音画同步问题)；根因是重编码视频与直通音频之间的帧率漂移
- [ ] **VFR（可变帧率）支持** — 检测 VFR 源，在 muxer 中使用 `-vsync cfr` 或逐帧 PTS 重映射
- [ ] **字幕轨直通** — `-map 0:s?` 让 ASS/SRT/PGS 字幕在重封装后保留
- [ ] **章节元数据复制** — `-map_metadata 0` 保留章节、标题和标签
- [ ] **HDR10 静态元数据 SEI** — 向 NVENC 输出注入 SMPTE 2086 Mastering Display + CEA-861.3 MaxCLL/MaxFALL，满足严格 HDR10 规范

### 🚀 v1.3 — 进阶功能

- [ ] **多 GPU 支持** — 多张 RTX 显卡时可选择使用哪张
- [ ] **暂停 / 恢复 / 取消** — 可中断的处理流程，支持断点状态
- [ ] **输出预览** — 完成后"在播放器中打开"按钮（调用 mpv/VLC 并带 HDR 参数）
- [ ] **硬件要求自动检测** — 启动时校验驱动版本、GPU 能力、SDK DLL，给出可操作的指引
- [ ] **日志与诊断** — 每次运行的结构化日志文件，可导出用于 bug 报告

### 💭 未来构想

- [ ] **Windows on ARM 原生构建** — 面向英伟达 RTX Spark ARM 处理器的 ARM64 原生编译；包含 NEON/SVE2 优化的 CUDA 内核、ARM64EC 兼容层处理 x64 依赖（ffmpeg、NVENC SDK）、以及原生 WinUI 3 ARM64 界面
- [ ] Linux 支持（CUDA + RTX Video SDK via Wine/WSL2 或原生移植）
- [ ] macOS 支持（Metal-based HDR 管线，不依赖 RTX SDK）
- [ ] 插件系统，支持自定义后处理滤镜（降噪、锐化等）
- [ ] 多机分布式处理

---

## 已知音画同步问题

输出视频可能出现**音画不同步**（音频超前或滞后于画面）。这是一个已知问题，有两个根因：

### 根因 1：GPU-only 管线中的帧率不匹配

在 GPU-only 路径中，NVENC 按源文件的 `r_frame_rate`（如 23.976fps 对应 `24000/1001`）推导出固定帧率进行编码。muxer 使用 `-framerate num/den` 打时间戳。然而：

- **VFR（可变帧率）源** — 屏幕录制、手机视频、OBS 录制常用 VFR。源的 `r_frame_rate` 是*标称*帧率（如 30/1），但实际帧持续时间不固定。muxer 假设 CFR（恒定帧率），导致音画随视频时长逐渐漂移。
- **`avg_frame_rate` vs `r_frame_rate` 的舍入误差** — `ffprobe` 报告 `avg_frame_rate = 119997/1000`（由帧数/时长推算），而 `r_frame_rate = 120/1`。用错一个会引入约 25 ppm 漂移，在 11 小时的视频上累积到约 1 秒的偏差。

### 根因 2：重编码计时 vs 音频直通

音频直接从源文件复制（`-c:a copy`），保持原始时间戳。视频用 NVENC 重新编码，产生新的时间戳。如果 NVENC 丢帧或重复帧（因 GPU 负载、温控降频或管线背压），视频帧数与源文件出现偏差，音频按原始时间线播放，视频按新时间线播放，二者逐渐错开。

### 计划修复方案

v1.2 将：
1. **从源文件提取逐帧 PTS** — 使用 `ffprobe -show_frames` 或解析容器的时间元数据
2. **向 NVENC 输出注入匹配的 PTS** — 在传给 muxer 之前，让视频时间戳与原始音频保持同步
3. **muxer 加 `-async 1`** — 作为轻微帧数不匹配的安全网
4. **检测 VFR 源** — 在 muxer 层面转为 CFR，或给用户警告

### 临时解决方案

如遇到音画不同步，可尝试：
```powershell
# 在 demuxer 中强制 CFR 转换（对 VFR 源有帮助）
.\sdr2hdr.exe in.mp4 out.mp4 --hdr --legacy

# 或处理后重新同步音频
ffmpeg -i out.mp4 -i in.mp4 -map 0:v -map 1:a -c copy -shortest out_synced.mp4
```

---

## 已知限制

- CUDA hostptr 路径要求 pitch = 4×width，**无法输出 FP16**，所以 HDR 输出固定为 10-bit ABGR10 → HEVC Main10（绝大多数场景够用）
- 已经是 HDR 的视频**不要**再跑 `--hdr`，TrueHDR 只接受 SDR 输入
- 音频目前是直接 `-c:a copy` 复制，如果源音频编码很特殊，ffmpeg 可能无法入 mp4，可加 `--no-audio` 后用其它工具合轨
