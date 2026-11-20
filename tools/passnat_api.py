#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""PassNAT 控制台 API 小工具（登录 + 探接口）

用法:
    python tools/passnat_api.py login
    python tools/passnat_api.py get /user/userInfo
    python tools/passnat_api.py get "/super/tun/list?page=1&size=20"
    python tools/passnat_api.py post /super/tun '{"name":"x"}'
"""
import http.cookiejar
import json
import os
import sys
import urllib.request

BASE = "https://api.passnat.com/"
TOKEN_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "_passnat_token.txt")
PHONE = os.environ.get("PASSNAT_PHONE")
PASSWORD = os.environ.get("PASSNAT_PASSWORD")

cj = http.cookiejar.CookieJar()
opener = urllib.request.build_opener(urllib.request.HTTPCookieProcessor(cj))


def call(method, path, data=None, token=None, timeout=25):
    url = BASE + path.lstrip("/")
    body = None
    headers = {
        "Accept": "application/json, text/plain, */*",
        "Origin": "https://dashboard.passnat.com",
        "Referer": "https://dashboard.passnat.com/",
        "User-Agent": "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
                      "(KHTML, like Gecko) Chrome/140.0 Safari/537.36",
    }
    if data is not None:
        body = json.dumps(data).encode()
        headers["Content-Type"] = "application/json"
    if token:
        headers["Authorization"] = "Bearer " + token
        headers["token"] = token
    req = urllib.request.Request(url, data=body, headers=headers, method=method)
    try:
        with opener.open(req, timeout=timeout) as r:
            raw = r.read().decode("utf-8", "replace")
            return r.status, raw
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode("utf-8", "replace")


def login():
    if not PHONE or not PASSWORD:
        raise SystemExit("请先设置 PASSNAT_PHONE 和 PASSNAT_PASSWORD")
    code, raw = call("POST", "/public/login",
                     {"phone_number": PHONE, "password": PASSWORD, "platform": 1})
    print("HTTP", code)
    try:
        d = json.loads(raw)["data"]
    except Exception:
        return None
    tok = d.get("auth_token", "")
    with open(TOKEN_FILE, "w", encoding="utf-8") as f:
        f.write(tok)
    print("token saved, len", len(tok))
    print("cookies:", [c.name for c in cj])
    return tok


def load_token():
    try:
        return open(TOKEN_FILE, encoding="utf-8").read().strip()
    except Exception:
        return ""


if __name__ == "__main__":
    cmd = sys.argv[1] if len(sys.argv) > 1 else "login"
    if cmd == "login":
        login()
    else:
        tok = load_token() or login()
        path = sys.argv[2]
        data = None
        if cmd == "post":
            data = json.loads(sys.argv[3])
        code, raw = call("GET" if cmd == "get" else "POST", path, data, token=tok)
        print("HTTP", code)
        print(raw[:3000])
