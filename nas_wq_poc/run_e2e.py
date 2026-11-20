"""端到端可行性验证：真实 MQTT 协议，全链路闭环。

链路：
    传感器模拟器 --publish--> broker --subscribe--> NAS 服务 --推理-->
    --publish retained--> broker --subscribe--> 手机 App 模拟器

验证项：
  E1 服务能连上 broker 并订阅 state
  E2 窗口填充期出 WARMUP（带进度），不是静默
  E3 窗口满后出 OK 判定
  E4 判定结果 retained：中途才连上的手机立刻能拿到
  E5 独立 heartbeat + availability 主题存在（能区分"水质正常"和"NAS 挂了"）
  E6 进程重启后窗口由 DB 恢复，不必等 12 h 重新灌满
"""
from __future__ import annotations

import json
import os
import shutil
import sys
import tempfile
import time
import traceback

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from tools.app_sim import AppSimulator                      # noqa: E402
from tools.dev_broker import DevBroker, port_free            # noqa: E402
from tools.sensor_sim import Scenario, publish               # noqa: E402
from wq_nas import ServiceConfig, WQService                  # noqa: E402

WEIGHTS = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                       "weights.json")
PORT = 18841
BASE = "pool/01"
PASS: list[str] = []
FAIL: list[str] = []


def check(name: str, ok: bool, detail: str = "") -> None:
    (PASS if ok else FAIL).append(name)
    print(f"  [{'PASS' if ok else 'FAIL'}] {name}" + (f"  — {detail}" if detail else ""))


def wait_for(fn, timeout: float, step: float = 0.05) -> bool:
    t0 = time.time()
    while time.time() - t0 < timeout:
        if fn():
            return True
        time.sleep(step)
    return False


def run_scenario(broker_port: int, scenario: str, hours: float,
                 label: str) -> tuple[WQService, AppSimulator, dict]:
    tmp = tempfile.mkdtemp(prefix="wqnas_")
    db = os.path.join(tmp, "wq.sqlite")
    cfg = ServiceConfig(host="127.0.0.1", port=broker_port, base=BASE,
                        weights=WEIGHTS, db=db, step_s=1800, vote_n=3,
                        heartbeat_s=1.5, client_id=f"wq-nas-{label}")
    svc = WQService(cfg)
    svc.start()

    app = AppSimulator(host="127.0.0.1", port=broker_port, base=BASE,
                       client_id=f"app-{label}")
    app.start()

    start_ts = 1_800_000_000
    n = publish("127.0.0.1", broker_port, f"{BASE}/state",
                Scenario(scenario, hours), start_ts)
    wait_for(lambda: app.latest is not None, 6.0)
    time.sleep(0.8)
    return svc, app, {"n_frames": n, "db": db, "tmp": tmp, "start_ts": start_ts}


def main() -> int:
    if not port_free(PORT):
        print(f"端口 {PORT} 被占用，换一个"); return 2
    print("=" * 84)
    print("水质模型 NAS + MQTT 端到端验证")
    print("=" * 84)

    broker = DevBroker(PORT)
    broker.start()
    print(f"broker up on 127.0.0.1:{PORT}\n")

    svc = app = None
    try:
        # ---------------- E1~E3 ----------------
        print("【场景 A】缓慢恶化 30 h，step=1800 s")
        svc, app, meta = run_scenario(PORT, "degrade_slow", 30.0, "a")
        check("E1 服务已订阅 state 并收到帧", svc.stats["frames"] > 0,
              f"frames={svc.stats['frames']}, rejected={svc.stats['rejected']}")

        statuses = [r.get("status") for r in app.results]
        check("E2 填充期发布 WARMUP（不静默）", "WARMUP" in statuses,
              f"status 序列 = {sorted(set(statuses))}")
        check("E3 窗口满后发布 OK 判定", "OK" in statuses,
              f"OK 条数 = {statuses.count('OK')}")

        ok = [r for r in app.results if r.get("status") == "OK"]
        if ok:
            last = ok[-1]
            check("E3b 判定类别为 警告/严重（缓慢恶化被识别）",
                  last["cls"] >= 2,
                  f"cls={last['cls']}({last['name']}) conf={last['conf']} "
                  f"n={last['n']} span={last['span_s']/3600:.1f}h")
            print(f"       App 卡片渲染: {app.render()}")

        # ---------------- E4 retained ----------------
        late = AppSimulator(host="127.0.0.1", port=PORT, base=BASE,
                            client_id="app-late")
        late.start()
        got = wait_for(lambda: late.latest is not None, 3.0)
        check("E4 迟到订阅者立刻收到 retained 结果", got,
              f"拿到 {len(late.messages)} 条历史消息")
        check("E4b retained 结果与最新一致",
              got and late.latest.get("seq") == app.latest.get("seq"),
              "seq 相同")
        late.stop()

        # ---------------- E5 heartbeat / availability ----------------
        check("E5 heartbeat 主题有数据", len(app.health) > 0,
              json.dumps(app.health[-1], ensure_ascii=False)[:110] if app.health else "")
        check("E5b availability 为 online",
              bool(app.avail) and app.avail[-1].get("online") is True)

        # ---------------- E6 重启恢复 ----------------
        print("\n【场景 A'】杀掉服务进程后重启，验证窗口恢复")
        svc.stop()
        time.sleep(0.3)
        cfg2 = ServiceConfig(host="127.0.0.1", port=PORT, base=BASE,
                             weights=WEIGHTS, db=meta["db"], step_s=1800,
                             vote_n=3, heartbeat_s=1.5, client_id="wq-nas-a2")
        svc2 = WQService(cfg2)
        svc2.start()
        check("E6 重启后从 DB 恢复窗口", svc2._restored >= 20,
              f"恢复 {svc2._restored} 点")
        # 继续往后灌几桶，应立刻恢复 OK
        nxt = meta["start_ts"] + int(30 * 3600) + 1800
        publish("127.0.0.1", PORT, f"{BASE}/state",
                Scenario("degrade_slow", 12.0, seed=99), nxt)
        app2 = AppSimulator(host="127.0.0.1", port=PORT, base=BASE,
                            client_id="app-a2")
        app2.start()
        time.sleep(1.0)
        ok2 = [r for r in app2.results if r.get("status") == "OK"]
        check("E6b 恢复后第一桶即出 OK（不用再等 12 h）", len(ok2) > 0,
              f"OK={len(ok2)}，首个 n={ok2[0]['n'] if ok2 else 0}")
        app2.stop()
        svc2.stop()

        # ---------------- 场景 B：干净水 ----------------
        print("\n【场景 B】干净水体 30 h")
        svcB, appB, metaB = run_scenario(PORT, "clean", 30.0, "b")
        okB = [r for r in appB.results if r.get("status") == "OK"]
        if okB:
            check("E7 干净水判定为 正常", okB[-1]["cls"] == 0,
                  f"cls={okB[-1]['cls']} p={okB[-1]['p']}")
        # 结果主题与固件 wq 分离
        check("E8 结果走独立主题（不与固件 state.wq 混用）",
              bool(appB.messages) and all(
                  t.endswith("/wq/nas") for t, d in appB.messages
                  if d.get("src") == "nas"),
              f"{BASE}/wq/nas")
        svcB.stop(); appB.stop()

        # ---------------- 场景 C：探头掉线 ----------------
        print("\n【场景 C】浊度探头最后 6 h 掉线")
        svcC, appC, metaC = run_scenario(PORT, "probe_drop", 30.0, "c")
        stC = [r.get("status") for r in appC.results]
        check("E9 探头掉线时降级为非 OK 状态（不冒充正常）",
              stC and stC[-1] != "OK",
              f"末状态={stC[-1] if stC else None}；全体={sorted(set(stC))}")
        svcC.stop(); appC.stop()

    except Exception:
        traceback.print_exc()
        FAIL.append("exception")
    finally:
        try:
            if svc is not None:
                svc.stop()
        except Exception:
            pass
        try:
            if app is not None:
                app.stop()
        except Exception:
            pass

    print("\n" + "=" * 84)
    print(f"结果: PASS={len(PASS)}  FAIL={len(FAIL)}")
    if FAIL:
        print("失败项:", FAIL)
    print("=" * 84)
    return 0 if not FAIL else 1


if __name__ == "__main__":
    raise SystemExit(main())
