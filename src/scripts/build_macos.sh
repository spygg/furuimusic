#!/usr/bin/env bash
# 福瑞音乐 - macOS 桌面构建（增量，不清理产物）
set -e
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SRC="$ROOT/src"
QT_DIR="${QT_DIR:-$HOME/Qt/5.15.2/clang_64}"
export PATH="$QT_DIR/bin:$PATH"

mkdir -p "$ROOT/build/macos"
cd "$ROOT/build/macos"
echo "[qmake] generating unified QML Makefile"
qmake "$SRC/FuruiMusic.pro" -o Makefile
echo "[make] building FuruiMusic.app"
make -j"$(sysctl -n hw.ncpu)"

if command -v cmake >/dev/null 2>&1; then
    echo "[cmake] building unified QML frontend"
    cmake -S "$SRC" -B "$ROOT/build/cmake" -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$QT_DIR"
    cmake --build "$ROOT/build/cmake" --target FuruiMusic -j"$(sysctl -n hw.ncpu)"
fi
echo "Build finished: $ROOT/build/macos, $ROOT/build/cmake"
