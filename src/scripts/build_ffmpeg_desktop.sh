#!/usr/bin/env sh
# 福瑞音乐 - Qt 5 桌面「自编译 FFmpeg 播放后端」构建脚本
#
# 为什么不能直接用 reference/*_build_script 里的 FFmpeg：
#   那四套脚本是 QtRemoteDesktop 的**屏幕编码**链路，configure 里带
#   --disable-avformat --disable-swresample --disable-everything，只保留
#   swscale + h264 编码器（openh264 / NVENC / MediaCodec）。没有解封装、
#   没有音频解码，根本打不开 MP4。这里沿用它的三件事：
#     1. 源码放在 thirdparty/ffmpeg/FFmpeg-n<version>（本工程已有 6.1.2）
#     2. Windows 用 Git Bash 跑 configure + MinGW 工具链
#     3. 增量：产物已存在就直接复用，默认绝不删除任何已有构建产物
#   其余（组件裁剪、TLS 后端、输出目录）都按「播放」重新设计。
#
# 产物：<repo>/build_output/ffmpeg-qt5/{include,lib}
#   libavformat.a libavcodec.a libavutil.a libswscale.a libswresample.a
#
# 用法（任意平台，POSIX sh）：
#   sh src/scripts/build_ffmpeg_desktop.sh              # 增量，已有产物则跳过
#   sh src/scripts/build_ffmpeg_desktop.sh -j 16        # 指定并行度
#   FURUI_FFMPEG_ROOT=/opt/ffmpeg sh ...                # 自定义产物目录
#   FURUI_FFMPEG_SRC=/path/FFmpeg  sh ...                # 自定义源码目录
#   FURUI_FFMPEG_TLS=none sh ...                        # 不编 https（仅本地文件）
#   FURUI_FFMPEG_RECONFIGURE=1 sh ...                   # 强制重跑 configure（不删产物）
#   FURUI_FFMPEG_CC=E:/Qt/Tools/mingw530_32/bin/gcc.exe # 指定与 Qt 相同的 gcc
#   FURUI_FFMPEG_STAMP_FILE=/path/toolchain.txt sh ...  # CMake 传来的工具链指纹
#
# Windows 需要 Git Bash（提供 sh/perl/nasm）；MinGW 工具链可用
#   FURUI_MINGW_BIN=E:/programes/Qt/Qt/Tools/mingw810_64/bin 指定。
# 主构建脚本 build_windows.bat 在 FURUI_BUILD_QT5=1 时会自动调用本脚本。
# CMake 的 Qt5 桌面构建也会在 configure 阶段调用本脚本（见
# src/cmake/FuruiFfmpeg.cmake），并传入 FURUI_FFMPEG_CC / STAMP_FILE，
# 让 FFmpeg 静态库与主程序使用完全相同的编译器 ABI。

set -eu

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
SRC_DIR=$(cd "$SCRIPT_DIR/.." && pwd)
ROOT_DIR=$(cd "$SRC_DIR/.." && pwd)

FFMPEG_VERSION=${FURUI_FFMPEG_VERSION:-6.1.2}
FFMPEG_SRC=${FURUI_FFMPEG_SRC:-"$SRC_DIR/thirdparty/ffmpeg/FFmpeg-n$FFMPEG_VERSION"}
FFMPEG_OUT=${FURUI_FFMPEG_ROOT:-"$ROOT_DIR/build_output/ffmpeg-qt5"}
FFMPEG_BUILD=${FURUI_FFMPEG_BUILD:-"$ROOT_DIR/build/ffmpeg-qt5"}
JOBS=${FURUI_FFMPEG_JOBS:-8}
case "${1:-}" in -j) JOBS=${2:-8}; shift 2 ;; esac

UNAME_S=$(uname -s)
case "$UNAME_S" in
    MINGW*|MSYS*|CYGWIN*) PLATFORM=windows ;;
    Darwin*)              PLATFORM=macos ;;
    *)                    PLATFORM=linux ;;
esac

# POSIX 路径：Git Bash 下 pwd 已是 /g/... 形式；用户传 Windows 路径时用 cygpath 转
to_unix_path() {
    _p=$1
    case "$_p" in
        [A-Za-z]:[/\\]*)
            if command -v cygpath >/dev/null 2>&1; then cygpath -u "$_p"
            else printf '%s' "$_p" | sed -e 's#\\#/#g' -e 's#^\([A-Za-z]\):#/\L\1#' ; fi ;;
        *) printf '%s' "$_p" ;;
    esac
}
FFMPEG_SRC=$(to_unix_path "$FFMPEG_SRC")
FFMPEG_OUT=$(to_unix_path "$FFMPEG_OUT")
FFMPEG_BUILD=$(to_unix_path "$FFMPEG_BUILD")

echo "=============================================================="
echo " FuruiMusic Qt5 FFmpeg playback backend"
echo "  platform : $PLATFORM ($UNAME_S / $(uname -m))"
echo "  source   : $FFMPEG_SRC"
echo "  build    : $FFMPEG_BUILD"
echo "  install  : $FFMPEG_OUT"
echo "=============================================================="

if [ ! -f "$FFMPEG_SRC/configure" ]; then
    echo "错误: 找不到 FFmpeg 源码 $FFMPEG_SRC/configure" >&2
    echo "请确认 src/thirdparty/ffmpeg/FFmpeg-n$FFMPEG_VERSION 已就位" >&2
    exit 1
fi

# 增量约定：五个库齐全且公共头已装就复用，绝不删除已有产物
LIBS_OK=1
for _lib in libavformat libavcodec libavutil libswscale libswresample; do
    [ -f "$FFMPEG_OUT/lib/$_lib.a" ] || LIBS_OK=0
done
[ -f "$FFMPEG_OUT/include/libavutil/avutil.h" ] || LIBS_OK=0
if [ "$LIBS_OK" -eq 1 ] && [ "${FURUI_FFMPEG_RECONFIGURE:-0}" != "1" ]; then
    echo ">>> FFmpeg 产物已存在，跳过编译（增量构建）: $FFMPEG_OUT/lib"
    if [ ! -f "$FFMPEG_OUT/.toolchain" ]; then
        echo "    [note] 该目录没有 .toolchain 指纹（旧脚本产物）；CMake 无法核验 ABI，"
        echo "           会自动改用带工具链 tag 的目录重编，旧产物保持不动。"
    fi
    echo "    需要重配请用 FURUI_FFMPEG_RECONFIGURE=1（不会删除已有产物）"
    exit 0
fi

mkdir -p "$FFMPEG_BUILD" "$FFMPEG_OUT/lib" "$FFMPEG_OUT/include" "$FFMPEG_BUILD/tmp"

# Git Bash 下 Windows 的 TMPDIR 是 "C:\Users\...\Temp"（反斜杠），FFmpeg
# configure 的 sanity test 会拼出 "C:Users.../test.exe" 这种坏路径并报
# "Unable to create and execute files"。临时目录换到工程内的 POSIX 路径。
if [ "$PLATFORM" = "windows" ]; then
    TMPDIR="$FFMPEG_BUILD/tmp"
    TMP="$TMPDIR"
    TEMP="$TMPDIR"
    export TMPDIR TMP TEMP
fi

# ---- 工具链 ----------------------------------------------------------
# CMake 自动编译时会同时传入 FURUI_FFMPEG_CC（CMAKE_C_COMPILER 的原生路径）
# 与 FURUI_MINGW_BIN（编译器所在 bin）。这样 Qt 5.7 mingw53_32 这类 32 位
# 老工具链也能编出 ABI 一致的 .a，不会误用默认的 64 位 MinGW 8.1。
CC_BIN=cc
MAKE_BIN=make
case "$PLATFORM" in
    windows)
        # 与 Qt5 MinGW 用同一套 gcc，保证静态库 ABI 一致
        MINGW_BIN=${FURUI_MINGW_BIN:-}
        if [ -z "$MINGW_BIN" ] && [ -n "${FURUI_FFMPEG_CC:-}" ]; then
            MINGW_BIN=$(dirname "$FURUI_FFMPEG_CC")
        fi
        if [ -z "$MINGW_BIN" ]; then
            MINGW_BIN="/e/programes/Qt/Qt/Tools/mingw810_64/bin"
        fi
        MINGW_BIN=$(to_unix_path "$MINGW_BIN")
        if [ -x "$MINGW_BIN/gcc.exe" ]; then
            PATH="$MINGW_BIN:$PATH"
            export PATH
        fi
        # --cc 必须保留 Windows 原生路径（E:/...）：configure 生成的
        # config.mak 由原生 mingw32-make.exe 执行，它不认 /e/... 这类 MSYS 路径。
        CC_BIN=${FURUI_FFMPEG_CC:-gcc}
        if command -v mingw32-make >/dev/null 2>&1; then
            MAKE_BIN=$(command -v mingw32-make)
        elif [ -x "$MINGW_BIN/mingw32-make.exe" ]; then
            MAKE_BIN="$MINGW_BIN/mingw32-make.exe"
        else
            echo "错误: 找不到 mingw32-make，请设置 FURUI_MINGW_BIN" >&2
            exit 1
        fi
        # FFmpeg 的 Makefile 里有直接写 make 的子规则，放一个 shim 到 PATH 最前
        SHIM_DIR="$FFMPEG_BUILD/.tools"
        mkdir -p "$SHIM_DIR"
        printf '#!/bin/sh\nexec "%s" "$@"\n' "$MAKE_BIN" > "$SHIM_DIR/make"
        chmod +x "$SHIM_DIR/make"
        PATH="$SHIM_DIR:$PATH"
        export PATH
        ;;
    macos)  CC_BIN=${FURUI_FFMPEG_CC:-clang} ;;
    linux)  CC_BIN=${FURUI_FFMPEG_CC:-gcc} ;;
esac
echo ">>> cc=$CC_BIN  make=$MAKE_BIN"

# ---- TLS 后端：MV 地址都是 https MP4，必须能走 TLS ---------------------
# Windows 用系统 SChannel、macOS 用 SecureTransport，都不需要外部库；
# Linux 优先 OpenSSL（pkg-config 探测），没有则退回 --disable-https，
# 此时 Qt5 后端只能播本地文件 / http 流（CI 装 libssl-dev 即可恢复）。
TLS_FLAGS=""
case "$PLATFORM" in
    windows) TLS_FLAGS="--enable-schannel" ;;
    macos)   TLS_FLAGS="--enable-securetransport" ;;
    linux)
        if [ "${FURUI_FFMPEG_TLS:-auto}" = "openssl" ] ||
           { [ "${FURUI_FFMPEG_TLS:-auto}" = "auto" ] && command -v pkg-config >/dev/null 2>&1 && pkg-config --exists openssl; }; then
            TLS_FLAGS="--enable-openssl"
            echo ">>> TLS: OpenSSL"
        else
            TLS_FLAGS="--disable-openssl --disable-gnutls --disable-mbedtls"
            echo ">>> [warn] 未找到 OpenSSL 开发包，https 协议禁用；Qt5 后端仅能播本地文件/http"
        fi
        ;;
esac
if [ "${FURUI_FFMPEG_TLS:-auto}" = "none" ]; then
    TLS_FLAGS="--disable-openssl --disable-gnutls --disable-mbedtls --disable-schannel --disable-securetransport"
fi

# 原生 mingw32-make.exe 不认 MSYS 的 /g/... 挂载路径，prefix 要给 G:/... 形式
CFG_PREFIX="$FFMPEG_OUT"
if [ "$PLATFORM" = "windows" ] && command -v cygpath >/dev/null 2>&1; then
    CFG_PREFIX=$(cygpath -m "$FFMPEG_OUT")
fi

# ---- 播放所需的最小组件集 ---------------------------------------------
# --disable-everything 之后逐个点名，库体积与编译时间都远小于全量 FFmpeg。
# 覆盖：网易云 MV（MP4/H.264 + AAC）、常见 webm/flv/ts/mp3 源。
# httpproxy 不能省：环境里有 https_proxy 时，FFmpeg 的 https 要走 HTTP
# CONNECT 隧道，缺这个协议会直接报 "Protocol not found"（直连环境用不到，
# 但启用它几乎没有体积代价）。
set -- \
    --prefix="$CFG_PREFIX" \
    --cc="$CC_BIN" --ld="$CC_BIN" \
    --disable-debug --disable-doc --disable-programs \
    --disable-avdevice --disable-avfilter --disable-postproc \
    --disable-encoders --disable-muxers --disable-devices --disable-filters \
    --disable-hwaccels --disable-autodetect \
    --disable-everything \
    --enable-avformat --enable-avcodec --enable-avutil \
    --enable-swscale --enable-swresample \
    --enable-network \
    --enable-static --disable-shared --enable-pic \
    --enable-protocol=file,pipe,fd,http,https,tcp,tls,crypto,data,httpproxy \
    --enable-demuxer=mov,matroska,flv,avi,mpegts,hls,mp3,aac,wav,ogg,asf \
    --enable-parser=h264,hevc,aac,aac_latm,mpeg4video,mpegaudio,vp8,vp9,opus,flac \
    --enable-decoder=h264,hevc,mpeg4,vp8,vp9,mjpeg,aac,mp3,mp3float,opus,vorbis,flac,pcm_s16le,ac3,eac3 \
    --enable-bsf=h264_mp4toannexb,hevc_mp4toannexb,aac_adtstoasc \
    $TLS_FLAGS

echo ">>> configure ..."
( cd "$FFMPEG_BUILD" && "$FFMPEG_SRC/configure" "$@" ) > "$FFMPEG_BUILD/configure.log" 2>&1 || {
    echo "错误: FFmpeg configure 失败，日志尾部：" >&2
    tail -40 "$FFMPEG_BUILD/configure.log" >&2
    exit 1
}
echo ">>> configure 通过（日志: $FFMPEG_BUILD/configure.log）"

# configure 从 $0 推导的 source_path 是 /g/...（MSYS 形式），原生
# mingw32-make 把它当相对路径解析会直接报 "No such file or directory"。
# 只有这两处是绝对路径，改成 G:/... 形式（MSYS 工具和原生 make 都认）。
if [ "$PLATFORM" = "windows" ] && command -v cygpath >/dev/null 2>&1; then
    WIN_SRC=$(cygpath -m "$FFMPEG_SRC")
    sed -i "s#^SRC_PATH=.*#SRC_PATH=$WIN_SRC#" "$FFMPEG_BUILD/ffbuild/config.mak"
    sed -i "1s#^include .*#include $WIN_SRC/Makefile#" "$FFMPEG_BUILD/Makefile"
    echo ">>> Windows 路径修正: SRC_PATH=$WIN_SRC"
fi

echo ">>> make -j$JOBS ..."
"$MAKE_BIN" -C "$FFMPEG_BUILD" -j"$JOBS" > "$FFMPEG_BUILD/build.log" 2>&1 || {
    echo "错误: FFmpeg 编译失败，日志尾部：" >&2
    tail -40 "$FFMPEG_BUILD/build.log" >&2
    exit 1
}

# 不用 make install：Windows 上 MSYS 的 install 会把 G:/... 形式的路径拆坏
# （"target 'G:/.../src/thirdp' is not a directory"）。reference 的 mac
# 脚本也是手工拷头，这里统一成跨平台一致的做法。
echo ">>> 安装静态库与公共头 ..."
mkdir -p "$FFMPEG_OUT/lib/pkgconfig"
for _lib in libavformat libavcodec libavutil libswscale libswresample; do
    mkdir -p "$FFMPEG_OUT/include/$_lib"
    cp "$FFMPEG_SRC/$_lib"/*.h "$FFMPEG_OUT/include/$_lib/" 2>/dev/null || true
    cp "$FFMPEG_BUILD/$_lib/$_lib.a" "$FFMPEG_OUT/lib/"
    [ -f "$FFMPEG_BUILD/$_lib/$_lib.pc" ] && \
        cp "$FFMPEG_BUILD/$_lib/$_lib.pc" "$FFMPEG_OUT/lib/pkgconfig/"
done
# configure 生成的公共头不在源码树里
for _hdr in avconfig.h ffversion.h; do
    [ -f "$FFMPEG_BUILD/libavutil/$_hdr" ] && \
        cp "$FFMPEG_BUILD/libavutil/$_hdr" "$FFMPEG_OUT/include/libavutil/"
done

for _lib in libavformat libavcodec libavutil libswscale libswresample; do
    if [ ! -f "$FFMPEG_OUT/lib/$_lib.a" ]; then
        echo "错误: 缺少产物 $FFMPEG_OUT/lib/$_lib.a" >&2
        exit 1
    fi
done

# CMake 传入的工具链指纹：只有本次真的编译安装过才落盘，用于下次
# configure 判断能否复用（32/64 位、不同 gcc 的静态库绝不能混链）。
# 手工跑脚本时没有这个变量，产物目录保持无标记，CMake 会保守重编一份。
if [ -n "${FURUI_FFMPEG_STAMP_FILE:-}" ]; then
    STAMP_SRC=$(to_unix_path "$FURUI_FFMPEG_STAMP_FILE")
    if [ -f "$STAMP_SRC" ]; then
        cp "$STAMP_SRC" "$FFMPEG_OUT/.toolchain"
        echo ">>> 工具链指纹: $FFMPEG_OUT/.toolchain"
    fi
fi

echo ">>> 完成: $FFMPEG_OUT"
ls -1 "$FFMPEG_OUT/lib"
