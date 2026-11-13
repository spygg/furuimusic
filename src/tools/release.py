#!/usr/bin/env python3
# 福瑞音乐 - 发版版本号同步脚本
#
# 版本号的唯一人工来源是 src/core/Version.h 的 FURUI_APP_VERSION 宏；
# 其余各处全部由此脚本派生，消除发版时四处手改的同步负担：
#   1. src/core/Version.h            改宏（唯一人工编辑点，也可只跑本脚本顺带改）
#   2. src/CMakeLists.txt            不用改：configure 期 file(READ) 自动注入 project VERSION
#   3. src/version.json              version 字段 + downloads 四条 URL 由本脚本重写
#   4. publish/version.json          兼容副本，本脚本从 3 复制并校验一致
#
# desc/history（发版说明）是人工内容，本脚本不碰——发版时手工编辑 version.json 后跑：
#   python src/tools/release.py 1.4.4
# 脚本会改 Version.h、同步两份清单、校验一致，并打印后续 git 提交/打 tag 命令。
import json
import re
import shutil
import sys
from pathlib import Path

# GBK 控制台下 print 中文可能直接 UnicodeEncodeError 崩掉，降级为替换输出
if hasattr(sys.stdout, "reconfigure"):
    try:
        sys.stdout.reconfigure(errors="replace")
    except Exception:
        pass

ROOT = Path(__file__).resolve().parents[1]  # src/
VERSION_H = ROOT / "core" / "Version.h"
MANIFEST = ROOT / "version.json"
MANIFEST_COPY = ROOT.parent / "publish" / "version.json"

DOWNLOAD_TEMPLATES = {
    "windows": "https://github.com/spygg/furuimusic/releases/download/v{v}/FuruiMusic-windows-x64.zip",
    "linux": "https://github.com/spygg/furuimusic/releases/download/v{v}/FuruiMusic-linux-x86_64.AppImage",
    "macos": "https://github.com/spygg/furuimusic/releases/download/v{v}/FuruiMusic-macos-x86_64.zip",
    "android": "https://github.com/spygg/furuimusic/releases/download/v{v}/FuruiMusic-android-arm64-v8a.apk",
}


def fail(msg):
    print("ERROR: %s" % msg)
    sys.exit(1)


def read_version_from_header():
    text = VERSION_H.read_text(encoding="utf-8")
    m = re.search(r'#define FURUI_APP_VERSION "(\d+\.\d+\.\d+)"', text)
    if not m:
        fail("Version.h 中找不到 FURUI_APP_VERSION 宏")
    return m.group(1), text


def bump_header(new_version):
    _, text = read_version_from_header()
    updated = re.sub(r'#define FURUI_APP_VERSION "\d+\.\d+\.\d+"',
                     '#define FURUI_APP_VERSION "%s"' % new_version, text)
    if updated != text:
        VERSION_H.write_text(updated, encoding="utf-8", newline="\n")
        print("Version.h: 版本号已改为 %s" % new_version)
    else:
        print("Version.h: 已是 %s，无需改动" % new_version)


def sync_manifest(new_version):
    if not MANIFEST.exists():
        fail("找不到 %s" % MANIFEST)
    data = json.loads(MANIFEST.read_text(encoding="utf-8"))
    data["version"] = new_version
    data["downloads"] = {k: tpl.format(v=new_version) for k, tpl in DOWNLOAD_TEMPLATES.items()}
    text = json.dumps(data, ensure_ascii=False, indent=2) + "\n"
    MANIFEST.write_text(text, encoding="utf-8", newline="\n")
    if MANIFEST_COPY.exists():
        shutil.copyfile(MANIFEST, MANIFEST_COPY)
        if MANIFEST_COPY.read_text(encoding="utf-8") != text:
            fail("publish/version.json 与 src/version.json 不一致")
        print("publish/version.json: 已同步（与 src/version.json 一致）")
    else:
        print("publish/version.json: 不存在，跳过兼容副本")
    print("version.json: version=%s，downloads 四条 URL 已指向 v%s" % (new_version, new_version))


def main():
    if len(sys.argv) != 2:
        print("用法: python src/tools/release.py <版本号>   例如: python src/tools/release.py 1.4.4")
        sys.exit(2)
    new_version = sys.argv[1].strip()
    if not re.fullmatch(r"\d+\.\d+\.\d+", new_version):
        fail("版本号格式应为 x.y.z，收到: %r" % new_version)

    old_version, _ = read_version_from_header()
    bump_header(new_version)
    sync_manifest(new_version)

    print()
    print("后续步骤（desc/history 发版说明请在 version.json 里手工编辑）:")
    print('  git add src/core/Version.h src/version.json publish/version.json')
    print('  git commit -m "release: v%s"' % new_version)
    print('  git tag -a v%s -m "v%s"' % (new_version, new_version))
    print("  git push origin main v%s   # tag 触发 CI 发布" % new_version)


if __name__ == "__main__":
    main()
