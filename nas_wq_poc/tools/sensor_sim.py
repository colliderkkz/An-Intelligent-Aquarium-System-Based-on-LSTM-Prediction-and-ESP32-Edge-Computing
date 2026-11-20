"""传感器轨迹生成 + MQTT 发布（模拟 ESP32-S3 的 state 帧）。

场景都按"2 min 原始帧 / 12 h 以上"生成，和真实固件的行为对齐：
固件大约每 5 s 发一帧 state，探头按 2 min 目标节拍被采样。
"""
from __future__ import annotations

import argparse
import json
import math
import time
from dataclasses import dataclass
from typing import Iterator

import numpy as np

FRAME_S = 120           # 原始帧间隔 2 min，与固件默认节拍一致


@dataclass
class Scenario:
    name: str
    hours: float
    seed: int = 42


def _base(n: int, rng: np.random.Generator) -> np.ndarray:
    a = np.empty((n, 4), np.float64)
    a[:, 0] = 25.0 + rng.normal(0, 0.08, n)
    a[:, 1] = 7.22 + np.cumsum(rng.normal(0, 0.004, n))
    a[:, 2] = 205.0 + np.cumsum(rng.normal(0, 0.9, n))
    a[:, 3] = 5.2 + np.abs(np.cumsum(rng.normal(0, 0.12, n)))
    a[:, 1] = np.clip(a[:, 1], 6.9, 7.5)
    a[:, 3] = np.clip(a[:, 3], 1.5, 14.0)
    return a


def make_trace(sc: Scenario) -> np.ndarray:
    """返回 (n, 4) 原始工程量 + 首帧 ts 由调用方决定。"""
    rng = np.random.default_rng(sc.seed)
    n = int(sc.hours * 3600 / FRAME_S) + 1
    a = _base(n, rng)
    f = np.linspace(0.0, 1.0, n)

    if sc.name == "clean":
        pass

    elif sc.name == "degrade_slow":
        # 两天没换水 + 过量投喂：12 h 内浊度 5 -> 30、pH 7.22 -> 6.95、TDS 205 -> 290
        a[:, 0] += 1.1 * f
        a[:, 1] += -0.27 * f
        a[:, 2] += 85.0 * f
        a[:, 3] += 25.0 * f ** 1.3

    elif sc.name == "feed_spike":
        # 稳定水体，中途一次投喂导致的浊度尖峰（40 min 内起来又沉降干净），
        # 尖峰在结束前 ~2 h，保证终态是干净的 —— 这样"终态判定"和"过程误报"
        # 才能分开看。
        idx = np.arange(n)
        peak = n - int(120 * 60 / FRAME_S)
        bump = np.exp(-((idx - peak) / 6.0) ** 2)
        a[:, 3] += 52.0 * bump
        a[:, 2] += 18.0 * bump

    elif sc.name == "probe_drop":
        # 浊度探头在最后 6 h 掉线（模拟线松/进水）
        a[-int(6 * 3600 / FRAME_S):, 3] = np.nan

    elif sc.name == "uncalibrated":
        # 探头未标定：浊度整体读高 3 倍（模型会系统性偏严重）
        a[:, 3] *= 3.0

    elif sc.name == "stable_warm":
        # 温度整体偏高但水质指标正常：只看温度的伪告警测试
        a[:, 0] += 2.0
        a[:, 3] += 1.0

    else:
        raise ValueError(f"unknown scenario {sc.name}")

    a[:, 2] = np.clip(a[:, 2], 30.0, 900.0)
    return np.round(a, 3)


def frames(sc: Scenario, start_ts: int
           ) -> Iterator[tuple[int, float, float, float, float]]:
    a = make_trace(sc)
    for i, row in enumerate(a):
        ts = start_ts + i * FRAME_S
        if any(math.isnan(float(x)) for x in row):
            yield ts, float("nan"), float("nan"), float("nan"), float("nan")
            continue
        yield ts, float(row[0]), float(row[1]), float(row[2]), float(row[3])


def publish(host: str, port: int, topic: str, sc: Scenario,
            start_ts: int, delay: float = 0.0, jitter: bool = False) -> int:
    import paho.mqtt.client as mqtt

    c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2,
                    client_id=f"sim-{sc.name}", protocol=mqtt.MQTTv311)
    c.connect(host, port, 30)
    c.loop_start()
    n = 0
    t0 = time.time()
    last = None
    for ts, t, ph, tds, tur in frames(sc, start_ts):
        if math.isnan(t):
            payload = {"ts": ts, "t": None, "ph": None, "tds": None, "tur": None}
        else:
            payload = {"ts": ts, "t": round(t, 3), "ph": round(ph, 3),
                       "tds": round(tds, 1), "tur": round(tur, 2)}
        # QoS1：固件侧用 QoS0 更省电，但验证链路必须确认"发出去的每一帧都到了"
        last = c.publish(topic, json.dumps(payload), qos=1)
        n += 1
        if delay:
            target = t0 + n * delay
            d = target - time.time()
            if d > 0:
                time.sleep(d)
    # 必须等最后一帧被 broker 确认，否则 loop_stop 会把还在 socket 缓冲里的帧丢掉
    if last is not None:
        last.wait_for_publish(15)
    time.sleep(0.3)
    c.loop_stop()
    c.disconnect()
    return n


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=1883)
    ap.add_argument("--topic", default="pool/01/state")
    ap.add_argument("--scenario", default="degrade_slow")
    ap.add_argument("--hours", type=float, default=13.0)
    ap.add_argument("--seed", type=int, default=42)
    ap.add_argument("--delay", type=float, default=0.0)
    ap.add_argument("--start-ts", type=int, default=0)
    a = ap.parse_args()
    st = a.start_ts or int(time.time()) - int(a.hours * 3600)
    n = publish(a.host, a.port, a.topic,
                Scenario(a.scenario, a.hours, a.seed), st, a.delay)
    print(f"published {n} frames of {a.scenario} to {a.topic}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
