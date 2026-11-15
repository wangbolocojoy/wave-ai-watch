#!/bin/bash
# 双击这个文件启动图形界面（用 macOS 自带 Python，带 Tkinter）
cd "$(dirname "$0")" || exit 1
exec /usr/bin/python3 convert.py --gui
