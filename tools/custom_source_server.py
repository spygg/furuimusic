#!/usr/bin/env python3
# 福瑞音乐 - 自定义音源示例服务（仅用于演示/本地测试）
#
# 作用：把「你自己拥有合法授权的本地音乐目录」暴露成福瑞音乐可调用的 HTTP 音源，
#       用于替换 VIP / 不可播放的歌曲。不内置任何第三方音源，不访问任何在线音乐平台。
#
# 用法：
#   python tools/custom_source_server.py "D:/MyMusic" --port 9763
# 然后在 福瑞音乐 -> 设置 -> 自定义音源 -> 添加 HTTP 音源：
#   名称：本地示例音源
#   地址：http://127.0.0.1:9763
#
# 接口契约（与 config/sources.json 的 customSourceContract 一致）：
#   GET /musicUrl?name=&artist=&album=&id=&source=&quality=  -> {"url": "http://.../file/<id>"}
#   GET /search?keyword=&limit=                            -> {"list":[{id,title,artist,album,cover,duration}]}
#   GET /lyric?name=&artist=&id=                            -> {"lyric": "[00:00.00]..."}
#   GET /file/<id>                                          -> 音频文件流
#   GET /health                                             -> 状态
import argparse
import difflib
import json
import os
import re
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

AUDIO_EXT = (".mp3", ".flac", ".m4a", ".wav", ".ogg", ".aac")
LIBRARY = []          # [{id, path, title, artist, filename}]
LOCK = threading.Lock()


def normalize(text):
    text = (text or "").lower()
    text = re.sub(r"\([^)]*\)|\[[^]]*\]|（[^）]*）", "", text)
    return "".join(ch for ch in text if ch.isalnum())


def scan(root):
    found = []
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if not d.startswith(".")]
        for name in filenames:
            if not name.lower().endswith(AUDIO_EXT):
                continue
            path = os.path.join(dirpath, name)
            base = os.path.splitext(name)[0]
            title, artist = base, ""
            if " - " in base:
                artist, title = base.split(" - ", 1)
            found.append({
                "id": str(len(found)),
                "path": path,
                "title": title.strip(),
                "artist": artist.strip(),
                "filename": base,
            })
    with LOCK:
        LIBRARY.clear()
        LIBRARY.extend(found)
    return len(found)


def best_match(name, artist):
    target = normalize(name + " " + artist)
    best, score = None, 0.0
    with LOCK:
        items = list(LIBRARY)
    for item in items:
        cand = normalize(item["title"] + " " + item["artist"])
        ratio = difflib.SequenceMatcher(None, target, cand).ratio()
        if normalize(name) and normalize(name) in cand:
            ratio += 0.15
        if ratio > score:
            best, score = item, ratio
    return best if score >= 0.55 else None, score


class Handler(BaseHTTPRequestHandler):
    server_version = "FuruiCustomSource/1.0"

    def log_message(self, fmt, *args):
        pass

    def _send(self, code, body, ctype="application/json; charset=utf-8"):
        if isinstance(body, (dict, list)):
            body = json.dumps(body, ensure_ascii=False).encode("utf-8")
        elif isinstance(body, str):
            body = body.encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Access-Control-Allow-Origin", "*")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        parsed = urlparse(self.path)
        query = {k: v[0] for k, v in parse_qs(parsed.query).items()}
        path = parsed.path.rstrip("/") or "/"

        if path == "/health":
            with LOCK:
                count = len(LIBRARY)
            self._send(200, {"status": "ok", "tracks": count, "source": "local"})
            return

        if path == "/search":
            keyword = normalize(query.get("keyword", ""))
            limit = int(query.get("limit", "30") or 30)
            with LOCK:
                items = list(LIBRARY)
            result = [i for i in items if keyword in normalize(i["filename"])] if keyword else items
            self._send(200, {"list": [
                {"id": i["id"], "title": i["title"], "artist": i["artist"],
                 "album": "", "cover": "", "duration": 0} for i in result[:limit]
            ]})
            return

        if path == "/musicUrl":
            item, score = best_match(query.get("name", ""), query.get("artist", ""))
            if not item:
                self._send(200, {"url": "", "message": "no match"})
                return
            host = self.headers.get("Host", "127.0.0.1:9763")
            self._send(200, {"url": "http://%s/file/%s" % (host, item["id"]),
                             "title": item["title"], "artist": item["artist"],
                             "score": round(score, 3)})
            return

        if path == "/lyric":
            item, _ = best_match(query.get("name", ""), query.get("artist", ""))
            # 同名 .lrc 文件存在则直接返回，否则返回空歌词
            if item:
                lrc = os.path.splitext(item["path"])[0] + ".lrc"
                if os.path.isfile(lrc):
                    with open(lrc, "r", encoding="utf-8", errors="ignore") as f:
                        self._send(200, {"lyric": f.read()})
                        return
            self._send(200, {"lyric": ""})
            return

        if path.startswith("/file/"):
            sid = path[len("/file/"):]
            with LOCK:
                item = next((i for i in LIBRARY if i["id"] == sid), None)
            if not item or not os.path.isfile(item["path"]):
                self._send(404, {"error": "not found"})
                return
            size = os.path.getsize(item["path"])
            start = 0
            range_header = self.headers.get("Range")
            if range_header:
                m = re.match(r"bytes=(\d+)-", range_header)
                if m:
                    start = int(m.group(1))
            ext = os.path.splitext(item["path"])[1].lower()
            ctype = {".mp3": "audio/mpeg", ".flac": "audio/flac", ".m4a": "audio/mp4",
                     ".wav": "audio/wav", ".ogg": "audio/ogg", ".aac": "audio/aac"}.get(ext, "application/octet-stream")
            self.send_response(206 if start else 200)
            self.send_header("Content-Type", ctype)
            self.send_header("Accept-Ranges", "bytes")
            self.send_header("Content-Length", str(size - start))
            if start:
                self.send_header("Content-Range", "bytes %d-%d/%d" % (start, size - 1, size))
            self.send_header("Access-Control-Allow-Origin", "*")
            self.end_headers()
            with open(item["path"], "rb") as f:
                f.seek(start)
                while True:
                    chunk = f.read(65536)
                    if not chunk:
                        break
                    try:
                        self.wfile.write(chunk)
                    except (BrokenPipeError, ConnectionResetError):
                        break
            return

        self._send(404, {"error": "unknown endpoint",
                         "endpoints": ["/health", "/search", "/musicUrl", "/lyric", "/file/<id>"]})


def main():
    parser = argparse.ArgumentParser(description="福瑞音乐自定义音源示例服务")
    parser.add_argument("directory", help="你自己拥有合法授权的音乐目录")
    parser.add_argument("--port", type=int, default=9763)
    parser.add_argument("--host", default="127.0.0.1")
    args = parser.parse_args()
    if not os.path.isdir(args.directory):
        parser.error("目录不存在: %s" % args.directory)
    count = scan(args.directory)
    print("已扫描 %d 首本地音乐，服务地址 http://%s:%d" % (count, args.host, args.port))
    print("在福瑞音乐设置中添加 HTTP 自定义音源: http://%s:%d" % (args.host, args.port))
    server = ThreadingHTTPServer((args.host, args.port), Handler)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
