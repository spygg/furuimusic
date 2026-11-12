#!/usr/bin/env bash
# ============================================================================
# Qt 6.8+ 在线仓库镜像安装器（绕过 aqtinstall，bash 版）
#
# 为什么需要它（参照 QtRemoteDesktop 的 install_qt_mirror.ps1）：
#  - Qt 6.11+ 桌面仓库布局从 <host>_x86/desktop/qt6_693/qt6_693/Updates.xml
#    改为 qt6_6111/qt6_6111_mingw/Updates.xml，aqtinstall 3.3.0（install-qt-action
#    默认 pin）仍按旧路径构造 URL，报 "Failed to locate XML data"；
#  - Android kit 自 Qt 6.8 起从 <host>_x86/android 移到 all_os/android/，
#    旧树只服务到 6.7.x 且内容已清空（aqt list-qt linux android --arch 全部
#    报 "No architectures available"，CI 上就是 Android job 的
#    "No latest version available for this request"）。
#
# 做法：直接读目标 group 的 Updates.xml，按 <DownloadableArchives> 逐个下载
# 7z 包，对照 .sha1 校验后解包到 <Operation name="Extract"> 声明的目标目录
# （@TargetDir@ 即安装根）。需要 curl + 7z（Ubuntu: p7zip-full）+ python3。
#
# 用法：
#   install_qt_mirror.sh --version 6.8.3 --outdir "$RUNNER_TEMP/Qt"
#   # 产物：<outdir>/<version>/android_arm64_v8a  （arm64-v8a 全套 + qtmultimedia）
# ============================================================================
set -euo pipefail

VERSION=''
OUTDIR=''
MIRROR='https://download.qt.io/online/qtsdkrepository'
# Android=基础 arm64-v8a kit；Multimedia=qt.qt6.*.addons.qtmultimedia（本项目 MV/播放依赖）
COMPONENTS='Android,Multimedia'
RETRIES=3

while [ $# -gt 0 ]; do
    case "$1" in
        --version) VERSION="$2"; shift 2 ;;
        --outdir)  OUTDIR="$2";  shift 2 ;;
        --mirror)  MIRROR="$2";  shift 2 ;;
        *) echo "[qt-mirror] ERROR: 未知参数 '$1'" >&2; exit 1 ;;
    esac
done
echo "$VERSION" | grep -Eq '^[0-9]+\.[0-9]+(\.[0-9]+)?$' \
    || { echo "[qt-mirror] ERROR: 版本号不合法: '$VERSION'" >&2; exit 1; }
[ -n "$OUTDIR" ] || { echo "[qt-mirror] ERROR: 需要 --outdir" >&2; exit 1; }

SEVENZ="$(command -v 7z || command -v 7za || true)"
[ -n "$SEVENZ" ] || { echo "[qt-mirror] ERROR: 找不到 7z（Ubuntu: apt-get install p7zip-full）" >&2; exit 1; }
command -v python3 >/dev/null || { echo "[qt-mirror] ERROR: 需要 python3 解析 Updates.xml" >&2; exit 1; }

NO_DOTS="${VERSION//./}"
DL="$OUTDIR/_downloads"
mkdir -p "$DL"

fetch() { # fetch <url> <dest>
    local i
    for i in $(seq 1 "$RETRIES"); do
        if curl -sSL --fail --connect-timeout 30 --retry 2 -o "$2" "$1"; then
            return 0
        fi
        echo "[qt-mirror] 下载失败($i/$RETRIES): $1" >&2
        rm -f "$2"
        sleep $((5 * i))
    done
    return 1
}

# install_group <groupUrl> <packageName> <defaultTarget>
install_group() {
    local groupUrl="$1" pkg="$2" defaultTarget="$3"
    local xml="$DL/$(echo "$pkg" | tr -c 'A-Za-z0-9._-' '_').Updates.xml"
    fetch "$groupUrl/Updates.xml" "$xml" \
        || { echo "[qt-mirror] ERROR: 无法获取 $groupUrl/Updates.xml" >&2; exit 1; }

    # 解析出 (版本号, 归档名, 解包目标) 三元组；@TargetDir@ 是安装根
    python3 - "$xml" "$pkg" "$defaultTarget" "$OUTDIR" > "$DL/plan.txt" <<'PY'
import re, sys
xml_path, pkg, default_target, outdir = sys.argv[1:5]
xml = open(xml_path, encoding="utf-8").read()
m = re.search(r'<Name>' + re.escape(pkg) + r'</Name>(.*?)</PackageUpdate>', xml, re.S)
if not m:
    sys.stderr.write("[qt-mirror] ERROR: 包 %s 不在该 group（Qt 版本或仓库布局变了？）\n" % pkg)
    sys.exit(2)
blk = m.group(1)
ver = re.search(r'<Version>([^<]+)</Version>', blk).group(1)
archives = [a.strip() for a in
            re.search(r'<DownloadableArchives>([^<]+)</DownloadableArchives>', blk).group(1).split(',')
            if a.strip()]
targets = {}
for op in re.findall(r'<Operation name="Extract">(.*?)</Operation>', blk, re.S):
    args = re.findall(r'<Argument>([^<]*)</Argument>', op)
    if len(args) >= 2:
        targets[args[1]] = args[0]
for a in archives:
    t = default_target
    if a in targets:
        t = targets[a].replace("@TargetDir@", outdir)
    print(ver + "\t" + a + "\t" + t)
PY

    # Windows python 的 print 是 \r\n 结尾，目标路径尾部会带 \r，必须剥掉
    tr -d '\r' < "$DL/plan.txt" > "$DL/plan.unix.txt" && mv "$DL/plan.unix.txt" "$DL/plan.txt"
    local n=0 total
    total="$(wc -l < "$DL/plan.txt")"
    while IFS=$'\t' read -r ver archive target; do
        n=$((n + 1))
        local url="$groupUrl/$pkg/$ver$archive"
        local dest="$DL/$archive"
        echo "[qt-mirror] [$n/$total] $archive"
        fetch "$url" "$dest" || { echo "[qt-mirror] ERROR: 下载失败 $url" >&2; exit 1; }
        # sha1 校验：镜像侧发布 <archive>.sha1，取不到即失败而非跳过
        fetch "$url.sha1" "$dest.sha1" || { echo "[qt-mirror] ERROR: 无法获取校验文件 $url.sha1" >&2; exit 1; }
        local expect actual
        expect="$(awk '{print tolower($1)}' "$dest.sha1")"
        actual="$(sha1sum "$dest" | awk '{print $1}')"
        rm -f "$dest.sha1"
        [ "$expect" = "$actual" ] || { echo "[qt-mirror] ERROR: SHA1 不匹配 $archive (期望 $expect, 实际 $actual)" >&2; exit 1; }
        mkdir -p "$target"
        "$SEVENZ" x "-o$target" -y "$dest" >/dev/null
        rm -f "$dest"
    done < "$DL/plan.txt"
    rm -f "$DL/plan.txt"
    echo "[qt-mirror] $pkg 安装完成（$total 个归档）"
}

echo "[qt-mirror] 版本     : $VERSION"
echo "[qt-mirror] 安装根   : $OUTDIR"
echo "[qt-mirror] 镜像     : $MIRROR"
echo "[qt-mirror] 组件     : $COMPONENTS"

for c in $(echo "$COMPONENTS" | tr ',' ' '); do
    case "$c" in
        Android)
            group="$MIRROR/all_os/android/qt6_$NO_DOTS/qt6_${NO_DOTS}_arm64_v8a"
            install_group "$group" "qt.qt6.$NO_DOTS.android_arm64_v8a" \
                "$OUTDIR/$VERSION/android_arm64_v8a"
            [ -f "$OUTDIR/$VERSION/android_arm64_v8a/bin/qt-cmake" ] \
                || { echo "[qt-mirror] ERROR: 未找到 qt-cmake，Android kit 安装不完整" >&2; exit 1; }
            ;;
        Multimedia)
            group="$MIRROR/all_os/android/qt6_$NO_DOTS/qt6_${NO_DOTS}_arm64_v8a"
            install_group "$group" "qt.qt6.$NO_DOTS.addons.qtmultimedia.android_arm64_v8a" \
                "$OUTDIR/$VERSION/android_arm64_v8a"
            find "$OUTDIR/$VERSION/android_arm64_v8a" -name 'Qt6MultimediaConfig.cmake' | grep -q . \
                || { echo "[qt-mirror] ERROR: 未找到 Qt6MultimediaConfig.cmake" >&2; exit 1; }
            ;;
        *) echo "[qt-mirror] ERROR: 未知组件 '$c'（支持: Android, Multimedia）" >&2; exit 1 ;;
    esac
done

echo "[qt-mirror] 全部组件已安装到 $OUTDIR"
