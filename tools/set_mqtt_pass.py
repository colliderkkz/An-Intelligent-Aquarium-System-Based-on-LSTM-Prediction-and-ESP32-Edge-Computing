#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""通过调试串口改板子的 MQTT 密码并重启（密码轮换用）

用法: python tools/set_mqtt_pass.py <新密码>
"""
import sys
import time

import fish_tank_debugger as ft

PORT = "COM6"
NEW = sys.argv[1] if len(sys.argv) > 1 else ""

frames = []


def on_frame(f):
    frames.append(f)


def show(tag):
    for f in frames:
        kind = getattr(f, "kind", "?")
        verb = getattr(f, "verb", "")
        txt = " ".join(str(x) for x in getattr(f, "fields", []))
        print("  [%s/%s] %s %s" % (tag, kind, verb, txt[:160]))
    frames.clear()


def pump(c, secs):
    t = time.time() + secs
    while time.time() < t:
        c.pump()
        time.sleep(0.02)


def main():
    if not NEW:
        print("需要新密码")
        return 1
    c = ft.DeviceClient(on_frame, lambda k, p: None)
    c.connect(PORT, 115200)
    print("已连接，等板子就绪…")
    pump(c, 6)
    show("boot")

    print("-> NET,SET,mqtt_pass,***")
    c.send("NET", "SET", "mqtt_pass", NEW)
    pump(c, 1.5)
    show("set")

    print("-> NET,SAVE")
    c.send("NET", "SAVE")
    pump(c, 1.5)
    show("save")

    print("-> NET,STATUS")
    c.send("NET", "STATUS")
    pump(c, 2.0)
    show("status")

    c.disconnect()
    print("完成（板子需要重启才生效）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
