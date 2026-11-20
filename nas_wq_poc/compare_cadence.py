"""节拍口径对照：同一份遥测，只改窗口步长，量化结论差多少。

三个指标（都不是"精度"，而是**部署口径敏感性**）：
  1. 首报提前量 —— 第一次给出 Caution 及以上判定的时刻，越早越有预警价值；
  2. 瞬态误报   —— 一次会自行恢复的投喂尖峰期间，报了几次 Severe；
  3. 终态一致性 —— 过程跑完时两边结论是否一致。

背景：交付包 README §6.2 / MODEL_CARD §6.1 把"训练 30 min 节拍 vs 设备 2 min
节拍"列为最大现场风险，但没给量级。这里把它量化出来。
"""
from __future__ import annotations

import os
import sys
from collections import Counter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from tools.sensor_sim import FRAME_S, Scenario, make_trace   # noqa: E402
from wq_nas import EngineConfig, Model, WindowEngine         # noqa: E402

WEIGHTS = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                       "weights.json")
NAMES = {0: "正常", 1: "注意", 2: "警告", 3: "严重", -1: "无"}
T0 = 1_800_000_000


def run(scenario: str, hours: float, step_s: int):
    model = Model.from_json(WEIGHTS)
    eng = WindowEngine(model, EngineConfig(step_s=step_s, vote_n=3))
    tr = make_trace(Scenario(scenario, hours))
    hist: list[tuple[int, int, float]] = []
    for i, row in enumerate(tr):
        if any(x != x for x in row):
            continue
        ts = T0 + i * FRAME_S
        eng.note(ts, True)
        r = eng.ingest(ts, float(row[0]), float(row[1]), float(row[2]),
                       float(row[3]))
        if r is not None and r.status == "OK":
            hist.append((ts, r.vote_cls, r.conf))
    return hist


def first_alarm(hist) -> float | None:
    """第一次给出 注意 及以上的轨迹时刻（小时，从轨迹起点算）。"""
    for ts, c, _ in hist:
        if c >= 1:
            return (ts - T0) / 3600.0
    return None


def main() -> int:
    steps = [(1800, "1800 s / 12.0 h"), (600, "600 s / 4.0 h"),
             (120, "120 s / 48 min")]

    print("=" * 92)
    print("1) 缓慢恶化：首报时刻（第一次给出 注意 及以上的轨迹时刻，单位 h）")
    print("=" * 92)
    for sc, desc in [("degrade_slow", "32 h 不换水，浊度 5→30 NTU，pH 缓降")]:
        print(f"  场景 {sc}：{desc}")
        res = {}
        for step, label in steps:
            h = run(sc, 32.0, step)
            fa = first_alarm(h)
            res[step] = fa
            tail = NAMES[h[-1][1]] if h else "-"
            print(f"    {label:<18} 首报 "
                  f"{('%5.1f h' % fa) if fa is not None else '  从未':>8}"
                  f"   终态 {tail}")
        ref = res.get(120)
        if ref is not None and res.get(1800) is not None:
            print(f"\n    → 1800 s 口径比 120 s 口径早 {ref - res[1800]:.1f} h 报警")
        if ref is not None and res.get(600) is not None:
            print(f"    →  600 s 口径比 120 s 口径早 {ref - res[600]:.1f} h 报警")

    print()
    print("=" * 92)
    print("2) 瞬态误报：一次 40 min 投喂尖峰（之后自行沉降），过程中报 Severe 的帧数")
    print("=" * 92)
    for step, label in steps:
        h = run("feed_spike", 32.0, step)
        cnt = Counter(c for _, c, _ in h)
        sev = cnt.get(3, 0)
        print(f"    {label:<18} 判定 {dict((NAMES[k], v) for k, v in sorted(cnt.items()))}"
              f"   Severe 帧 = {sev} ({100.0*sev/max(len(h),1):.1f}%)")

    print()
    print("=" * 92)
    print("3) 判定抖动：窗口每滑一格，判定就翻转的次数")
    print("=" * 92)
    print(f"    {'场景':<16}{'1800 s':<18}{'600 s':<18}{'120 s':<18}")
    for sc in ("clean", "degrade_slow", "feed_spike"):
        cells = []
        for step, _ in steps:
            h = run(sc, 32.0, step)
            flips = sum(1 for a, b in zip(h, h[1:]) if a[1] != b[1])
            cells.append(f"{flips}/{max(len(h)-1,1)}")
        print(f"    {sc:<16}{cells[0]:<18}{cells[1]:<18}{cells[2]:<18}")

    print()
    print("=" * 92)
    print("4) 计算量：同一份 32 h 遥测，各口径实际推理次数")
    print("=" * 92)
    for step, label in steps:
        h = run("degrade_slow", 32.0, step)
        print(f"    {label:<18} 推理 {len(h):>5} 次 / 32 h"
              f"   ≈ {len(h)/32:.2f} 次/小时")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
