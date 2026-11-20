#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""临时探针：订阅本机/NAS broker 上的设备话题，打印 20 秒内的消息（调试用）。"""
import os
import sys, time, json
import paho.mqtt.client as mqtt

BROKER = sys.argv[1] if len(sys.argv) > 1 else os.environ.get("MQTT_HOST", "localhost")
SECS = float(sys.argv[2]) if len(sys.argv) > 2 else 12.0
TOPIC = sys.argv[3] if len(sys.argv) > 3 else "fishtank-eed4/#"

got = []


def on_connect(c, u, f, rc, props=None):
    print("connected rc=%s" % rc, flush=True)
    c.subscribe(TOPIC, 0)


def on_message(c, u, m):
    p = m.payload.decode("utf-8", "replace")
    if len(p) > 300:
        p = p[:300] + "...(%d)" % len(m.payload)
    print("%7.2f %s retain=%s qos=%d %s" % (time.time() - t0, m.topic, m.retain, m.qos, p), flush=True)
    got.append(m.topic)


c = mqtt.Client(client_id="probe-%d" % int(time.time() * 1000), protocol=mqtt.MQTTv311)
if os.environ.get("MQTT_USER"):
    c.username_pw_set(os.environ["MQTT_USER"], os.environ.get("MQTT_PASS"))
c.on_connect = on_connect
c.on_message = on_message
c.connect(BROKER, 1883, 20)
c.loop_start()
t0 = time.time()
time.sleep(SECS)
c.loop_stop()
c.disconnect()
print("共收到 %d 条，话题：%s" % (len(got), sorted(set(got))))
