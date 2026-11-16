#!/usr/bin/env python3
"""把照片和视频压成手表能流畅播放的格式。

手表端限制（Waveshare ESP32-S3 2.06 AMOLED 固件实际值）：
  照片：只认 /Photos 下的 .jpg，解码缓冲 480 KB（RGB565 2 字节/像素），
        所以像素数必须 ≤ 240,000（例如 512×468），且必须是 baseline JPEG。
  视频：只认 /Video 下的 MJPEG 编码 .avi，逐帧软解，320×240@15fps 流畅。

用法：
  python3 convert.py --input ~/Pictures/旅行 --output ~/Desktop/watch-media
  python3 convert.py --input a.jpg b.mp4 --output out --preset compact
  python3 convert.py --gui                # 图形界面
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
from dataclasses import dataclass, field
from pathlib import Path

PHOTO_EXTS = {".jpg", ".jpeg", ".png", ".heic", ".heif", ".webp", ".bmp", ".tif", ".tiff", ".gif"}
VIDEO_EXTS = {".mp4", ".mov", ".m4v", ".mkv", ".avi", ".webm", ".wmv", ".flv", ".3gp", ".mts"}

# 照片像素预算：480KB / 2 字节
MAX_PHOTO_PIXELS = 240_000


@dataclass
class Preset:
    name: str
    label: str
    photo_side: int
    photo_quality: int      # ffmpeg -q:v（越小越清晰）
    video_width: int
    video_height: int
    video_fps: int
    video_quality: int
    note: str


PRESETS: dict[str, Preset] = {
    "compact": Preset("compact", "紧凑（省空间，翻页快）", 384, 6, 288, 216, 12, 8,
                      "适合大量照片、长视频"),
    "standard": Preset("standard", "标准（推荐）", 480, 4, 320, 240, 15, 7,
                       "清晰度和流畅度的平衡"),
    "sharp": Preset("sharp", "清晰（占空间）", 512, 3, 360, 288, 15, 5,
                    "照片最清楚，视频更费卡"),
}

FFMPEG = shutil.which("ffmpeg") or "/opt/homebrew/bin/ffmpeg"
SIPS = "/usr/bin/sips"


def run(command: list[str], capture: bool = True) -> subprocess.CompletedProcess:
    return subprocess.run(command, capture_output=capture, text=True)


def check_tools() -> bool:
    if shutil.which(FFMPEG) is None:
        print("找不到 ffmpeg，请先安装：brew install ffmpeg")
        return False
    return True


def photo_box(preset: Preset) -> int:
    """正方形缩放盒：小于预设边长，且盒内最大像素数不超过解码预算。"""
    return min(preset.photo_side, int(MAX_PHOTO_PIXELS ** 0.5))


def crop_to_qr(source: Path) -> Path:
    """收款码模式：把截图裁到二维码/深色内容区域（需要 Pillow）。"""
    try:
        from PIL import Image
    except ImportError:
        return source
    try:
        image = Image.open(source).convert("RGB")
        gray = image.convert("L")
        width, height = gray.size
        # 缩小后再找深色区域，速度快且不受噪点影响
        small = gray.resize((max(1, width // 8), max(1, height // 8)))
        pixels = small.load()
        xs, ys = [], []
        for y in range(small.height):
            for x in range(small.width):
                if pixels[x, y] < 110:
                    xs.append(x)
                    ys.append(y)
        if not xs:
            return source
        left, right = min(xs) * 8, (max(xs) + 1) * 8
        top, bottom = min(ys) * 8, (max(ys) + 1) * 8
        span = max(right - left, bottom - top)
        pad = int(span * 0.08)
        side = span + pad * 2
        cx = (left + right) // 2
        cy = (top + bottom) // 2
        box = (max(0, cx - side // 2), max(0, cy - side // 2),
               min(width, cx + side // 2), min(height, cy + side // 2))
        cropped = image.crop(box)
        out = Path(tempfile.mkstemp(suffix=".png")[1])
        cropped.save(out)
        print(f"  收款码裁切：{width}×{height} → {cropped.size[0]}×{cropped.size[1]}")
        return out
    except Exception:
        return source


def convert_photo(source: Path, destination: Path, preset: Preset, log) -> tuple[bool, str]:
    """转成 baseline JPEG（4:2:0），自动摆正方向。"""
    work = source
    temp: Path | None = None
    if source.suffix.lower() in {".heic", ".heif"}:
        # ffmpeg 没有 HEIF 解码器，用 macOS 自带的 sips 先转 JPEG
        fd, tmp_name = tempfile.mkstemp(suffix=".jpg")
        os.close(fd)
        temp = Path(tmp_name)
        result = run([SIPS, "-s", "format", "jpeg", "-s", "formatOptions", "best",
                      str(source), "--out", str(temp)])
        if result.returncode != 0:
            return False, "HEIC 转换失败（sips）"
        work = temp

    try:
        width, height = probe_image_size(work)
        box = photo_box(preset)
        temp_output = destination.with_suffix(".tmp.jpg")
        result = run([
            FFMPEG, "-y", "-hide_banner", "-loglevel", "error",
            "-i", str(work),
            "-vf", (f"scale={box}:{box}:force_original_aspect_ratio=decrease:flags=lanczos,"
                    f"format=yuvj420p"),
            "-frames:v", "1",
            "-q:v", str(preset.photo_quality),
            "-map_metadata", "-1",
            str(temp_output),
        ])
        if result.returncode != 0 or not temp_output.exists():
            return False, "ffmpeg 转换失败"
        temp_output.replace(destination)
        out_w, out_h = probe_image_size(destination)
        return True, f"{width}×{height} → {out_w}×{out_h}"
    finally:
        if temp is not None:
            temp.unlink(missing_ok=True)


def probe_image_size(path: Path) -> tuple[int, int]:
    import re

    result = run([FFMPEG, "-hide_banner", "-i", str(path)])
    for line in result.stderr.splitlines():
        if "Video:" not in line:
            continue
        match = re.search(r"(\d{2,5})x(\d{2,5})", line)
        if match:
            return int(match.group(1)), int(match.group(2))
    return 0, 0


def convert_video(source: Path, destination: Path, preset: Preset, log) -> tuple[bool, str]:
    """转成 MJPEG AVI（无音轨），按预设分辨率/帧率缩放。"""
    width, height = preset.video_width, preset.video_height
    video_filter = (
        f"scale={width}:{height}:force_original_aspect_ratio=decrease:flags=lanczos,"
        f"scale=trunc(iw/2)*2:trunc(ih/2)*2,"
        f"fps={preset.video_fps}"
    )
    temp_output = destination.with_suffix(".tmp.avi")
    result = run([
        FFMPEG, "-y", "-hide_banner", "-loglevel", "error",
        "-i", str(source),
        "-vf", video_filter,
        "-c:v", "mjpeg",
        "-q:v", str(preset.video_quality),
        "-pix_fmt", "yuvj420p",
        # 音频保留为 16 kHz 单声道 PCM：手表逐块喂给 ES8311 播放
        "-c:a", "pcm_s16le",
        "-ar", "16000",
        "-ac", "1",
        "-map_metadata", "-1",
        "-f", "avi",
        str(temp_output),
    ])
    if result.returncode != 0 or not temp_output.exists():
        return False, "ffmpeg 转换失败（可能格式不受支持）"
    temp_output.replace(destination)
    size_mb = destination.stat().st_size / (1024 * 1024)
    return True, f"{width}×{height} @ {preset.video_fps}fps · {size_mb:.1f} MB"


def unique_path(directory: Path, stem: str, suffix: str) -> Path:
    candidate = directory / f"{stem}{suffix}"
    index = 2
    while candidate.exists():
        candidate = directory / f"{stem}-{index}{suffix}"
        index += 1
    return candidate


def collect_sources(inputs: list[Path]) -> list[Path]:
    files: list[Path] = []
    for item in inputs:
        if item.is_dir():
            for path in sorted(item.rglob("*")):
                if path.is_file() and path.suffix.lower() in (PHOTO_EXTS | VIDEO_EXTS):
                    files.append(path)
        elif item.is_file():
            files.append(item)
    return files


def convert_all(inputs: list[Path], output: Path, preset: Preset, log, progress=None,
                payment_mode: bool = False) -> dict:
    photos_dir = output / ("Payment" if payment_mode else "Photos")
    videos_dir = output / "Video"
    photos_dir.mkdir(parents=True, exist_ok=True)
    videos_dir.mkdir(parents=True, exist_ok=True)

    sources = collect_sources(inputs)
    summary = {"photos": 0, "videos": 0, "failed": 0, "skipped": [], "total_in": 0, "total_out": 0}
    total = len(sources)
    log(f"共找到 {total} 个文件，预设：{preset.label}")
    for index, source in enumerate(sources, start=1):
        if progress is not None:
            progress(index - 1, total)
        suffix = source.suffix.lower()
        target = None
        detail = ""
        ok = False
        if suffix in PHOTO_EXTS:
            if payment_mode:
                cropped = crop_to_qr(source)
                if cropped != source:
                    source_to_use = cropped
                else:
                    source_to_use = source
            else:
                source_to_use = source
            target = unique_path(photos_dir, source.stem, ".jpg")
            ok, detail = convert_photo(source_to_use, target, preset, log)
            kind = "photo"
        elif suffix in VIDEO_EXTS:
            target = unique_path(videos_dir, source.stem, ".avi")
            ok, detail = convert_video(source, target, preset, log)
            kind = "video"
        else:
            continue
        if ok:
            size_in = source.stat().st_size
            size_out = target.stat().st_size
            summary["total_in"] += size_in
            summary["total_out"] += size_out
            if kind == "photo":
                summary["photos"] += 1
            else:
                summary["videos"] += 1
            log(f"[{index}/{total}] ✓ {source.name} → {target.name}  ({detail})")
        else:
            summary["failed"] += 1
            log(f"[{index}/{total}] ✗ {source.name}：{detail}")
    if progress is not None:
        progress(total, total)
    return summary


# ---------------------------------------------------------------- 图形界面


def launch_gui() -> None:
    import threading
    import tkinter as tk
    from tkinter import filedialog, messagebox, ttk

    root = tk.Tk()
    root.title("手表媒体压缩工具 · WAVE AI Watch")
    root.geometry("760x560")
    root.minsize(680, 480)

    preset_var = tk.StringVar(value="standard")
    input_var = tk.StringVar(value=str(Path.home() / "Pictures"))
    output_var = tk.StringVar(value=str(Path.home() / "Desktop" / "watch-media"))

    header = tk.Label(root, text="把照片和视频压成手表能流畅查看的格式",
                      font=("Helvetica", 15, "bold"))
    header.pack(pady=(14, 2))
    subtitle = tk.Label(root, text="照片 → baseline JPEG（≤512px）  ·  视频 → MJPEG AVI（320×240 / 15fps）",
                        fg="#666")
    subtitle.pack()

    frame = tk.Frame(root)
    frame.pack(fill="x", padx=18, pady=12)

    def add_row(row: int, label: str, variable: tk.StringVar) -> None:
        tk.Label(frame, text=label, width=10, anchor="w").grid(row=row, column=0, sticky="w", pady=4)
        entry = tk.Entry(frame, textvariable=variable)
        entry.grid(row=row, column=1, sticky="ew", padx=6, pady=4)

    add_row(0, "输入文件夹", input_var)
    add_row(1, "输出文件夹", output_var)
    frame.columnconfigure(1, weight=1)

    def choose_input() -> None:
        path = filedialog.askdirectory(title="选择照片/视频所在文件夹", initialdir=input_var.get())
        if path:
            input_var.set(path)

    def choose_output() -> None:
        path = filedialog.askdirectory(title="选择输出文件夹", initialdir=output_var.get())
        if path:
            output_var.set(path)

    buttons = tk.Frame(frame)
    buttons.grid(row=2, column=1, sticky="w", padx=6, pady=2)
    tk.Button(buttons, text="选择输入…", command=choose_input).pack(side="left", padx=(0, 6))
    tk.Button(buttons, text="选择输出…", command=choose_output).pack(side="left")

    preset_frame = tk.LabelFrame(root, text="清晰度预设", padx=12, pady=6)
    preset_frame.pack(fill="x", padx=18)
    for key, preset in PRESETS.items():
        tk.Radiobutton(preset_frame, text=f"{preset.label}  {preset.note}",
                       variable=preset_var, value=key, anchor="w").pack(fill="x")

    progress = ttk.Progressbar(root, mode="determinate")
    progress.pack(fill="x", padx=18, pady=(12, 4))

    log_box = tk.Text(root, height=12, state="disabled", wrap="word")
    log_box.pack(fill="both", expand=True, padx=18, pady=(0, 8))

    status_var = tk.StringVar(value="就绪")
    tk.Label(root, textvariable=status_var, fg="#444").pack(anchor="w", padx=18)

    action_row = tk.Frame(root)
    action_row.pack(fill="x", padx=18, pady=10)
    open_button = tk.Button(action_row, text="打开输出文件夹", state="disabled",
                            command=lambda: subprocess.run(["open", output_var.get()]))
    open_button.pack(side="right")

    def append_log(message: str) -> None:
        log_box.configure(state="normal")
        log_box.insert("end", message + "\n")
        log_box.see("end")
        log_box.configure(state="disabled")
        root.update_idletasks()

    def set_progress(done: int, total: int) -> None:
        progress["maximum"] = max(1, total)
        progress["value"] = done
        status_var.set(f"进度 {done}/{total}")
        root.update_idletasks()

    def start() -> None:
        if not check_tools():
            messagebox.showerror("缺少 ffmpeg", "请先在终端执行：brew install ffmpeg")
            return
        inputs = [Path(input_var.get())]
        if not inputs[0].exists():
            messagebox.showerror("路径不存在", f"找不到输入文件夹：{inputs[0]}")
            return
        output = Path(output_var.get())
        preset = PRESETS[preset_var.get()]
        start_button.configure(state="disabled")
        open_button.configure(state="disabled")
        progress["value"] = 0
        log_box.configure(state="normal")
        log_box.delete("1.0", "end")
        log_box.configure(state="disabled")

        def worker() -> None:
            try:
                summary = convert_all(inputs, output, preset, append_log, set_progress)
                append_log("")
                append_log(f"完成：照片 {summary['photos']} 张，视频 {summary['videos']} 个，"
                           f"失败 {summary['failed']} 个")
                append_log(f"输出体积 {summary['total_out'] / 1024 / 1024:.1f} MB"
                           f"（原始 {summary['total_in'] / 1024 / 1024:.1f} MB）")
                append_log(f"把整个输出文件夹里的 Photos、Video 拷到 TF 卡根目录即可。")
                status_var.set("完成")
                open_button.configure(state="normal")
            except Exception as error:  # noqa: BLE001 - 界面里显示任何异常
                append_log(f"出错：{error}")
                status_var.set("出错")
            finally:
                start_button.configure(state="normal")

        threading.Thread(target=worker, daemon=True).start()

    start_button = tk.Button(root, text="开始转换", height=2, bg="#2ea6a0", fg="white",
                             font=("Helvetica", 13, "bold"), command=start)
    start_button.pack(fill="x", padx=18, pady=(0, 14))

    root.mainloop()


def main() -> None:
    parser = argparse.ArgumentParser(description="把手表照片/视频压成可流畅播放的格式")
    parser.add_argument("--input", nargs="*", type=Path, help="输入文件或文件夹")
    parser.add_argument("--output", type=Path, default=Path.home() / "Desktop" / "watch-media",
                        help="输出文件夹（会生成 Photos/ 和 Video/）")
    parser.add_argument("--preset", choices=sorted(PRESETS), default="standard")
    parser.add_argument("--qr", action="store_true",
                        help="收款码模式：图片输出到 Payment/（384px，质量优先），跳过视频")
    parser.add_argument("--gui", action="store_true", help="打开图形界面")
    args = parser.parse_args()

    if args.gui or not args.input:
        launch_gui()
        return

    if not check_tools():
        sys.exit(1)
    preset = PRESETS["sharp"] if args.qr else PRESETS[args.preset]
    if args.qr:
        log = print
        log("收款码模式：图片输出到 Payment/（384px）")
    summary = convert_all(
        args.input, args.output, preset, print, None, payment_mode=args.qr)
    print()
    print(f"完成：照片 {summary['photos']} 张，视频 {summary['videos']} 个，失败 {summary['failed']} 个")
    print(f"输出：{args.output}")
    print(f"输出体积 {summary['total_out'] / 1024 / 1024:.1f} MB "
          f"（原始 {summary['total_in'] / 1024 / 1024:.1f} MB）")


if __name__ == "__main__":
    main()
