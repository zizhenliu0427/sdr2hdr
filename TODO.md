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

### 音画同步 & Mux（2026-06）

- [x] **#17** **VFR 输入音画同步**（PUBG Game DVR 等）
  - `probeVideo`：`r_frame_rate` vs `avg_frame_rate` 偏差 >5% 判 VFR，改用 avg fps
  - MKV/WebM 无 `nb_frames` 时 `count_packets` 取真实帧数
  - 帧数 ÷ 容器时长 vs 元数据 fps 偏差 >3% 时，改用实测 fps（修 SVP 125→120 fps 差 ~2 s 问题）
  - 测试：`2026.05.05 Death.DVR.mp4`（240→~107 fps，37 s 对齐）
- [x] **#18** **GPU-only 两阶段 mux**（所有带 `--copy-audio` 的输出）
  - 编码阶段写 raw Annex-B（`.sdr2hdr.vidonly.hevc` / `.h264`）
  - 收尾 `remuxCopyAudio()` 从源文件 copy 音频
  - 原因：Windows ffmpeg pipe 双输入不可靠（MP4 丢音轨 / MKV 音频堆 EOF）
- [x] **#19** **MKV 音频交错 remux**
  - raw ES + 音频一步 remux 会把 Opus 全堆在文件末尾（前 ~80% 无声、拖进度条才正常）
  - 修复：`remuxCopyAudio` 先 ES→video-only MKV，再 MKV+源→最终输出
  - 测试：`2026.04.12 SVP.mkv` → `--vsr-hdr --4k`
- [x] **#19b** `remuxCopyAudio` 支持 `--codec h264`（`-f h264` / `avc1` tag）
- [x] **#31** **输出自检**：mux 完成后 `verifyMuxOutput()` ffprobe 抽查
  - A/V `start_time` 差 >0.5 s → 报错（开头纯音频 / 纯画面）
  - 尾部 packet pts 差 >1 s → 报错（时长不对齐）
  - 第 2 个音频包 `pos` >50% 文件大小 → 报错（Opus 堆 EOF）
  - 接入 `remuxCopyAudio` 与 legacy `--copy-audio` 路径；失败时删除输出
- [x] **#35** **PUBG AV1 DVR 输入（GPU 主路径）**
  - Game DVR AV1/MP4：demux 改 **`-f ivf`** + `IvfStreamParser` 按帧喂 NVDEC（修 OBU chunk 切断 → error 999）
  - 输出默认 HEVC HDR；全 GPU ~127 fps @ 4K；5330 帧实测通过
- [x] **#48** **GUI 转换闪退根治：工作进程隔离（2026-06-11）**
  - **根因**（自写 ctypes 调试器抓栈实锤）：双 GPU 机器（5090 + AMD 核显）上,
    `NVSDK_NGX_CUDA_Init` → `GetLUIDFromCudaDevice`（NVIDIA 静态库内部）在
    混合显卡层 `nvcudart_hybrid64.dll` 介入时**栈溢出** → /GS fastfail 0xC0000409 闪退；
    纯控制台进程不走混合层所以从不崩。GpuPreference / NvOptimusEnablement /
    改 PE 子系统均无法规避——是驱动 bug
  - **修复**：GUI 队列改为**工作进程**跑转换（`sdr2hdr_cli.exe`，vcxproj 自动从
    build 目录拷贝）；stdout 解析进度（Frame x/y + 合并阶段），`--cancel-event`
    命名事件协作取消（取消后临时/半成品清理与进程内路径一致，已实测零残留）；
    worker 缺失时回退进程内转换
  - **附带收益**：今后任何引擎/驱动崩溃只表现为队列行 Failed + 错误信息，GUI 永不死；
    也为 #46 多进程分片铺好了进程模型
- [x] **#47** **进程生命周期与取消（2026-06-11）**
  - **kill-on-close Job Object**（`win32_utils launch()`，`CREATE_SUSPENDED`+assign）：
    主进程无论正常退出 / 被杀 / 崩溃，所有 ffmpeg/ffprobe 子进程由 OS 自动回收——
    修"关了 GUI 后台 ffmpeg 还在跑"
  - **转换中可取消**：`GpuPipelineOptions::cancelFlag` 接入主循环；GUI 队列里
    转换中的行 ✕ = 取消该任务（队列继续）；关窗口 = `JobQueue::shutdown()` 取消+有界等待
  - **垃圾清理**：取消/失败时删临时 video-only 文件与半成品输出；每次转换开始时
    扫除上次崩溃/强杀留下的同名陈旧临时文件；legacy 路径取消后误报成功的 bug 一并修复
  - **异常加固**：GPU 管线三个工作线程包 try/catch——NvDecoder/NvEncoder 全靠抛异常报错
    （GetLockedFrame/UnlockFrame/GetNextInputFrame 等），异常逃出 `std::thread` 就是
    `std::terminate` → 无声 0xC0000409 闪退（GUI 实测命中）；现在转成带错误信息的干净失败
- [x] **#32** **真 VFR PTS 透传**（2026-06-10，长视频音画同步根治）
  - **动机**：#17 的"平均 fps CFR"只保证首尾对齐；20 分钟 PUBG DVR 实测中段漂移
    **最大 ±10.6 s**（前半段实际 ~120 fps 被均匀化放慢，7 分钟处视频落后音频 10.6 s）。
    短片漂移累积不起来，所以当时测不出来
  - **采集**：`probeFramePts()` packet 级 ffprobe（demux-only，不解码，盘速 ~2 min/15 GB），
    与编码**并行**跑（`gpu_pipeline` 起线程），整数 pts + timebase 精确保留，排序=显示顺序
  - **应用**：`remuxCopyAudio` 第一步改 ES→video-only **MP4**，新模块 `mp4_retime.cpp`
    **原地重写 moov**（stts 逐帧时长 + mdhd/tkhd/mvhd/elst 时长，90 kHz timescale，
    moov 在文件尾所以不动 20 GB mdat）；第二步 `-c copy` 合并把逐帧时间戳带进最终 MP4/MKV
  - **起始偏移**：源视频流起点晚于文件起点时（MKV 防负 dts 偏移），合并时 `-itsoffset` 还原
  - **保险**：帧数不匹配 / 重复 pts / moov 形状异常 → 自动回退 CFR + 告警；`--no-vfr-pts` 强制关闭
  - **测试**：合成正弦 VFR 样片（±1.5 s 漂移），MP4 与 MKV 输出逐帧漂移均 **0.000 s**

---

## 待办

### 🔴 最高优先级 — WinUI 3 图形界面（v1.1）

> 替代控制台 wizard，作为下一阶段主要交付物。引擎层（`gpu_pipeline` 等）抽成静态库供 GUI 调用。

- [x] **#33a** **项目脚手架**
  - WinUI 3 C++/WinRT 工程 (`gui/sdr2hdr_gui.vcxproj`) + Windows App SDK NuGet
  - `sdr2hdr_core` 静态库；CLI 与 GUI 共用引擎
  - `build.ps1 -GUI` / CMake `-DBUILD_GUI=ON`
- [x] **#33b** **主窗口**
  - 拖放区 + 文件选择器 + 队列列表
- [x] **#33c** **模式与参数面板**
  - HDR / VSR / VSR+HDR；4K 目标、codec、quality、peak nits
- [x] **#33d** **批处理队列**
  - 后台线程 `processFile()` + DispatcherQueue UI 更新
- [~] **#33e** **实时预览** —— **已移除**。RTX Video SDK 的 GPU 管线（NVDEC→CUDA→NVENC）不向界面吐中间帧，且输出是 HDR，普通 `MediaPlayerElement` 无法正确显示，故"边转边预览成片"技术上做不到。原本的源视频回放面板用处不大，已删除，队列列表占满整行。
- [x] **#33f** **设置页**
  - 默认输出目录、语言 EN/ZH、主题（跟随系统/浅/深）、依赖检测面板（ffmpeg / nvngx，官方下载链接 + 重新检测）
- [x] **#33g** **关于**
  - 版本号、GitHub 链接、app.ico、ffmpeg / 英伟达 logo（随主题重着色 + 轮廓阴影）

### 🟢 GUI v1.1 已交付 + 增强（2026-06-10）

> WinUI 3 图形界面已完整落地，并在此基础上做了一批 UX / 工程增强。

- [x] **#37** **CLI 与 GUI 合并为单一二进制** `sdr2hdr.exe`
  - 无参双击 → 图形界面；带命令行参数 → 当 CLI 用（`wWinMain` 检测 `__argc>1` 转发到 `cliMain`）
  - GUI 内新增「命令行」页，可直接输入参数在控制台运行；不再单独产出 CLI 版
- [x] **#38** **GPU/CPU 编码器开关（GUI）**
  - 「编码器」下拉：GPU (NVENC) / CPU (Software)；AI 转换始终走 GPU，仅视频编码切换（CPU = 软件 x265/x264/AV1，走 `--backend software` 路径）
- [x] **#39** **队列预估时间 + fps**
  - 转换中显示 `45%  127.0 fps  ~2:13`（ETA = 剩余帧 ÷ 实测 fps）；1 Hz 刷新
  - 进度刷新改为**原地更新**行（不再整列重建），关掉入场动画 → 进度条不再上下跳
  - 每行右侧加 ✕ 可删除单个任务（正在转换的行禁用）
- [x] **#40** **音频合并阶段反馈**
  - 帧编码完成后进入两阶段 mux，状态显示「合并音频中… 0:0X」（已用时间，`-c copy` 无法可靠预测百分比），进度条与任务栏均切为 indeterminate 滚动动画
- [x] **#41** **HDR 输入提示**
  - `probeVideo` 增加 `color_transfer` / `color_primaries` 探测（`VideoInfo.isHdr`）；拖入已是 HDR（PQ/HLG 或 BT.2020 10bit）的视频时，界面顶部黄色 InfoBar 提示"再转一次可能发灰"并列出文件名
- [x] **#42** **子进程不再弹空白终端**
  - `win32_utils.cpp launch()` 加 `CREATE_NO_WINDOW`，GUI 调用 ffmpeg/ffprobe 不再弹控制台窗口
- [x] **#43** **任务栏实时进度 + 完成系统通知**
  - `ITaskbarList3` 任务栏进度条（finalize 阶段 indeterminate）；`AppNotificationManager` 完成 Toast
- [x] **#44** **主题 / i18n / 标题栏 / 最小尺寸**
  - Mica 背景、ExtendsContentIntoTitleBar + Tall caption、浅色模式 caption 按钮变深、`WM_GETMINMAXINFO` 最小尺寸、resize 缝隙主题填色、滚轮滚动、页面上浮非线性入场动画
  - 简体中文真正生效（终态 Done→完成 等渲染时本地化）；语言 Auto 跟随系统并显示当前系统语言
- [x] **#45** **转换崩溃修复（根因：CUDA 13.x 崩 RTX Video SDK v1.1.0 NGX）**
  - 三方工具链死锁：VS 2026 强制 CUDA ≥13.2 ↔ CUDA 13.x 让 `NVSDK_NGX_CUDA_Init` 栈溢出（0xC0000409）↔ RTX SDK v1.1.0 只兼容 CUDA 12.x
  - 修复：改用 **VS 2022 (v143) + CUDA 12.9**；`build.ps1` 自动优先 VS 2022、强制 CUDA 12.x（详见 `build.ps1` 注释）

### 功能完善

- [ ] **#7** 处理 VSR（输出分辨率不同于输入）时的缓冲区分配策略
  - 当前只在 HDR-only 路径下验证过，输入 / 输出同分辨率
  - 需要：分配独立的 `rgbaOut`（目标分辨率）、独立的 NVENC input buffer pool
  - 要点：RTX SDK 的 VSR 需要 `rtx_video_api_parameter_vsr_enabled_t` 打开
- [ ] **#9** 更新 README 说明新架构
  - GPU-only pipeline 数据流图
  - 与 legacy CPU 回退路径的性能对比
  - 命令行参数变化（`--gpu-only` 默认开启等）
  - **音画同步**：VFR 检测、两阶段 mux、支持的素材类型、临时文件说明

- [ ] **#29** **Legacy 管线（`--legacy`）音画同步**
  - 现状：`FFmpegEncoder` 仍用 pipe 同时喂像素帧 + copy 音频，MKV 可能有音频 EOF 堆叠
  - 选项 A：legacy 也改两阶段（编码→临时文件→remux）
  - 选项 B：文档标注 `--legacy` 仅诊断用，默认路径已修复

- [ ] **#30** **回归测试矩阵**（手动或脚本）
  - [ ] PUBG DVR VFR MP4（标称 240 fps，~107 fps 实测，H.264/HEVC）
  - [ ] **PUBG DVR AV1 MP4**（CFR 120 fps，如 `2026.04.12 … DVR.mp4`；5090 NVDEC）
  - [ ] SVP/OBS MKV（元数据 fps 虚高，如 125→120）
  - [ ] 真 CFR MP4（如 120 fps Game DVR）
  - [ ] `--vsr-hdr --4k` MKV 输出（PotPlayer + MPC-HC 从头播 + 拖进度条）
  - 每项检查：总时长对齐、从头有声音、进度条正常、无整段加速

- [ ] **#34** **输出帧率（`--output-fps`）**
  - CLI：`--output-fps auto|120|60|…`（默认 `auto` = 跟源，含 **#17** 校正后的实测 fps）
  - **仅 GPU 主路径**：编码循环内按时间戳 / 帧号跳帧，**不改变播放速度**（丢帧，非慢放）
  - NVENC + muxer 写入目标 fps；音频 copy 不变
  - 非整数比（如 250→120）用 pts 选帧，不做简单 `mod(n,N)`
  - 第一版：常用整数 fps（60 / 120）；已转好的文件用 README ffmpeg 示例，不做后处理子命令
  - WinUI：**#33c / #33f** 中增加 fps 下拉（依赖 GUI）

### 性能提升（按优先级 / ROI 递增）

- [~] **#36** **GPU 编码速度优化（总项）** —— **2026-06-10 调查结论：单进程已到顶**
  - **已做（2026-06-10）**
    - **NGX 直连层 `ngx_session.cpp`**：绕过 SDK 样例封装（其 deviceptr 路径每帧两次**同步**
      `cuMemcpy2D`，阻塞 CPU 并迫使整个 context 排空）；改为 `cuMemcpy2DAsync` + 全异步入队，
      帧按 session 条带化分流到独立 CUDA stream，CUDA event 与 NVENC IO stream 跨流同步
      （= #10 的工程化版本，且为多 session 预留好了架构）
    - **finalize I/O 重做**：编码时直接 pipe 进 video-only MP4（省掉 raw ES 落盘 + 单独容器化
      两遍全文件读写）；去掉 `+faststart`（它把 20+ GB 输出整个重写一遍，仅对 HTTP 流式播放有意义）
    - **SDR 输出补 BT.709 VUI**（顺手修复：新版 ffmpeg `-c copy` 忽略 `-color_*` 标志，
      容器级标签不可靠；HDR/SDR 现在都是码流级 VUI）
  - **实测**：4K TrueHDR ~121 fps —— 与旧路径持平 ⇒ **瓶颈就是 NGX TrueHDR 单实例推理
    （~8 ms/帧 @ 4K），管线已不是瓶颈**
  - **❌ #10 双 stream**：已实现（见上），但单实例下无增益——NGX 占满整帧时间，kernel 重叠救不了
  - **❌ #11 双 NGX session**：**被驱动硬拒**。第二个 TrueHDR 实例返回
    `NVSDK_NGX_Result_FAIL_FeatureAlreadyExists (0xbad00003)`；换独立 parameter 对象、
    换独立 CUDA context 都一样（驱动 610.47 / SDK v1.1.0）⇒ **每进程一个实例是硬限制**
  - **❌ #12 CUDA Graph**：前置依赖 #11，连带作废
  - **✅ 唯一可行的扩展路径：多进程分片（新 #46）**

- [ ] **#46** **多进程并行分片转换 —— v1.2 主打更新（已确认，2026-06-11）**
  - **为什么是唯一路径**：单进程已到 NGX TrueHDR 单实例硬上限（~125 fps @ 4K，
    驱动拒绝同进程第二实例，见 #36）；GPU 整体利用率仅 ~33%，余量全在
  - 方案：输入按时间切 N 段 → N 个 `sdr2hdr_cli.exe` 并行转换（各自独立的
    NGX / NVDEC / NVENC 实例）→ 各段 ES 拼接 → 一次 mux + 音频 copy + VFR PTS 透传
  - 每段开头自然是 IDR（编码器新起），HEVC ES 直接 concat 可行；VFR 透传按全局帧序映射
  - 预期：2 进程 ≈ 2×（~250 fps @ 4K）；**#48 的 worker 进程模型已就位，直接复用**
  - 风险：低（跨进程多 NGX 实例没问题，RTX HDR 叠加层与本工具共存已证明）

- [ ] **#49** **项目瘦身（v1.2，与 #46 同期）** —— 当前内容偏多，约定收缩范围：
  - **代码**：`--legacy` 旧管线评估删除（GPU 主路径已稳定且功能超集；删掉可同时关闭 #29）；
    随之可删 `FFmpegDecoder/FFmpegEncoder` 像素管道与 RtxConverter hostptr 路径
  - **TODO.md**：已完成项归档压缩（保留一行结论 + 关键数字，过程细节删除）；
    「Python 脚本融合」整节（#20–#22）移出主仓或砍掉
  - **README**：两份 README 当前 500+ 行,目标砍半——Q&A 表与「已知音画同步问题」
    合并精简,典型用法示例保留 1/3
  - 原则：GUI 是唯一交付物,CLI 仅作为内部 worker 与排障入口

- [x] **#10** kernel/NGX 分流到独立 stream + event 同步 —— 已实现于 `ngx_session` 直连层，
  单实例下无吞吐增益（见 #36 结论），架构保留供 #46 复用
- [x] ~~**#11** 双 NGX session~~ —— **不可行**，驱动返回 FeatureAlreadyExists（详见 #36）
- [-] ~~**#12** CUDA Graph 多路~~ —— 依赖 #11，作废

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
  - **联网调研结论（2026-06-09）**：NGX/RTX SDK EULA 原则上**允许**把"被文档标注为 distributable 的运行时文件"以 object code 形式随应用一起分发（Section 1.1/1.2、RTX Supplement 8.3）；项目已用 `rel/` 版（非 `dev/` 水印版），通常就是可分发的运行时版本。**两个前置条件**：① 在 RTX Video SDK 官方文档里确认 `nvngx_truehdr.dll` / `nvngx_vsr.dll` 明确列为 distributable；② 必须做 **NVIDIA 署名归属**（About 框里放 NVIDIA Marks / 商标，EULA Section 6.1b 要求）。仍建议本人对照 SDK 根目录 `EULA.txt` 复核。
  - 来源：NGX EULA https://docs.nvidia.com/rtx/ngx/ngx-eula/index.html
- [ ] **#27c** **Inno Setup 安装程序**（用户已确认要做，暂缓，记为 TODO）
  - 用 Inno Setup 写 `.iss`，把 `gui/x64/Release/` 整个文件夹打包成 `setup.exe`
    （已含 sdr2hdr.exe + 44 个 WinAppSDK 自包含运行库 + ffmpeg/ffprobe + nvngx_*.dll）
  - 装到 Program Files、开始菜单 + 桌面快捷方式（带 app.ico）、自带卸载
  - **ffmpeg**：GPL 构建——若打进安装包需附 GPL 协议文本 + 源码获取途径（gyan.dev / ffmpeg.org）；或保留"首次运行自动下载"以免自己承担分发义务
  - **nvngx**：仅在 #26 两个前置条件满足后才内置；否则安装完引导用户自取
  - WinAppSDK 运行时是自包含部署，无需单独装运行时
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
| VFR 整段加速 ~2.2× | 视频 ~17 s，音频 ~38 s | `r_frame_rate=240` 当 CFR，实际 avg ~107 fps | **#17** VFR 检测 + avg fps + 两阶段 mux |
| 元数据 fps 虚高差 ~2 s | SVP MKV 125 fps 声明，6606 帧实际 ~55 s | 帧数÷时长 vs 元数据偏差 4% | **#17** 实测 fps 校正（>3% 阈值） |
| MKV 前 80% 无声 / 拖进度条才正常 | PotPlayer 无音；MPC-HC 进度条卡住 | pipe 或 ES 一步 remux 把 Opus 堆在 EOF | **#18 #19** 两阶段 mux + ES→MKV→合并 |
| `--codec h264` remux 失败 | 两阶段 remux 硬编码 `-f hevc` | `remuxCopyAudio` 未区分编码 | **#19b** 按 codec 选 `.h264` / `-f h264` / `avc1` |
| mux 后无报错但播放异常 | 开头纯音频 / 拖进度条才有声 | 未做输出校验 | **#31** `verifyMuxOutput` 三检 + 失败删输出 |
| PUBG AV1 DVR GPU 路径失败 | `cuvidParseVideoData` 999 | OBU pipe + 任意 chunk 切断 AV1 访问单元 | **#35** IVF 按帧喂 NVDEC |
| 长 VFR 视频中段音画不同步（拖进度条明显） | 首尾对齐但 7 分钟处差 ~10 s | 平均 fps CFR 把局部快/慢段均匀化，漂移随时长累积 | **#32** 逐帧 PTS 透传（stts 重写） |

---

_最近更新（2026-06-10）：**v1.1 WinUI 3 GUI 完整交付**（#33a–g），并新增 **#37 CLI/GUI 合并**、
**#38 编码器开关**、**#39 队列 ETA**、**#40 音频阶段反馈**、**#41 HDR 输入提示**、
**#42 无终端弹窗**、**#43 任务栏进度/通知**、**#44 主题/i18n**、**#45 转换崩溃修复**。
版本状态：**v1.1（GUI）已完成**；**v1.2（转码可靠性）大部分完成**——音画同步 + VFR 平均 fps
（#17–#19、#31）+ **#32 真 VFR PTS 透传（长视频中段漂移根治）**已交付，
但**字幕直通 / 章节元数据 / `--output-fps` / HDR10 SEI / legacy 同步**仍未做。
下一步：v1.2 剩余项（字幕 / 章节 / `--output-fps`），以及 **#10/#36** 性能 profiling。_
