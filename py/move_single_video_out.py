from pathlib import Path
import shutil
import sys

# 常见视频扩展名，匹配时会统一转小写，所以大小写不敏感
VIDEO_EXTS = {
    ".mp4", ".mkv", ".avi", ".mov", ".wmv",
    ".flv", ".m4v", ".ts", ".mts", ".m2ts",
    ".webm", ".mpg", ".mpeg", ".vob", ".rm",
    ".rmvb", ".3gp", ".3g2", ".asf", ".divx",
    ".f4v", ".ogv", ".ogm", ".qt", ".tod",
    ".tp", ".trp", ".mxf", ".dv", ".dat"
}


def is_video_file(path: Path) -> bool:
    # suffix.lower() 已经保证大小写不敏感
    return path.is_file() and path.suffix.lower() in VIDEO_EXTS


def get_unique_target_path(target_dir: Path, original_name: str) -> Path:
    """
    如果目标目录里已有同名文件，则自动生成:
    name(1).ext, name(2).ext ...
    """
    candidate = target_dir / original_name
    if not candidate.exists():
        return candidate

    original_path = Path(original_name)
    stem = original_path.stem
    suffix = original_path.suffix

    index = 1
    while True:
        new_name = f"{stem}({index}){suffix}"
        candidate = target_dir / new_name
        if not candidate.exists():
            return candidate
        index += 1


def main():
    # 默认处理当前目录；也可以命令行传路径
    if len(sys.argv) > 1:
        root = Path(sys.argv[1]).resolve()
    else:
        root = Path.cwd()

    if not root.exists() or not root.is_dir():
        print(f"目录不存在或不是文件夹: {root}")
        return

    print(f"开始处理目录: {root}")
    print("-" * 60)

    moved_count = 0
    skipped_count = 0

    # 只遍历一级子文件夹
    for subdir in root.iterdir():
        if not subdir.is_dir():
            continue

        # 只看子文件夹当前层，不递归
        video_files = [p for p in subdir.iterdir() if is_video_file(p)]

        if len(video_files) == 1:
            src_video = video_files[0]
            dst_video = get_unique_target_path(root, src_video.name)

            try:
                print(f"[移动] {src_video} -> {dst_video}")
                shutil.move(str(src_video), str(dst_video))

                # 只有空文件夹才删除，避免误删其他内容
                try:
                    subdir.rmdir()
                    print(f"[删除空文件夹] {subdir}")
                except OSError:
                    print(f"[保留文件夹] {subdir} 不是空的，未删除")

                moved_count += 1

            except Exception as e:
                print(f"[失败] 处理 {subdir} 时出错: {e}")
                skipped_count += 1

        elif len(video_files) > 1:
            print(f"[跳过] {subdir} 里有 {len(video_files)} 个视频")
            skipped_count += 1

        else:
            print(f"[跳过] {subdir} 里没有视频")
            skipped_count += 1

    print("-" * 60)
    print(f"处理完成。成功移动: {moved_count}，跳过: {skipped_count}")


if __name__ == "__main__":
    main()