#!/usr/bin/env python3
"""Convert a transparent whale-girl PNG into an LVGL RGB565 C image."""
from pathlib import Path
import sys
from PIL import Image

BOARD = Path(__file__).resolve().parent.parent / "firmware/xiaozhi-esp32/main/boards/waveshare/esp32-s3-touch-amoled-2.06"
SIZE = 300
BG = (232, 237, 255, 255)


def main():
    if len(sys.argv) < 2:
        raise SystemExit("usage: png_to_whale_image.py input.png [output_dir]")
    src = Path(sys.argv[1])
    out_dir = Path(sys.argv[2]) if len(sys.argv) > 2 else BOARD
    img = Image.open(src).convert("RGBA")
    bbox = img.getbbox()
    if bbox:
        img = img.crop(bbox)
    img.thumbnail((SIZE - 20, SIZE - 20), Image.Resampling.LANCZOS)
    canvas = Image.new("RGBA", (SIZE, SIZE), BG)
    x = (SIZE - img.width) // 2
    y = (SIZE - img.height) // 2
    canvas.alpha_composite(img, (x, y))
    rgb = canvas.convert("RGB")
    data = bytearray()
    for r, g, b in rgb.getdata():
        value = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
        data += bytes((value & 0xFF, (value >> 8) & 0xFF))
    out_dir.mkdir(parents=True, exist_ok=True)
    header = out_dir / "whale_girl_image.h"
    source = out_dir / "whale_girl_image.c"
    header.write_text('''#pragma once
#include <lvgl.h>
#ifdef __cplusplus
extern "C" {
#endif
LV_IMAGE_DECLARE(whale_girl_image);
#ifdef __cplusplus
}
#endif
''')
    lines = ["#include \"whale_girl_image.h\"", "", "static const uint8_t whale_girl_map[] = {"]
    for i in range(0, len(data), 16):
        lines.append("    " + ", ".join(f"0x{b:02x}" for b in data[i:i+16]) + ",")
    lines += ["};", "", "const lv_image_dsc_t whale_girl_image = {",
              "    .header = {",
              "        .magic = LV_IMAGE_HEADER_MAGIC,",
              "        .cf = LV_COLOR_FORMAT_RGB565,",
              "        .flags = 0,",
              f"        .w = {SIZE},",
              f"        .h = {SIZE},",
              f"        .stride = {SIZE * 2},",
              "    },",
              f"    .data_size = sizeof(whale_girl_map),",
              "    .data = whale_girl_map,",
              "};", ""]
    source.write_text("\n".join(lines))
    print(f"wrote {header} and {source} ({len(data)} bytes)")


if __name__ == "__main__":
    main()
