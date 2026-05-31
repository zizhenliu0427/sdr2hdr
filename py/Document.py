import os
import re
import shutil
from datetime import datetime, timedelta

# ================= 配置区 =================
# 你想要处理的文件夹路径，"." 代表当前目录
TARGET_DIR = "." 

# 每天的起始计算时间（改成 12 代表中午 12 点）。
# 中午 12 点之前的视频都会被归类到前一天的场次中！
DAY_START_HOUR = 12 

# 支持整理的视频格式，如果还有别的格式可以自己往里加
VIDEO_EXTENSIONS = ('.mp4', '.webm', '.mkv', '.avi', '.flv', '.mov')
# ==========================================

# 用正则表达式提取文件名中的时间
# 匹配格式如：2026.04.12-01.33.47 或 2026.04.11 - 22.45.01
time_pattern = re.compile(r"(\d{4})\.(\d{2})\.(\d{2})\s*[-_]?\s*(\d{2})\.(\d{2})\.(\d{2})")

def organize_files():
    print("🎬 开始扫描并整理视频文件 (中午12点分界)...\n")
    moved_count = 0

    for filename in os.listdir(TARGET_DIR):
        # 1. 过滤非视频文件
        if not filename.lower().endswith(VIDEO_EXTENSIONS):
            continue

        # 2. 匹配文件名中的时间戳
        match = time_pattern.search(filename)
        if not match:
            continue # 如果文件名里没有时间，就跳过

        # 提取 年, 月, 日, 时, 分, 秒
        y, m, d, H, M, S = map(int, match.groups())
        
        try:
            dt = datetime(y, m, d, H, M, S)
        except ValueError:
            print(f"⚠️ 跳过：{filename} (时间格式不合法)")
            continue

        # 3. 核心逻辑：判断属于哪个“游戏日”
        if dt.hour < DAY_START_HOUR:
            # 中午12点之前，算作前一天
            start_date = dt - timedelta(days=1)
            end_date = dt
        else:
            # 中午12点及以后，算作当天
            start_date = dt
            end_date = dt + timedelta(days=1)

        # 4. 生成文件夹名称
        # 月份文件夹：以 start_date 为准，例如 "202604"
        month_folder = start_date.strftime("%Y%m")
        # 日期区间文件夹：例如 "20260411~20260412"
        session_folder = f"{start_date.strftime('%Y%m%d')}~{end_date.strftime('%Y%m%d')}"

        # 5. 构建完整的目标路径
        dest_dir = os.path.join(TARGET_DIR, month_folder, session_folder)
        os.makedirs(dest_dir, exist_ok=True) # 如果文件夹不存在，自动创建

        # 6. 移动文件
        src_path = os.path.join(TARGET_DIR, filename)
        dest_path = os.path.join(dest_dir, filename)

        if not os.path.exists(dest_path):
            try:
                shutil.move(src_path, dest_path)
                print(f"✅ 成功移动: {filename}")
                print(f"   -> {month_folder}\\{session_folder}\\")
                moved_count += 1
            except Exception as e:
                print(f"❌ 移动失败: {filename} (原因: {e})")
        else:
            print(f"⏭️ 跳过: {filename} (目标文件夹中已存在同名文件)")

    print(f"\n🎉 整理完成！共分类了 {moved_count} 个视频。")

if __name__ == "__main__":
    organize_files()