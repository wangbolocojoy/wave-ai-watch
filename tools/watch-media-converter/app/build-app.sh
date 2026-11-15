#!/bin/bash
# 编译并打包成 macOS .app（无需 Xcode 工程，swiftc 直接编译）
set -euo pipefail
cd "$(dirname "$0")"

APP_NAME="WAVE 媒体压缩"
DIST="$(pwd)/../dist"
BUNDLE="$DIST/${APP_NAME}.app"
ICON_SRC="$(pwd)/../../../assets/app-icons/01-assistant-whale.png"

rm -rf "$BUNDLE"
mkdir -p "$BUNDLE/Contents/MacOS" "$BUNDLE/Contents/Resources"

echo "==> 编译 Swift"
swiftc -O -o "$BUNDLE/Contents/MacOS/WaveMediaConverter" main.swift -framework AppKit

cp Info.plist "$BUNDLE/Contents/Info.plist"

if [ -f "$ICON_SRC" ]; then
    echo "==> 生成图标"
    ICON_TMP="$(mktemp -d)"
    ICONSET="$ICON_TMP/AppIcon.iconset"
    mkdir -p "$ICONSET"
    for size in 16 32 128 256 512; do
        /usr/bin/sips -z $size $size "$ICON_SRC" --out "$ICONSET/icon_${size}x${size}.png" >/dev/null
        double=$((size * 2))
        /usr/bin/sips -z $double $double "$ICON_SRC" --out "$ICONSET/icon_${size}x${size}@2x.png" >/dev/null
    done
    /usr/bin/iconutil -c icns "$ICONSET" -o "$BUNDLE/Contents/Resources/AppIcon.icns"
    rm -rf "$ICON_TMP"
fi

echo "==> Ad-hoc 签名"
codesign --force --deep --sign - "$BUNDLE" >/dev/null 2>&1 || true

echo "完成：$BUNDLE"
