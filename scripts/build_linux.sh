#!/usr/bin/env bash
# 福瑞音乐 - Linux 桌面构建（增量，不清理产物）
set -e
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
QT_DIR="${QT_DIR:-$HOME/Qt/5.15.2/gcc_64}"
export PATH="$QT_DIR/bin:$PATH"
export LD_LIBRARY_PATH="$QT_DIR/lib:${LD_LIBRARY_PATH:-}"

mkdir -p "$ROOT/build/linux"
cd "$ROOT/build/linux"
echo "[qmake] generating unified QML Makefile"
qmake "$ROOT/FuruiMusic.pro" -o Makefile
echo "[make] building FuruiMusic (QML)"
make -j"$(nproc)"

if command -v cmake >/dev/null 2>&1; then
    echo "[cmake] building unified QML frontend"
    cmake -S "$ROOT" -B "$ROOT/build/cmake" -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$QT_DIR"
    cmake --build "$ROOT/build/cmake" --target FuruiMusic -j"$(nproc)"
fi
echo "Build finished: $ROOT/build/linux, $ROOT/build/cmake"
