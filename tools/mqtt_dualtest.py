#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""双路联调：从公网 broker 下发一条命令，量回执延迟。
用法: python tools/mqtt_dualtest.py [broker] [act] [val]
"""
import os
import sys, time, json
import paho.mqtt.client as mqtt

BROKER = sys.argv[1] if len(sys.argv) > 1 else os.environ.get("MQTT_HOST", "localhost")
ACT = sys.argv[2] if len(sys.argv) > 2 else "pump"
VAL = sys.argv[3] if len(sys.argv) > 3 else "100"
PFX, DEV = "fishtank-eed4", "ftcc3c78"

t_pub = None
t_ack = None
t_state = None
states = []
dups = 0
last_payload = None
last_t = 0


def on_connect(c, u, f, rc, props=None):
    print("connected rc=%s" % rc, flush=True)
    c.subscribe("%s/%s/ack" % (PFX, DEV), 0)
    c.subscribe("%s/%s/state" % (PFX, DEV), 0)


def on_message(c, u, m):
    global t_ack, t_state, dups, last_payload, last_t
    now = time.time()
    txt = m.payload.decode("utf-8", "replace")
    if m.topic.endswith("/state"):
        states.append(now)
        if last_payload == txt and now - last_t < 1.0:
            dups += 1
        last_payload, last_t = txt, now
        try:
            s = json.loads(txt)
        except Exception:
            return
        if t_state is None and s.get(ACT + "_pct") is not None and int(s[ACT + "_pct"]) == int(VAL):
            t_state = now
            print("  state  %+6.0f ms  %s_pct=%s" % ((now - t_pub) * 1000, ACT, s[ACT + "_pct"]), flush=True)
    elif m.topic.endswith("/ack"):
        try:
            o = json.loads(txt)
        except Exception:
            return
        if o.get("cmd") == ACT:
            if t_ack is None:
                t_ack = now
                print("  ack    %+6.0f ms  ok=%s hint=%s" % ((now - t_pub) * 1000, o.get("ok"), o.get("hint")), flush=True)


c = mqtt.Client(client_id="dual-%d" % int(time.time() * 1000), protocol=mqtt.MQTTv311)
if os.environ.get("MQTT_USER"):
    c.username_pw_set(os.environ["MQTT_USER"], os.environ.get("MQTT_PASS"))
c.on_connect = on_connect
c.on_message = on_message
c.connect(BROKER, 1883, 20)
c.loop_start()
time.sleep(2.0)

t_pub = time.time()
cmd = json.dumps({"act": ACT, "val": int(VAL), "_t": int(t_pub * 1000)})
c.publish("%s/%s/cmd" % (PFX, DEV), cmd, qos=0)
print("publish -> %s : %s" % (BROKER, cmd), flush=True)

time.sleep(6.0)
c.loop_stop()
c.disconnect()
print("ack=%s  state=%s  重复帧=%d" % (
    "%.0fms" % ((t_ack - t_pub) * 1000) if t_ack else "无",
    "%.0fms" % ((t_state - t_pub) * 1000) if t_state else "无",
    dups))
