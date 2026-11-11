#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""汽水音乐（qishui / 豆乐）登录流程探针脚本。

用途：用 Python 直接打 汽水账号 API 服务的登录端点，逐步验证「出码 -> 扫码确认 -> 会话 -> 账号信息」
登录链路在什么阶段卡住；只读探针，不发起任何越界登录。

上游接口（来源：guowenye/qishui-api openapi.json /auth 实现）：
    GET  /auth/qrcode         -> { token, qrcode, qrcode_index_url, expire_time, ... }
    POST /auth/qrcode/status  -> { token, status, ... }    # 扫码确认后 auth.sessionid 生效
    POST /auth/me             -> { sessionid, ... }         # 换取当前登录账号信息

运行：
    python src/tools/qishui_login_flow_test.py [--base http://127.0.0.1:3300] [--token <已有token>] [--poll 6]
"""
import argparse
import json
import time
import urllib.error
import urllib.request

DEFAULT_BASE = "http://127.0.0.1:3300"


def request(method, base, path, body=None, timeout=25):
    url = base.rstrip("/") + path
    data = None
    headers = {"User-Agent": "FuruiMusicPythonProbe/1.0"}
    if body is not None:
        data = json.dumps(body).encode("utf-8")
        headers["Content-Type"] = "application/json"
    req = urllib.request.Request(url, data=data, headers=headers, method=method)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            raw = resp.read().decode("utf-8", "replace")
            try:
                parsed = json.loads(raw) if raw.strip() else None
            except Exception:
                parsed = {"raw": raw}
            return resp.status, parsed
    except urllib.error.HTTPError as e:
        raw = e.read().decode("utf-8", "replace")
        try:
            parsed = json.loads(raw) if raw.strip() else None
        except Exception:
            parsed = {"raw": raw, "status": e.code}
        return e.code, parsed
    except Exception as e:
        return None, {"error": "%s: %s" % (type(e).__name__, e)}


def main():
    ap = argparse.ArgumentParser(description="汽水音乐登录流程探针")
    ap.add_argument("--base", default=DEFAULT_BASE, help="账号 API 服务基地址，默认 %(default)s")
    ap.add_argument("--token", default="", help="已有 qrcode token 直接开始轮询（可选）")
    ap.add_argument("--poll", type=int, default=6, help="轮询次数（默认 6）")
    ap.add_argument("--interval", type=float, default=5, help="轮询间隔秒数（默认 5）")
    args = ap.parse_args()
    base = args.base

    print("[qishui-probe] base=%s" % base)

    token = args.token
    if not token:
        status, out = request("GET", base, "/auth/qrcode")
        print("[qishui-probe] GET /auth/qrcode -> status=%s" % status)
        print("[qishui-probe] body=%s" % json.dumps(out, ensure_ascii=False, default=str))
        token = (out or {}).get("token") if isinstance(out, dict) else ""

    final = None
    for i in range(max(1, args.poll)):
        if not token:
            print("[qishui-probe] 没有 token，跳过轮询")
            break
        status, out = request("POST", base, "/auth/qrcode/status", body={"token": token})
        print("[qishui-probe] POST /auth/qrcode/status[%d] -> status=%s" % (i + 1, status))
        print("[qishui-probe] body=%s" % json.dumps(out, ensure_ascii=False, default=str))
        final = out
        time.sleep(max(0, args.interval))

    data = (final or {}).get("data") if isinstance(final, dict) else None
    sessionid = ""
    if isinstance(data, dict):
        sessionid = data.get("sessionid") or final.get("sessionid") or ""
    elif isinstance(final, dict):
        sessionid = final.get("sessionid") or ""
    if sessionid:
        status, out = request("POST", base, "/auth/me", body={"sessionid": sessionid})
        print("[qishui-probe] POST /auth/me -> status=%s" % status)
        print("[qishui-probe] body=%s" % json.dumps(out, ensure_ascii=False, default=str))
    elif args.token:
        status, out = request("POST", base, "/auth/me", body={})
        print("[qishui-probe] POST /auth/me(no session) -> status=%s" % status)
        print("[qishui-probe] body=%s" % json.dumps(out, ensure_ascii=False, default=str))

    print("[qishui-probe] done")


if __name__ == "__main__":
    main()
