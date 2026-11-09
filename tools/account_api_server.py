#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""福瑞音乐 - 账号 API 服务参考实现

用途：为 QQ音乐 / 酷狗 / 酷我 / 咪咕 这类「官方没有开放用户歌单 API」的平台，
提供一层自建的合规账号服务，让 Qt 客户端能用用户自己导出的 Cookie 读取
「我的歌单 / 我喜欢的音乐」。网易云不需要本服务（客户端已内置真实实现）。

启动（用于链路验证）：
    python tools/account_api_server.py --demo --port 9764
然后在客户端「设置 -> 网络 -> 账号 API 服务」填写 http://127.0.0.1:9764，
在登录页粘贴任意 Cookie（演示模式不校验），即可看到歌单与收藏被同步下来。

演示模式返回的数据带有 demo=true 标记，仅用于验证链路，不是真实账号数据。
接入真实数据时，请改写 Upstream 类，把它对接到你已获授权的数据源
（例如自建的 QQMusicApi 服务 /user/detail、/user/songlist、/user/collect/songlist），
不要在此实现任何绕过付费或 DRM 的逻辑。

统一契约（与 config/sources.json 的 userApiContract 一致）：
    GET /health
    GET /user/verify?source=&cookie=          -> {ok, profile:{uid,nickname,avatar}}
    GET /user/profile?source=&cookie=         -> {ok, profile:{uid,nickname,avatar}}
    GET /user/playlists?source=&cookie=&uid=&limit=&offset= -> {ok, playlists:[{id,title,cover,count,owner}]}
    GET /user/favorites?source=&cookie=&uid=&limit=         -> {ok, songs:[{id,title,artist,album,cover,duration}]}
    GET /user/playlistSongs?source=&cookie=&uid=&id=&limit= -> {ok, songs:[...]}
"""
import argparse
import json
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs

PLATFORMS = {
    "qq": "QQ音乐",
    "kugou": "酷狗音乐",
    "kuwo": "酷我音乐",
    "migu": "咪咕音乐",
    "netease": "网易云音乐",
}


def demo_song(source, index):
    return {
        "id": "%s_demo_%d" % (source, index),
        "title": "示例歌曲 %d" % index,
        "artist": "演示歌手 %d" % (index % 3 + 1),
        "album": "演示专辑",
        "cover": "",
        "duration": 180000 + index * 1000,
        "vip": False,
    }


class Upstream:
    """真实数据接入点：默认只有演示数据，接入你自己的合规服务时改写这里。"""

    def __init__(self, demo=False):
        self.demo = demo

    def profile(self, source, cookie):
        if not self.demo:
            return None, "未配置上游账号服务：请改写 Upstream 或加 --demo 验证链路"
        if not cookie:
            return None, "Cookie 为空"
        return {
            "uid": "%s_demo_user" % source,
            "nickname": "演示账号（%s）" % PLATFORMS.get(source, source),
            "avatar": "",
        }, None

    def playlists(self, source, uid, limit, offset):
        if not self.demo:
            return None, "未配置上游账号服务"
        names = ["我喜欢的音乐", "通勤路上", "深夜循环"]
        out = []
        for i, name in enumerate(names):
            out.append({
                "id": "%s_pl_%d" % (source, i + 1),
                "title": name,
                "cover": "",
                "count": 3,
                "owner": "演示账号",
            })
        return out[offset:offset + limit], None

    def favorites(self, source, uid, limit):
        if not self.demo:
            return None, "未配置上游账号服务"
        return [demo_song(source, i + 1) for i in range(min(limit, 3))], None

    def playlist_songs(self, source, uid, playlist_id, limit):
        if not self.demo:
            return None, "未配置上游账号服务"
        seed = sum(ord(c) for c in playlist_id) % 5
        return [demo_song(source, seed + i + 1) for i in range(min(limit, 3))], None


class Handler(BaseHTTPRequestHandler):
    upstream = None

    def log_message(self, fmt, *args):
        print("[account-api] " + (fmt % args))

    def send_json(self, obj, status=200):
        body = json.dumps(obj, ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        parsed = urlparse(self.path)
        query = {k: v[0] if v else "" for k, v in parse_qs(parsed.query).items()}
        path = parsed.path.rstrip("/") or "/"
        source = query.get("source", "")
        cookie = query.get("cookie", "") or self.headers.get("X-Account-Cookie", "")
        uid = query.get("uid", "")
        try:
            limit = max(1, min(int(query.get("limit", "30")), 500))
            offset = max(0, int(query.get("offset", "0")))
        except ValueError:
            limit, offset = 30, 0

        if path == "/health":
            return self.send_json({
                "ok": True, "demo": self.upstream.demo,
                "platforms": list(PLATFORMS.keys()),
                "note": "账号 API 服务契约参考实现",
            })

        if path in ("/user/verify", "/user/profile"):
            profile, err = self.upstream.profile(source, cookie)
            if err:
                return self.send_json({"ok": False, "error": err, "demo": self.upstream.demo})
            return self.send_json({"ok": True, "profile": profile, "demo": self.upstream.demo})

        if path == "/user/playlists":
            lists, err = self.upstream.playlists(source, uid, limit, offset)
            if err:
                return self.send_json({"ok": False, "error": err})
            return self.send_json({"ok": True, "playlists": lists, "demo": self.upstream.demo})

        if path == "/user/favorites":
            songs, err = self.upstream.favorites(source, uid, limit)
            if err:
                return self.send_json({"ok": False, "error": err})
            return self.send_json({"ok": True, "songs": songs, "demo": self.upstream.demo})

        if path == "/user/playlistSongs":
            songs, err = self.upstream.playlist_songs(source, uid, query.get("id", ""), limit)
            if err:
                return self.send_json({"ok": False, "error": err})
            return self.send_json({"ok": True, "songs": songs, "demo": self.upstream.demo})

        return self.send_json({"ok": False, "error": "未知端点 %s" % path}, 404)


def main():
    ap = argparse.ArgumentParser(description="福瑞音乐账号 API 服务参考实现")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=9764)
    ap.add_argument("--demo", action="store_true",
                    help="返回明确标注的演示数据，用于验证登录同步链路")
    args = ap.parse_args()
    Handler.upstream = Upstream(demo=args.demo)
    server = ThreadingHTTPServer((args.host, args.port), Handler)
    print("账号 API 服务已启动: http://%s:%d  demo=%s" % (args.host, args.port, args.demo))
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
