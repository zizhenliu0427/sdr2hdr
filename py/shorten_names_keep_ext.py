from pathlib import Path
import sys

MAX_LEN = 100


def char_weight(ch: str) -> int:
    """
    非中日韩字符按 1
    中日韩字符按 3
    """
    code = ord(ch)
    cjk_ranges = [
        (0x4E00, 0x9FFF),
        (0x3400, 0x4DBF),
        (0x20000, 0x2A6DF),
        (0x2A700, 0x2B73F),
        (0x2B740, 0x2B81F),
        (0x2B820, 0x2CEAF),
        (0xF900, 0xFAFF),
        (0x3040, 0x309F),
        (0x30A0, 0x30FF),
        (0xAC00, 0xD7AF),
        (0x1100, 0x11FF),
        (0x3130, 0x318F),
    ]
    for start, end in cjk_ranges:
        if start <= code <= end:
            return 3
    return 1


def weighted_length(text: str) -> int:
    return sum(char_weight(ch) for ch in text)


def trim_to_max_length(text: str, max_len: int) -> str:
    result = []
    current = 0
    for ch in text:
        w = char_weight(ch)
        if current + w > max_len:
            break
        result.append(ch)
        current += w
    return "".join(result)


def split_file_name(name: str):
    p = Path(name)
    if p.suffix and p.stem:
        return p.stem, p.suffix
    return name, ""


def existing_names_casefold(parent: Path, exclude_name: str = None):
    names = set()
    for p in parent.iterdir():
        if exclude_name is not None and p.name == exclude_name:
            continue
        names.add(p.name.casefold())
    return names


def make_safe_name(original_name: str, max_len: int, is_file: bool) -> str:
    """
    严格保留文件扩展名。
    如果扩展名本身太长，直接报错。
    """
    if is_file:
        stem, suffix = split_file_name(original_name)

        if not suffix:
            # 没扩展名的文件，按普通名称处理
            new_name = trim_to_max_length(original_name, max_len)
            if not new_name:
                raise ValueError(f"无法生成合法名称: {original_name}")
            return new_name

        suffix_len = weighted_length(suffix)
        if suffix_len >= max_len:
            raise ValueError(f"扩展名过长，无法在保留后缀前提下处理: {original_name}")

        allowed_stem_len = max_len - suffix_len
        new_stem = trim_to_max_length(stem, allowed_stem_len)

        if not new_stem:
            raise ValueError(f"主文件名为空，无法在保留后缀前提下处理: {original_name}")

        candidate = new_stem + suffix

        if weighted_length(candidate) > max_len:
            raise ValueError(f"生成名称仍超长: {original_name}")

        return candidate

    else:
        new_name = trim_to_max_length(original_name, max_len)
        if not new_name:
            raise ValueError(f"无法生成合法文件夹名称: {original_name}")
        return new_name


def add_numeric_suffix(name: str, index: int, max_len: int, is_file: bool) -> str:
    """
    冲突时加 1/2/3...
    对文件严格保留扩展名。
    """
    suffix_num = str(index)

    if is_file:
        stem, ext = split_file_name(name)

        if not ext:
            base_len = max_len - weighted_length(suffix_num)
            if base_len <= 0:
                raise ValueError(f"编号后长度不足，无法处理: {name}")
            new_stem = trim_to_max_length(stem, base_len)
            if not new_stem:
                raise ValueError(f"编号后无法生成合法名称: {name}")
            candidate = new_stem + suffix_num
            if weighted_length(candidate) > max_len:
                raise ValueError(f"编号后仍超长: {name}")
            return candidate

        ext_len = weighted_length(ext)
        num_len = weighted_length(suffix_num)

        if ext_len + num_len >= max_len:
            raise ValueError(f"扩展名+编号已超限，无法处理: {name}")

        allowed_stem_len = max_len - ext_len - num_len
        new_stem = trim_to_max_length(stem, allowed_stem_len)

        if not new_stem:
            raise ValueError(f"编号后主文件名为空，无法处理: {name}")

        candidate = new_stem + suffix_num + ext

        if weighted_length(candidate) > max_len:
            raise ValueError(f"编号后仍超长: {name}")

        return candidate

    else:
        num_len = weighted_length(suffix_num)
        if num_len >= max_len:
            raise ValueError(f"编号本身已超限: {name}")

        allowed_len = max_len - num_len
        new_base = trim_to_max_length(name, allowed_len)

        if not new_base:
            raise ValueError(f"编号后文件夹名称为空: {name}")

        candidate = new_base + suffix_num

        if weighted_length(candidate) > max_len:
            raise ValueError(f"编号后文件夹名仍超长: {name}")

        return candidate


def resolve_unique_name(parent: Path, original_name: str, max_len: int, is_file: bool) -> str:
    base_candidate = make_safe_name(original_name, max_len, is_file)
    existing = existing_names_casefold(parent, exclude_name=original_name)

    if base_candidate.casefold() not in existing:
        return base_candidate

    i = 1
    while True:
        candidate = add_numeric_suffix(base_candidate, i, max_len, is_file)
        if candidate.casefold() not in existing:
            return candidate
        i += 1


def rename_item(path: Path):
    parent = path.parent
    is_file = path.is_file()
    old_name = path.name

    need_rename = weighted_length(old_name) > MAX_LEN
    if not need_rename:
        return False, old_name, old_name

    new_name = resolve_unique_name(parent, old_name, MAX_LEN, is_file)

    if new_name == old_name:
        return False, old_name, old_name

    new_path = parent / new_name
    path.rename(new_path)
    return True, old_name, new_name


def main():
    if len(sys.argv) > 1:
        root = Path(sys.argv[1]).resolve()
    else:
        root = Path.cwd()

    if not root.exists() or not root.is_dir():
        print(f"目录不存在或不是文件夹: {root}")
        return

    print(f"开始处理: {root}")
    print("-" * 80)

    renamed_count = 0
    skipped_count = 0
    error_count = 0

    all_paths = sorted(root.rglob("*"), key=lambda p: len(p.parts), reverse=True)

    for path in all_paths:
        try:
            changed, old_name, new_name = rename_item(path)
            if changed:
                print(f"[重命名] {path}")
                print(f"    {old_name}")
                print(f" -> {new_name}")
                renamed_count += 1
            else:
                skipped_count += 1
        except Exception as e:
            print(f"[失败] {path} -> {e}")
            error_count += 1

    print("-" * 80)
    print(f"完成：重命名 {renamed_count} 项，跳过 {skipped_count} 项，失败 {error_count} 项")


if __name__ == "__main__":
    main()