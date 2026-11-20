#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""给鱼缸建/查 PassNAT 隧道（TCP -> NAS 8083 websockets）

用法:
    python tools/passnat_tunnel.py list
    python tools/passnat_tunnel.py create
    python tools/passnat_tunnel.py config <tunnel_id>
"""
import json
import sys

sys.path.insert(0, __file__.rsplit("\\", 1)[0])
import passnat_api as api

NAME = "fishtank-mqtt"
LOCAL_IP = "127.0.0.1"
LOCAL_PORT = 8083


def token():
    return api.load_token() or api.login()


def list_all():
    code, raw = api.call("GET", "/user/tunnel/list?page=1&size=20", token=token())
    if code != 200:
        print("HTTP", code, raw[:200])
        return []
    d = json.loads(raw)
    if d.get("code") != 10000:
        print("code", d.get("code"), d.get("msg"))
        return []
    return d.get("data") or []


def find(name):
    for t in list_all():
        if t.get("name") == name:
            return t
    return None


def show(t):
    node = t.get("node_info") or {}
    print("  tunnel_id = %s  name=%s  status=%s" % (t.get("tunnel_id"), t.get("name"), t.get("status")))
    print("  node      = %s (node_id=%s) ip=%s ports=%s-%s" % (
        node.get("name"), node.get("node_id"), node.get("remote_ip"),
        node.get("start_port"), node.get("end_port")))
    for p in t.get("proxy_info") or []:
        print("   - %s %s:%s -> %s:%s%s" % (
            p.get("protocol"), node.get("remote_ip"), p.get("remote_port"),
            p.get("local_ip"), p.get("local_port"),
            ("  [" + p.get("remark") + "]") if p.get("remark") else ""))


def create():
    t = find(NAME)
    if t:
        print("已存在，不重复创建：")
        show(t)
        return t
    payload = {
        "tunnel_name": NAME,
        "node_id": 0,
        "port_relation": [{
            "local_ip": LOCAL_IP,
            "local_port": LOCAL_PORT,
            "protocol": "tcp",
            "remote_port": 0,
            "sub_domain": "",
            "domain_id": 0,
            "http_user": "",
            "http_pass": "",
            "crt_path": "",
            "key_path": "",
            "remark": "fish tank mqtt ws",
        }],
    }
    code, raw = api.call("POST", "/user/tunnel/add", payload, token=token())
    print("HTTP", code)
    print(raw[:600])
    t = find(NAME)
    if t:
        print("创建成功：")
        show(t)
    return t


if __name__ == "__main__":
    cmd = sys.argv[1] if len(sys.argv) > 1 else "list"
    if cmd == "list":
        for t in list_all():
            show(t)
    elif cmd == "create":
        create()
    elif cmd == "config":
        code, raw = api.call("GET", "/user/tunnel/config?tunnel_id=" + sys.argv[2], token=token())
        print("HTTP", code)
        print(raw[:4000])
