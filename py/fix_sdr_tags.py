import subprocess
from pathlib import Path

VIDEO_EXTS = {
    ".mp4", ".mkv", ".mov", ".m4v",
    ".ts", ".m2ts", ".avi", ".webm"
}

RECURSIVE = False
SKIP_EXISTING = True
OUTPUT_DIR = Path("SDR_Fixed")


def choose_mode():
    print("请选择处理模式：")
    print("1 = 只改容器/流 metadata")
    print("2 = 改 metadata + 根据编码自动修改 bitstream metadata")
    print()

    while True:
        choice = input("请输入 1 或 2：").strip()

        if choice == "1":
            return 1
        if choice == "2":
            return 2

        print("输入无效，请输入 1 或 2。")


def run_command(cmd):
    print("\n运行命令：")
    print(" ".join(f'"{c}"' if " " in c else c for c in cmd))
    result = subprocess.run(cmd)
    return result.returncode


def run_command_capture(cmd):
    result = subprocess.run(
        cmd,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        encoding="utf-8",
        errors="ignore"
    )
    return result.returncode, result.stdout.strip(), result.stderr.strip()


def check_tools(mode):
    required_tools = ["ffmpeg"]

    if mode == 2:
        required_tools.append("ffprobe")

    for tool in required_tools:
        try:
            subprocess.run(
                [tool, "-version"],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                check=True
            )
        except Exception:
            print(f"错误：找不到 {tool}。")
            print("请先安装 ffmpeg，并确保 ffmpeg / ffprobe 已加入系统 PATH。")
            raise SystemExit(1)


def get_video_files():
    if RECURSIVE:
        return [
            p for p in Path(".").rglob("*")
            if p.is_file()
            and p.suffix.lower() in VIDEO_EXTS
            and OUTPUT_DIR not in p.parents
        ]

    return [
        p for p in Path(".").iterdir()
        if p.is_file()
        and p.suffix.lower() in VIDEO_EXTS
    ]


def build_output_path(input_path: Path):
    if RECURSIVE:
        output_folder = OUTPUT_DIR / input_path.parent
    else:
        output_folder = OUTPUT_DIR

    output_folder.mkdir(parents=True, exist_ok=True)

    # 保留原文件名，只放到 SDR_Fixed 下
    return output_folder / input_path.name


def get_video_codec(input_path: Path):
    cmd = [
        "ffprobe",
        "-v", "error",
        "-select_streams", "v:0",
        "-show_entries", "stream=codec_name",
        "-of", "default=noprint_wrappers=1:nokey=1",
        str(input_path)
    ]

    returncode, stdout, stderr = run_command_capture(cmd)

    if returncode != 0 or not stdout:
        print(f"警告：无法检测编码：{input_path}")
        if stderr:
            print(stderr[-1000:])
        return None

    return stdout.strip().lower()


def build_bitstream_filter(codec: str):
    """
    返回对应编码的 bitstream filter。
    目标：尽量把编码流内部色彩标记改成 BT.709 / SDR。
    """

    if codec in {"hevc", "h265", "h.265", "hvc1"}:
        return (
            "hevc_metadata",
            "hevc_metadata=colour_primaries=1:transfer_characteristics=1:matrix_coefficients=1"
        )

    if codec in {"h264", "avc", "avc1"}:
        return (
            "h264_metadata",
            "h264_metadata=colour_primaries=1:transfer_characteristics=1:matrix_coefficients=1"
        )

    if codec == "av1":
        return (
            "av1_metadata",
            "av1_metadata=color_primaries=1:transfer_characteristics=1:matrix_coefficients=1"
        )

    if codec == "vp9":
        return (
            "vp9_metadata",
            "vp9_metadata=color_space=bt709:color_range=tv"
        )

    if codec in {"mpeg2video", "mpeg2"}:
        return (
            "mpeg2_metadata",
            "mpeg2_metadata=colour_primaries=1:transfer_characteristics=1:matrix_coefficients=1"
        )

    # VP8 / ProRes / Theora / MPEG-4 Part 2 / WMV 等没有这里适合的通用 SDR bitstream metadata filter
    return None, None


def fix_sdr_tag(input_path: Path, output_path: Path, mode: int):
    cmd = [
        "ffmpeg",
        "-hide_banner",
        "-stats",
        "-stats_period", "1",
        "-y",
        "-i", str(input_path),
        "-map", "0",
        "-c", "copy",

        # 容器/输出流层面的 SDR / BT.709 标记
        "-color_primaries", "bt709",
        "-color_trc", "bt709",
        "-colorspace", "bt709",
    ]

    if mode == 1:
        print("模式 1：只修改容器/流 metadata，不修改 bitstream。")

    elif mode == 2:
        codec = get_video_codec(input_path)
        print(f"检测到视频编码：{codec if codec else '未知'}")

        bsf_name, bsf_value = build_bitstream_filter(codec)

        if bsf_value:
            print(f"模式 2：启用 {bsf_name}。")
            cmd += ["-bsf:v", bsf_value]
        else:
            print("模式 2：当前编码没有合适的自动 bitstream filter，只改普通 metadata。")

    cmd.append(str(output_path))

    return run_command(cmd)


def main():
    mode = choose_mode()
    check_tools(mode)

    OUTPUT_DIR.mkdir(exist_ok=True)

    files = get_video_files()

    if not files:
        print("当前目录没有找到视频文件。")
        return

    print()
    print(f"找到 {len(files)} 个视频文件。")
    print(f"输出目录：{OUTPUT_DIR.resolve()}")

    if mode == 1:
        print("当前模式：1 = 只修改容器/流 metadata")
    else:
        print("当前模式：2 = metadata + 自动 bitstream metadata 修复")

    success = 0
    failed = 0
    skipped = 0

    for index, input_path in enumerate(files, start=1):
        output_path = build_output_path(input_path)

        print("\n" + "=" * 80)

        if SKIP_EXISTING and output_path.exists() and output_path.stat().st_size > 0:
            print(f"[{index}/{len(files)}] 跳过，输出文件已存在：{output_path}")
            skipped += 1
            continue

        print(f"[{index}/{len(files)}] 正在处理：{input_path}")

        returncode = fix_sdr_tag(input_path, output_path, mode)

        if returncode == 0:
            print(f"完成：{output_path}")
            success += 1
        else:
            print(f"失败：{input_path}")
            failed += 1

            if output_path.exists():
                try:
                    output_path.unlink()
                    print("已删除未完成的输出文件。")
                except Exception:
                    print("警告：未能删除未完成的输出文件。")

    print("\n" + "=" * 80)
    print("处理完成")
    print(f"成功：{success}")
    print(f"跳过：{skipped}")
    print(f"失败：{failed}")
    print(f"输出目录：{OUTPUT_DIR.resolve()}")

    if mode == 1:
        print("\n如果 PotPlayer 仍然显示 HDR，可以删除 SDR_Fixed 里的输出文件后重新运行，并选择模式 2。")
    else:
        print("\n如果 PotPlayer 仍然显示 HDR，可能还有 Mastering Display / MaxCLL 等 HDR SEI 残留，可能需要重编码或更专门的 SEI 清理。")


if __name__ == "__main__":
    main()