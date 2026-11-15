#!/bin/sh
set -eu
cd "$(dirname "$0")"
cmake -S . -B build
cmake --build build -j"$(sysctl -n hw.ncpu 2>/dev/null || echo 4)"
exec ./build/watch-host "$@"
