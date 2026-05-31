# sdr2hdr TODO

本文件追踪 `sdr2hdr` GPU-only pipeline 开发过程中的已完成与待办事项。

---

## 已完成

### 架构 & 基础设施

- [x] **#1** 设计 GPU-only pipeline 架构（数据流 / 线程 / 缓冲区模型）
- [x] **#2** 编写 demuxer / muxer 封装（`ffmpeg -c:v copy` 比特流管道）
- [x] **#3** Video Codec SDK vendor 目录 + CMake 改造
- [x] **#4a** 抽取 win32 共用工具（`win32_utils.h/.cpp`）

### 核心 GPU 管线

- [x] **#4b** 写 CUDA kernel（NV12 → RGBA8，ABGR10 → P010，RGBA8 → NV12）
- [x] **#4c** `RtxConverter` 增加 `initializeWithContext` / `convertFrameDevicePtr` 路径
- [x] **#6a** 编写 `gpu_pipeline.h/.cpp`：串联 NvDecoder → kernel → RTX SDK → kernel → NvEncoder
- [x] **#6b** `main.cpp` 接入 `--gpu-only` 开关，调用 `gpu_pipeline`

### 验证 & 调优

- [x] **#8** 编译 + 4K 测试 + 性能对比（已确认单 stream 架构上限 ~120 fps）
- [x] **#13** IDR 关键帧周期性抖动排查（GOP 2s → 5s）
- [x] **#14** NvDecoder 表面被覆盖造成的"每几秒跳帧"问题修复
  （`GetFrame` → `GetLockedFrame` + 延迟 `UnlockFrame`，滑动窗口 8 帧）

### 国际化

- [x] **#15** 中文 UI 支持（`i18n.h` + `tr(en, zh)` + 自动检测 OS UI 语言 +
  `--lang en|zh|auto` + `SDR2HDR_LANG` 环境变量覆盖；源文件用 `/utf-8` 编码）
- [x] **#16** 英式英语统一：`--middle-grey` 作主 flag，`--middle-gray` 保留别名；
  wizard 已用英式拼写（`Cancelled` / `normalised` / `colour`）

---

## 待办

### 功能完善

- [ ] **#7** 处理 VSR（输出分辨率不同于输入）时的缓冲区分配策略
  - 当前只在 HDR-only 路径下验证过，输入 / 输出同分辨率
  - 需要：分配独立的 `rgbaOut`（目标分辨率）、独立的 NVENC input buffer pool
  - 要点：RTX SDK 的 VSR 需要 `rtx_video_api_parameter_vsr_enabled_t` 打开
- [ ] **#9** 更新 README 说明新架构
  - GPU-only pipeline 数据流图
  - 与 legacy CPU 回退路径的性能对比
  - 命令行参数变化（`--gpu-only` 默认开启等）

- [ ] **#15** **VFR (变帧率) 输入的音画同步 bug** ⚠️ 已知 bug
  - **现象**：PUBG Game DVR 录的 mp4 (3840x2160, 标称 240 fps) 转换后视频明显加速 ~2.2×
  - **根因**：`probeVideo` 读取 `r_frame_rate` (nominal 最高 240) 而非 `avg_frame_rate` (实际 ~110)。
    PUBG DVR 是 VFR 容器，4085 帧实际跨 37 秒（平均 110 fps），但 sdr2hdr 当 CFR 240 处理 + 写入，
    muxer 用 `-framerate 240/1` → 4085 帧被声明为 17 秒 → 与原音频 37 秒错位
  - **快速修**（半小时）：`probeVideo` 加 VFR 检测，
    `abs(r_fr - avg_fr) / r_fr > 5%` 时改用 `avg_fr`；输出 CFR 110 fps
  - **中等修**（1 小时）：VFR 检测后 fallback 到 legacy pixel-pipe（ffmpeg 透传 PTS），性能损失 ~50%
  - **完整修**（半天-1 天，**推荐**）：GPU-only 流水线打通 PTS 透传：
    `NvDecoder::GetFrame()` 出来的 timestamp 串到 `NvEncoderCuda::EncodeFrame()`，
    muxer 改为 `-fps_mode passthrough` / `-vsync passthrough`，保留原始 VFR 时间戳
  - **测试样本**：`Z:\PLAYERUNKNOWN'S BATTLEGROUNDS\202605\20260505~20260506\PLAYERUNKNOWN'S BATTLEGROUNDS 2026.05.05 - 22.20.06.11.Death.DVR.mp4`

### 性能提升（按优先级 / ROI 递增）

- [ ] **#10 · 方案 A** NV12 → RGBA / RGBA → P010 两个 kernel 搬到独立 stream
  - **预期收益**：+15% ~ +25%（当前 ~125 fps → ~150 fps @ 4K TrueHDR）
  - **方法**：kernelStream 与 ngxStream 分离，CUDA event 做跨流同步
  - **风险**：低；NVDEC/NVENC 已经用 `SetIOCudaStreams` 接到 ngxStream

- [ ] **#11 · 方案 B** 双 NGX session + 双 rgbaIn / rgbaOut + 双 stream
  - **预期收益**：+60% ~ +90%（目标 200+ fps @ 4K TrueHDR）
  - **方法**：创建两个 RTX Video SDK 实例，偶 / 奇帧分别送两条独立流水线
  - **风险**：中；需要验证 RTX SDK 是否支持同进程多 session 并发（SDK v1.1.0 文档未明确）
  - **显存占用**：约翻倍（两套 RGBA 中间 buffer）

- [ ] **#12 · 方案 C** N 路并行 + CUDA Graph
  - **预期收益**：打满 RTX SDK 硬件极限，目标 240 fps @ 4K TrueHDR
  - **方法**：用 CUDA Graph 把"kernel → NGX → kernel"封为一个可重放图，
    N 路（典型 N=4）并行发射，调度器自动填满硬件
  - **风险**：高；NGX 是否能以 stream-captured 方式进 graph 需实验
  - **前置依赖**：方案 B 走通后再做

### Python 脚本融合 / 多工具入口

**待落地策略**：3 个方案待用户选定，详见本节末注释

- [ ] **#20** 设计 wizard 新增 `[0b] What do you want to do?` 工具选择菜单
  - 选项 1-3：现有 TrueHDR / VSR / VSR+HDR（走 RTX SDK 流水线）
  - 选项 4+：调用其它工具（fix_sdr_tags 等）
  - 共享文件选择、批处理、进度显示、i18n 等 wizard 基础设施
  - 仅在方案 1 / 方案 3 下需要做

#### 脚本清单与端口评估

| ID | 脚本 | 功能 | 相关度 | 端口工作量 | 端口路线 |
|---|---|---|---|---|---|
| **#21** | `py/fix_sdr_tags.py` | 修复假 HDR 标签（容器 metadata + 可选 bsf：hevc_metadata / h264_metadata / av1_metadata / vp9_metadata / mpeg2_metadata） | **强相关** | ~150 行 C++ | 端口为 C++ 子命令，复用 `runShell()` / `probeVideo()` |
| **#22a** | `py/Document.py` | 按文件名时间戳归档到"游戏日"目录（中午 12 点分界） | 弱相关（PUBG 工作流） | ~100 行 C++ `std::regex` + `std::filesystem` | 待方案选定 |
| **#22b** | `py/move_single_video_out.py` | 把"子目录里只有一个视频"的情况把视频提到上一层，删空目录 | 弱相关 | ~80 行 C++ | 待方案选定 |
| **#22c** | `py/shorten_names_keep_ext.py` | 截断超长文件名（CJK weighting = 3，保留扩展名，重名加数字后缀） | 弱相关 | ~150 行 C++（CJK 区间表要搬过来） | 待方案选定 |
| **#22d** | `py/New Text Document.py` | PDF → `.7z` 批量改名 | **无关** | trivial | **不接入**（跟视频/HDR 无关，建议本地保留） |

#### 三个落地方案（待用户选定）

- **方案 1（聚焦）**：只把 #21（fix_sdr_tags）端口为 C++ 进 sdr2hdr。#22a/b/c 另开独立小 repo
  （如 `pubg-clip-organiser`），#22d 不接入。
- **方案 2（保留 Python）**：sdr2hdr 主仓加 `tools/sdr2hdr/scripts/`，原样保留 #21 / #22a/b/c 共 4 个
  Python 脚本，README 加 "Companion scripts" 章节。wizard **不**集成。#22d 不接入。
- **方案 3（全 C++）**：#21 + #22a/b/c 全部端口为 C++ 子命令，wizard 增加工具选择菜单。#22d 不接入。

> 推荐方案 1：sdr2hdr 主项目定位清晰（NVIDIA RTX Video SDK），简历呈现也聚焦；
> `fix_sdr_tags` 进主仓后 "假 HDR → 修标签 → 升 HDR" 一条流水线可在 sdr2hdr 内跑完。

### 开源发布准备

- [ ] **#23** 新增 `.gitignore`
  - 排除：`build/`, `vendor/`, `bin/`, `doc/`, `samples/RTX_Video_API/`, `*.dll`
  - 排除：个人简历 (`Video Codec Resume Zizhen Liu.*`, `resume-zh_CN *.md`, `AI数据 Resume *.docx`)
- [ ] **#24** 添加 `LICENSE` 文件
  - 推荐 MIT（最宽松）或 Apache 2.0（带专利防御条款）
  - 待用户选定
- [ ] **#25** 写 `SETUP.md` 指引
  - 用户克隆后需要：
    1. 从 NVIDIA 官网下载 RTX Video SDK v1.1.0，把头文件 + DLL 放到 `samples/RTX_Video_API/` 与 `bin/Windows/x64/rel/`
    2. 从 NVIDIA 官网下载 Video Codec SDK v13.0.x，放到 `tools/sdr2hdr/vendor/nvcodec/`
    3. 安装 ffmpeg（或下载预编译 build），路径写进 CMake 配置
    4. 跑 `build.ps1`
- [ ] **#26** 确认 NVIDIA RTX Video SDK EULA 是否允许重分发 `nvngx_truehdr.dll` / `nvngx_vsr.dll`
  - 决定 GitHub Releases 包能不能内置 DLL（影响开箱即用体验）
  - 待用户查 SDK 根目录的 `EULA.txt` / `LICENSE.pdf`
- [ ] **#27** 建 GitHub Releases 发布流程
  - 打包 zip：`sdr2hdr.exe` + ffmpeg/ffprobe + (允许的话) NVNGX DLL + README + LICENSE
  - 用 git tag (`v1.0` 等) 触发，手动在 GitHub Releases UI 上传
- [ ] **#28**（可选）GitHub Actions CI 自动 build + release
  - Windows runner，NVIDIA SDK 文件通过 GitHub Secrets 或 self-hosted runner 提供
  - 触发：push tag 时自动 build → 打包 → 创建 Release

---

## 本项目已解决过的关键问题（备忘）

| 问题 | 现象 | 根因 | 修复 |
|---|---|---|---|
| FFmpeg muxer 拒绝选项 | `Unrecognized option 'master_display'` | 这些是 `libx265` 私有选项，`-c:v copy` 不认 | 从 muxer 命令行移除，改由 NVENC VUI + mp4 颜色 tag 承载 HDR10 |
| 输出视频带水印 | 左下角 "RTX Video SDK - DO NOT DISTRIBUTE" | 拷贝的是 `dev/nvngx_*.dll` | CMake 改拷 `rel/nvngx_*.dll` |
| 输出时长极长（2328:00:00） | Explorer 显示 0.10 fps | VUI `timingInfoPresentFlag` 与 mp4 muxer 的 `-c:v copy` 路径冲突 | 关 VUI timing，只靠 `-framerate num/den` |
| 处理感觉卡（非丢帧） | 播放微抖 | B-frame + Annex-B pipe 的 PTS/DTS 重排不稳 | `frameIntervalP = 1`（IPPP，禁 B 帧） |
| 每几秒跳帧 | 原文件流畅，转换后每几秒跳一下 | `GetFrame` 的 surface 可被后续 `Decode` 覆盖 | `GetLockedFrame` + 8 帧滑动窗口延迟 `UnlockFrame` |
| GPU 利用率低 | NVENC 33%，NVDEC 13%，3D 14% | 单 CUDA stream 串行 kernel → NGX → kernel | 见 #10 / #11 / #12（未来方案） |
| 10-bit HEVC 输入卡死 + 22 GB 临时文件 | Auto-HDR Game DVR 录的 P010 文件，进度走完但卡在 muxer 收尾 | NvDecoder 输出 P010 表面（2 字节/样本），`launchNv12ToRgba8` 当成 8-bit NV12 读 → 噪声 → NVENC 编不下来 | `probeVideo` 加 `pix_fmt`/`bitDepth` 探测，10-bit 输入走 `launchP010ToRgba8`；12-bit 早退报错 |
| WebM 输入批处理失败 | 单文件 `Failed to start BitstreamMuxer` | `.webm` 容器只允许 VP8/VP9/AV1，不能装 HEVC/H.264 | `processOne` 自动把输出扩展名改写为 `.mp4`（除非编码是 AV1） |
| VP9 输入解码失败 | `Resolution: 21249x1; Resolution not supported on this GPU` | demuxer 把 VP9 piping 成 `-f ivf` 容器流，NvDecoder 当成裸 VP9 packet 解析 IVF 头字节 | 输入编码 ∉ {h264, hevc, av1} 时自动 fallback 到 legacy ffmpeg 路径 |
| MKV / WebM 进度条显示 `~0 frames` | 处理本身正常，仅 banner 难看 | ffprobe `stream=nb_frames` 只有 MP4 系列容器才有 | `probeVideo` 再查 `format=duration` 和 `stream_tags=DURATION`，按 `duration × fps` 估算帧数 |
| 输出码率明显低于源（58 vs 141 Mbps） | "为什么文件变小了" | NVENC `-cq 19` 是 "视觉无损" 偏紧档；Game DVR 原码率本身就过剩 | 新增 `--quality auto`：按源码率 + 分辨率 + 帧率自适应 CQP；wizard `[4b/5]` 步骤展示各档预估码率 |

---

_最近更新：加入 10-bit P010 输入支持、`--quality auto` 自适应 CQP、wizard 语言选择步骤、批处理时
webm 容器和 VP9 输入的自动 fallback、MKV / WebM 用 duration × fps 估算 frameCount。
下一步：用户决定 Python 脚本接入路线 + GitHub 发布前的清理（LICENSE / .gitignore / SETUP.md）。_
