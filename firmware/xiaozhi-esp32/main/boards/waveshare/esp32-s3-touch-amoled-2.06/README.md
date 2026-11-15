# Waveshare ESP32-S3-Touch-AMOLED-2.06

本项目补丁额外启用一版触摸手表桌面：可切换金色雅韵（语音别名“鎏金雅韵”）、活力运动、经典指针三款表盘，打开小智、音乐、活动、每日闹钟、倒计时和设置页面，并通过 MCP 工具语音查询系统状态、切换表盘和应用、查询今日步数、设置目标和操作计时器/闹钟。亮度、勿扰状态、表盘选择、活动数据和闹钟设置保存到 NVS；活动计步使用板载 QMI8658 内置计步器。SD 卡挂载和曲库扫描在独立任务中执行，不阻塞 LVGL 页面。

TF 卡本地 MP3/WAV 音乐只从卡根目录的 `Music` 文件夹读取（FAT 不区分大小写），文件夹内部可再嵌套三层。卡上没有 `Music` 时曲库为空，其它目录一律不扫描，因此录音、日志和备份不会混进音乐列表；旧卡上的 `音乐` 目录会在首次扫描时自动 `rename` 成 `Music`。调试：WiFi 连上后板级在 8080 端口起只读诊断服务（`/log`、`/log.1`、`/info`、`/mem`），另带需 token 的开发接口（`/screen` 截图、`/nav?page=` 切页、`/upload` 写卡、`/file` 读卡），token 每次启动随机生成并写到串口和 `system.log` 的 `debug token` 行。内测接口 `/mem` 返回内部 SRAM / PSRAM 的空闲、最大块和最低水位。注意 MultiNet 常驻后内部 SRAM 只剩约 65KB（最大块 24KB），新功能不要把大缓冲放内部 RAM：图片/视频解码缓冲和视频任务栈都走 PSRAM。FAT 长文件名按 UTF-8 解析（板级 `config.json` 中的 `CONFIG_FATFS_API_ENCODING_UTF_8`），中文目录名和歌名才能正确显示和打开。应用页是 3×3 九宫格（大肥鱼、音乐、活动、闹钟、倒计时、设置、录音、图片、视频），图标来自 `assets/app-icons/*.png`（400×400 透明底），由 `scripts/app_icons_from_png.py` 生成 92×92 RGB565 C 数组（透明角烘焙成页面底色，省掉 alpha 平面）；表盘鲸鱼插画与音乐波形图用 `scripts/png_to_jpeg_asset.py` 转成 JPEG 存 flash，开机解码到 PSRAM 供 LVGL 使用。图片浏览与视频播放都只读 TF 卡：照片放 `/sdcard/Photos`（JPG，过大的图自动按 1/2、1/4、1/8 缩放），视频放 `/sdcard/Video`（MJPEG 编码的 AVI，按 AVI 帧率播放，无音轨）；缺卡或无目录时页面会提示。AI 可调用 `self.watch.music.search/play/pause/resume/next/previous/stop/status`。搜索结果带路径哈希曲目编号；歌名命中多首时返回编号供用户确认，之后用 `track_id` 指定播放。解码后的 16-bit PCM 统一转换到手表输出采样率，并通过 XiaoZhi AudioService 的共享输出队列播放。小智聆听/回答状态会展示中文双眼动画与字幕。

固件依赖 ESP-IDF 6.1 或更新版本，中文固件已完成构建。倒计时支持 1 秒至 24 小时、语音查询/取消和结束提示。每日闹钟支持触屏预设、MCP 设置/查询/关闭；它按已同步的系统本地时间运行，每 10 秒检查一次，在手表固件持续运行时触发，勿扰模式下静音。它目前不负责从关机或深度睡眠唤醒设备，时钟来源和唤醒能力仍待真机验证。实体板尚未到手，QMI8658 初始化与计步准确性、SD 卡扫描、音频/AEC 联动、播放中插话和热拔卡仍待真机验证。

本机配置中心可通过手表「设置 → 开启网页配置配对」连接设备。一次性配对码有效 60 秒，网页只可读取/写入表盘、亮度、音量和勿扰；令牌闲置 30 分钟失效，网页断开时会撤销令牌。配置服务使用明文 HTTP，只应在可信任的局域网中使用，不提供任意 MCP 调用。


[ESP32-S3-Touch-AMOLED-2.06](https://www.waveshare.com/esp32-s3-touch-amoled-2.06.htm) is a high-performance, wearable watch-style development board designed by Waveshare. Based on the ESP32-S3R8 microcontroller, it integrates a 2.06inch AMOLED capacitive touch display, 6-axis IMU, RTC chip, audio codec chip, power management IC, and so on. Comes with a custom-designed case with a smartwatch-like appearance, making it ideal for prototyping and functional verification of wearable applications.

## images

| [ESP32-S3-Touch-AMOLED-2.06](https://www.waveshare.com/esp32-s3-touch-amoled-2.06.htm)                                                                                                                                       |
|------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| <img style="width: 150px; height: auto; display: block; margin: 0 auto;" src= "https://www.waveshare.com/media/catalog/product/cache/1/image/800x800/9df78eab33525d08d6e5fb8d27136e95/e/s/esp32-s3-touch-amoled-2.06-1.jpg"> |

