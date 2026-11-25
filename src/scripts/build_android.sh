#!/usr/bin/env bash
# 福瑞音乐 - Android APK（Qt 6 + CMake，Linux/macOS 主机；Windows 用 build_android_qt6.bat）
# 车机兼容测试版：FURUI_CAR_COMPAT=1 时构建网易云车机版包名
# com.netease.cloudmusic.iot + BYD 小迪语音对接的变体（独立构建目录
# build/android_qt6_carcompat，产物与主流版互不可覆盖安装）；车机上须先卸载官方
# 网易云。应用名「福瑞音乐」供设置里区分，语音关键词仍是「网易云音乐」。
# 详见 src/CMakeLists.txt）。
set -e
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SRC="$ROOT/src"
QT_VERSION="${QT_VERSION:-6.11.1}"
# macOS Qt ships clang_64, Linux uses gcc_64; the Android kit dir is host-independent.
UNAME_S="$(uname -s)"
case "$UNAME_S" in
    Darwin) QT_HOST_DEFAULT="$HOME/Qt/$QT_VERSION/clang_64" ;;
    *)      QT_HOST_DEFAULT="$HOME/Qt/$QT_VERSION/gcc_64" ;;
esac
QT_ANDROID="${QT_ANDROID:-$HOME/Qt/$QT_VERSION/android_arm64_v8a}"
QT_HOST="${QT_HOST:-$QT_HOST_DEFAULT}"
export ANDROID_SDK_ROOT="${ANDROID_SDK_ROOT:-$HOME/Android/Sdk}"
export ANDROID_NDK_ROOT="${ANDROID_NDK_ROOT:-$ANDROID_SDK_ROOT/ndk/27.1.12297006}"
# Honour an inherited JAVA_HOME; otherwise probe the usual Linux/macOS JDK 17 homes.
if [ -z "${JAVA_HOME:-}" ] || [ ! -x "$JAVA_HOME/bin/java" ]; then
    for candidate in \
        /usr/lib/jvm/java-17-openjdk-amd64 \
        /usr/lib/jvm/java-17-openjdk-arm64 \
        /usr/lib/jvm/java-17-openjdk \
        /Library/Java/JavaVirtualMachines/jdk-17.jdk/Contents/Home; do
        if [ -x "$candidate/bin/java" ]; then JAVA_HOME="$candidate"; break; fi
    done
    if [ -z "${JAVA_HOME:-}" ] && [ -x /usr/libexec/java_home ]; then
        JAVA_HOME="$(/usr/libexec/java_home -v 17 2>/dev/null || true)"
    fi
fi
if [ -z "${JAVA_HOME:-}" ] || [ ! -x "$JAVA_HOME/bin/java" ]; then
    echo "[error] JDK 17 not found; set JAVA_HOME to a JDK 17 installation." >&2
    exit 1
fi
export JAVA_HOME
export PATH="$QT_HOST/bin:$JAVA_HOME/bin:$PATH"
# Linux has nproc; macOS does not (sysctl is the portable way there).
case "$UNAME_S" in
    Darwin) JOB_COUNT="$(sysctl -n hw.ncpu 2>/dev/null || echo 4)" ;;
    *)      JOB_COUNT="$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)" ;;
esac

BUILD_DIR="$ROOT/build/android_qt6"
EXTRA=""
if [ "${FURUI_CAR_COMPAT:-0}" = "1" ]; then
    BUILD_DIR="$ROOT/build/android_qt6_carcompat"
    EXTRA="-DFURUI_ANDROID_CAR_COMPAT=ON"
    echo "[carcompat] 车机兼容测试版：包名 com.netease.cloudmusic.iot，BYD 对接已启用"
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
cmake --build "$BUILD_DIR" --target FuruiMusic_make_apk -j"$JOB_COUNT"
echo "APK output under: $BUILD_DIR/android-build/build/outputs/apk/"
