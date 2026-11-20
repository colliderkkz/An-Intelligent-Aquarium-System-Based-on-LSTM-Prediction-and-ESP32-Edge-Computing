#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""串口小工具：正常复位板子 / 抓串口日志

用法:
    python tools/serial_tool.py reset COM6
    python tools/serial_tool.py log COM6 8
    python tools/serial_tool.py watch COM6 8 --cmd hist     # 抓日志同时发一条命令
"""
import json
import os
import sys
import threading
import time

import serial


def reset(port):
    s = serial.Serial(port, 115200, timeout=0.2)
    # IO0 = DTR = False（正常启动），EN = RTS，拉低再放开即复位
    s.dtr = False
    time.sleep(0.05)
    s.rts = True
    time.sleep(0.15)
    s.rts = False
    time.sleep(0.3)
    s.close()
    print("reset done")


def capture(port, secs, cmd=None):
    buf = []

    def reader():
        s = serial.Serial(port, 115200, timeout=0.2)
        s.dtr = False
        s.rts = False
        t0 = time.time()
        while time.time() - t0 < secs:
            d = s.read(8192)
            if d:
                buf.append(d.decode("utf-8", "replace"))
        s.close()

    th = threading.Thread(target=reader)
    th.start()
    if cmd:
        time.sleep(1.5)
        import paho.mqtt.client as mqtt
        c = mqtt.Client(client_id="sv%d" % int(time.time()), protocol=mqtt.MQTTv311)
        if os.environ.get("MQTT_USER"):
            c.username_pw_set(os.environ["MQTT_USER"], os.environ.get("MQTT_PASS"))
        c.connect(os.environ.get("MQTT_HOST", "localhost"), 1883, 20)
        c.loop_start()
        time.sleep(0.6)
        c.publish("fishtank-eed4/ftcc3c78/cmd",
                  json.dumps(cmd) if isinstance(cmd, dict) else cmd, 0)
        time.sleep(0.5)
        c.loop_stop()
        c.disconnect()
    th.join()
    return "".join(buf)


if __name__ == "__main__":
    mode = sys.argv[1] if len(sys.argv) > 1 else "log"
    port = sys.argv[2] if len(sys.argv) > 2 else "COM6"
    if mode == "reset":
        reset(port)
    else:
        secs = float(sys.argv[3]) if len(sys.argv) > 3 else 8
        cmd = None
        if "--cmd" in sys.argv:
            i = sys.argv.index("--cmd")
            cmd = json.loads(sys.argv[i + 1])
        print(capture(port, secs, cmd))
