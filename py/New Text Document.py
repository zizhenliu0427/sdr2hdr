from pathlib import Path
import sys

def main():
    root = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else Path.cwd()

    if not root.is_dir():
        print(f"不是有效目录: {root}")
        return

    count = 0

    # 只处理当前目录；如果要递归改子目录，我也能给你改
    for p in root.iterdir():
        if p.is_file() and p.suffix.lower() == ".pdf":
            new_path = p.with_suffix(".7z")

            # 避免重名覆盖
            if new_path.exists():
                i = 1
                while True:
                    candidate = new_path.with_name(f"{new_path.stem}({i}){new_path.suffix}")
                    if not candidate.exists():
                        new_path = candidate
                        break
                    i += 1

            p.rename(new_path)
            print(f"[已改] {p.name} -> {new_path.name}")
            count += 1

    print(f"完成，共修改 {count} 个文件。")

if __name__ == "__main__":
    main()