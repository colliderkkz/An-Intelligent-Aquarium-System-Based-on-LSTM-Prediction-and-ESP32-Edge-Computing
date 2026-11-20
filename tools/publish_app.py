#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""发布页面到 MQTT（供 App 外网热更新）

用法:
    python tools/publish_app.py            # 用当前时间做版本号
    python tools/publish_app.py 202610301500

做的事：
  1) 读取 tools/phone_console.html
  2) 把里面的 var BOOT_VER = 0; 替换成真实版本号
  3) 按 8000 字符切块，retain 发布到 <prefix>/app/c/<i>
  4) retain 发布 <prefix>/app/meta = {"v":版本, "n":块数, "size":字节数}

板子现在同时连着两个 broker（家里 NAS + 公网直连），NAS 上的桥接已经关掉，
所以页面必须**两边都发**：家里手机从 NAS 拿，外网手机从公网拿。
"""
import json
import os
import sys
import time
from datetime import datetime

import paho.mqtt.client as mqtt

PREFIX = "fishtank-eed4"
CHUNK = 8000

# (host, port, user, password)  —— None 表示匿名
# 现在只有一台 broker：家里 NAS 的 Mosquitto。
# 外网访问靠 NAS 上的 PassNAT 隧道（mqtt.example.invalid:43940 -> NAS:8083），
# 手机连隧道拿到的就是同一份 retained 消息，所以这里只需要发一次。
# 最后一项 True 表示走 MQTT over WebSocket（PassNAT 隧道只开 ws）
BROKERS = [
    (os.environ.get("MQTT_HOST", "localhost"), int(os.environ.get("MQTT_PORT", "1883")),
     os.environ.get("MQTT_USER"), os.environ.get("MQTT_PASS"),
     os.environ.get("MQTT_TRANSPORT") == "websockets"),
]

HERE = os.path.dirname(os.path.abspath(__file__))
HTML = os.path.join(HERE, "phone_console.html")


def publish_to(host, port, user, password, chunks, meta, ws=False):
    if ws:
        c = mqtt.Client(client_id="publisher-%d" % int(time.time() * 1000),
                        protocol=mqtt.MQTTv311, transport="websockets")
        c.ws_set_options(path="/mqtt")
    else:
        c = mqtt.Client(client_id="publisher-%d" % int(time.time() * 1000),
                        protocol=mqtt.MQTTv311)
    if user:
        c.username_pw_set(user, password)
    try:
        c.connect(host, port, 20)
    except Exception as e:
        print("  [x] 连不上 %s: %s" % (host, e))
        return False
    c.loop_start()
    time.sleep(0.6)

    # 先清掉旧的 meta，避免手机拿到半新半旧的版本
    c.publish("%s/app/meta" % PREFIX, "", qos=0, retain=True)
    time.sleep(0.3)

    for i, chunk in enumerate(chunks):
        info = c.publish("%s/app/c/%d" % (PREFIX, i), chunk, qos=0, retain=True)
        info.wait_for_publish()
        if (i + 1) % 20 == 0 or i + 1 == len(chunks):
            print("  %s %d/%d" % (host, i + 1, len(chunks)))

    # 清掉可能残留的旧分片（版本变大时块数可能变少）
    for i in range(len(chunks), len(chunks) + 40):
        c.publish("%s/app/c/%d" % (PREFIX, i), "", qos=0, retain=True)

    info = c.publish("%s/app/meta" % PREFIX, meta, qos=0, retain=True)
    info.wait_for_publish()
    time.sleep(0.8)
    c.loop_stop()
    c.disconnect()
    print("  [ok] %s -> %s" % (host, meta))
    return True


def main():
    ver = 0
    if len(sys.argv) > 1:
        try:
            ver = int(sys.argv[1])
        except ValueError:
            print("版本号必须是整数")
            return 1
    if not ver:
        ver = int(datetime.now().strftime("%Y%m%d%H%M"))

    if not os.path.exists(HTML):
        print("找不到", HTML)
        return 1

    with open(HTML, encoding="utf-8") as f:
        html = f.read()

    marker = "var BOOT_VER = 0;"
    if marker in html:
        html = html.replace(marker, "var BOOT_VER = %d;" % ver, 1)
    else:
        print("警告: 没找到 var BOOT_VER = 0;，跳过版本注入")

    chunks = [html[i:i + CHUNK] for i in range(0, len(html), CHUNK)]
    meta = json.dumps({"v": ver, "n": len(chunks), "size": len(html)},
                      ensure_ascii=False)
    print("页面 %.0f KB -> %d 块, 版本 %d" % (len(html.encode("utf-8")) / 1024,
                                              len(chunks), ver))

    # 把注入了版本号的页面写回本地，供下次打包 APK 使用。
    # 这样 APK 内置页面的版本号与线上版本一致，
    # 启动时就不必再 document.write 重写整个文档（那是启动卡顿的来源）。
    try:
        root = os.path.dirname(HERE)
        out1 = os.path.join(HERE, "phone_console.published.html")
        with open(out1, "w", encoding="utf-8") as f:
            f.write(html)
        out2 = os.path.join(root, "android", "app", "src", "main", "assets", "index.html")
        if os.path.isdir(os.path.dirname(out2)):
            with open(out2, "w", encoding="utf-8") as f:
                f.write(html)
        out3 = os.path.join(HERE, "index.html")
        with open(out3, "w", encoding="utf-8") as f:
            f.write(html)
        print("  已同步 published / APK assets / tools/index.html")
    except Exception as e:
        print("  写回失败:", e)

    ok = 0
    for host, port, user, password, ws in BROKERS:
        if publish_to(host, port, user, password, chunks, meta, ws):
            ok += 1
    print("完成：%d/%d 个 broker 发布成功" % (ok, len(BROKERS)))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
