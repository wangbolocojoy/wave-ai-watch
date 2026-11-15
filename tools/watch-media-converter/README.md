# 手表媒体压缩工具

把手机/电脑里的照片和视频，批量压成 **WAVE AI Watch 能流畅查看** 的格式。
双击 `启动压缩工具.command` 打开图形界面，或者用命令行批量处理。

> 依赖：`ffmpeg`（`brew install ffmpeg`）。图形界面用 macOS 自带的 `/usr/bin/python3`，不需要装 Python 包。

## 手表支持的格式与尺寸

固件里的限制是硬性的——`/Photos` 只认 JPG、`/Video` 只认 MJPEG AVI，格式不对会直接显示"无法播放"。

### 照片

| 项目 | 要求 | 原因 |
|---|---|---|
| 容器/编码 | **JPEG（baseline）**，4:2:0 / 4:2:2 | 固件用 TJpgDec 解码，不支持 progressive |
| 文件名 | `.jpg` / `.jpeg` | `/Photos` 只扫描这两种扩展名 |
| 像素数 | **≤ 240,000**（如 480×500、640×375） | 解码缓冲 480 KB ÷ 2 字节/像素 |
| 建议最长边 | 480~512 px | 屏幕显示区约 388×424，再大也会被缩小 |
| 单张大小 | 50~150 KB | 越大打开越慢（SD 卡读取 + 解码） |
| 方向 | 已摆正（无 EXIF 旋转） | 手表不读 EXIF 方向 |

超预算的图片固件会自动按 1/2、1/4 降采样（能看，但更小更糊），所以压缩工具会直接压到预算内。

### 视频

| 项目 | 要求 | 原因 |
|---|---|---|
| 容器 | **AVI**（`.avi` / `.mjpg`） | `/Video` 只扫描这两种扩展名 |
| 编码 | **MJPEG**（每帧是一张 baseline JPEG） | ESP32-S3 没有 H.264 硬解，软解只跑得动 MJPEG |
| 分辨率 | **320×240 或更小**（建议 288×216 ~ 360×288） | 逐帧软解 + RGB565 缩放 |
| 帧率 | **12~15 fps** | 更高的帧率解码跟不上，会掉帧/卡顿 |
| 音轨 | **无**（工具会去掉） | 固件目前只解码画面 |
| 单分钟体积 | 2~5 MB | TF 卡和读取速度都无压力 |

> 用 `ffmpeg` 手动转的等效命令：
>
> ```sh
> ffmpeg -i input.mp4 -vf "scale=320:240:force_original_aspect_ratio=decrease,fps=15" \
>        -c:v mjpeg -q:v 7 -pix_fmt yuvj420p -an -f avi output.avi
> ```

## 预设

| 预设 | 照片 | 视频 | 适合 |
|---|---|---|---|
| 紧凑 | 384 px 盒 / q6 | 288×216 / 12 fps / q8 | 大批量照片、长视频 |
| **标准（推荐）** | 480 px 盒 / q4 | 320×240 / 15 fps / q7 | 日常使用 |
| 清晰 | 512 px 盒 / q3 | 360×288 / 15 fps / q5 | 少量精挑细选的内容 |

## 使用

### 图形界面（推荐）

双击 `启动压缩工具.command`：

1. 选输入文件夹（照片/视频混放也行，会按扩展名自动分流）；
2. 选输出文件夹；
3. 选预设，点「开始转换」；
4. 结束后点「打开输出文件夹」。

### 命令行

```sh
# 整个文件夹
/usr/bin/python3 convert.py --input ~/Pictures/旅行 --output ~/Desktop/watch-media

# 指定文件 + 预设
/usr/bin/python3 convert.py --input a.jpg b.mp4 --output ~/Desktop/watch-media --preset compact --photos-only
```

支持的输入：`jpg/jpeg/png/heic/heif/webp/bmp/tif/tiff`、`mp4/mov/m4v/mkv/avi/webm/wmv/flv/3gp/mts`。
HEIC 用 macOS 自带的 `sips` 先转 JPEG（ffmpeg 不带 HEIC 解码器）。

## 拷到手表

输出文件夹里是 `Photos/` 和 `Video/` 两个子目录，直接拷到 TF 卡根目录：

```text
<TF 卡根目录>/
├── Music/       ← 音乐（mp3 / wav）
├── Photos/      ← 本次生成的照片
├── Video/       ← 本次生成的视频
└── Recordings/  ← 手表录音（自动生成）
```

用读卡器或 Finder 把 `Photos`、`Video` 两个文件夹拖进卡根目录即可（会合并/覆盖同名文件）。

## 常见问题

- **照片打不开 / 显示"无法显示这张图片"**：检查是不是 progressive JPEG 或 PNG（工具输出的都是 baseline JPEG）。
- **视频没有声音**：固件目前不支持音轨，属正常。
- **视频卡顿**：降低预设（紧凑），或确认分辨率 ≤ 320×240、帧率 ≤ 15。
- **照片是横着的**：工具已按 EXIF 自动摆正；如果某张仍不对，说明原图没有方向信息。
- **转换很慢**：视频转码吃 CPU，1080p 视频大致 1 秒可转 5~10 秒素材；可减少视频数量或先裁短。
