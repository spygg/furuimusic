#!/usr/bin/env bash
# 福瑞音乐 - Android APK（Qt 6 + CMake，Linux/macOS 主机；Windows 用 build_android_qt6.bat）
# 车机兼容测试版：FURUI_CAR_COMPAT=1 时构建包名伪装 com.netease.cloudmusic +
# BYD 小迪语音对接的变体（独立构建目录 build/android_qt6_carcompat，产物与主流版
# 互不可覆盖安装；详见 src/CMakeLists.txt 的 FURUI_ANDROID_CAR_COMPAT 说明）。
set -e
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SRC="$ROOT/src"
QT_ANDROID="${QT_ANDROID:-$HOME/Qt/6.11.1/android_arm64_v8a}"
QT_HOST="${QT_HOST:-$HOME/Qt/6.11.1/gcc_64}"
export ANDROID_SDK_ROOT="${ANDROID_SDK_ROOT:-$HOME/Android/Sdk}"
export ANDROID_NDK_ROOT="${ANDROID_NDK_ROOT:-$ANDROID_SDK_ROOT/ndk/27.1.12297006}"
export JAVA_HOME="${JAVA_HOME:-/usr/lib/jvm/java-17-openjdk-amd64}"
export PATH="$QT_HOST/bin:$JAVA_HOME/bin:$PATH"

BUILD_DIR="$ROOT/build/android_qt6"
EXTRA=""
if [ "${FURUI_CAR_COMPAT:-0}" = "1" ]; then
    BUILD_DIR="$ROOT/build/android_qt6_carcompat"
    EXTRA="-DFURUI_ANDROID_CAR_COMPAT=ON"
    echo "[carcompat] 车机兼容测试版：包名 com.netease.cloudmusic，BYD 对接已启用"
fi

# Qt6AndroidMacros 读的是 CMake 变量 ANDROID_SDK_ROOT（不是环境变量），
# 不传会报 "Could not locate Android SDK build tools under /build-tools"
cmake -S "$SRC" -B "$BUILD_DIR" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH="$QT_ANDROID" \
    -DQT_HOST_PATH="$QT_HOST" \
    -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_ROOT/build/cmake/android.toolchain.cmake" \
    -DANDROID_SDK_ROOT="$ANDROID_SDK_ROOT" \
    -DANDROID_ABI=arm64-v8a \
    -DANDROID_PLATFORM=android-23 \
    -DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=BOTH \
    $EXTRA
cmake --build "$BUILD_DIR" --target FuruiMusic_make_apk -j"$(nproc)"
echo "APK output under: $BUILD_DIR/android-build/build/outputs/apk/"
