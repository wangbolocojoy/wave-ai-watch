# DeepSeek 鲸鱼娘表盘素材 Brief

LVGL 用代码拼形状做不出好看的立绘，表盘只负责排版和时间/健康数据。请生成一张
**透明背景 PNG**，我们负责转换成 LVGL 图片并放到 410×502 表盘里。

## 规格

- 格式：PNG，带 alpha 透明通道（不要 JPG，不要带背景）
- 尺寸：1024×1024，主体居中，四周留 8% 透明边
- 构图：Q 版半身，正面，头肩，不要全身，不要文字/水印/logo
- 风格：高级扁平矢量贴纸、深蓝描边、轻微渐变和 soft cel shading
- 颜色：
  - 主蓝 `#4D6BFE`
  - 浅蓝 `#8EA6FF`
  - 肤色 `#FFF8F1`
  - 深海军蓝眼睛/描边 `#1B2350`
  - 腮红 `#FFB3C6`
- 角色：可爱鲸鱼娘，蓝色头发，两侧鲸鱼鳍/尾鳍发饰，头顶小水花；
  大眼睛、腮红、微笑，浅蓝色卫衣；不要写实，不要复杂背景。

## 可直接给 Codex / 图片模型的 Prompt

```text
Generate one transparent-background PNG (1024x1024, alpha channel) of a cute
chibi DeepSeek whale-girl mascot for a smartwatch face. Front-facing
head-and-shoulders, centered, 8% transparent margin. High-quality flat vector
sticker illustration, thick navy outline, soft cel shading, subtle gradients.
Deep blue hair (#4D6BFE) with two whale-fluke hair ornaments and a small water
spout; cream face (#FFF8F1); large navy eyes (#1B2350) with white highlights;
pink blush (#FFB3C6); tiny smile; light-blue hoodie (#8EA6FF). Cute, friendly,
clean silhouette, no background, no text, no logo, no watermark, no watch
frame. Deliver only the PNG.
```

如果只能出 SVG，也可以发 SVG，我来转 PNG。

## 交付位置

把文件放到：

`/Users/wangbo/Allworkspace/waveshare-watch-system/assets/whale-girl.png`

或在对话里直接发图片/绝对路径。

## 接入方式

拿到 PNG 后：
1. 缩放到约 300×300，合成到 `#E8EDFF` 背景或保留 alpha；
2. 转成 LVGL `RGB565A8` / `ARGB8888` 图片描述符；
3. 替换表盘中央的临时 `DEEPSEEK` 占位；
4. 真机截图微调大小和位置。
