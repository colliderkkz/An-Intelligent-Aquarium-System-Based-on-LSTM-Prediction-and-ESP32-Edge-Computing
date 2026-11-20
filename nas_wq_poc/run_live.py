"""NAS 上的服务入口（生产用）。

    python run_live.py --host 192.168.1.10 --base pool/01 --step-s 1800

也支持全部走环境变量（Docker 里就是这么用的）：
    WQ_MQTT_HOST / WQ_MQTT_PORT / WQ_BASE / WQ_STEP_S / WQ_DB / WQ_WEIGHTS ...
"""
from __future__ import annotations

import argparse
import logging
import os
import signal
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from wq_nas import ServiceConfig, WQService  # noqa: E402


def env(name: str, default: str) -> str:
    return os.environ.get(name, default)


def build_cfg(a: argparse.Namespace) -> ServiceConfig:
    return ServiceConfig(
        host=a.host, port=a.port, base=a.base,
        client_id=a.client_id,
        username=a.username or os.environ.get("WQ_MQTT_USER"),
        password=a.password or os.environ.get("WQ_MQTT_PASS"),
        weights=a.weights, db=a.db,
        ws=a.ws, ws_path=a.ws_path,
        step_s=a.step_s, vote_n=a.vote_n, min_conf=a.min_conf,
        heartbeat_s=a.heartbeat_s,
        state_topic=a.state_topic, result_topic=a.result_topic,
        restore_window=not a.no_restore,
    )


def main() -> int:
    ap = argparse.ArgumentParser(description="水质 AI 模型 NAS 推理服务")
    ap.add_argument("--host", default=env("WQ_MQTT_HOST", "127.0.0.1"))
    ap.add_argument("--port", type=int, default=int(env("WQ_MQTT_PORT", "1883")))
    ap.add_argument("--base", default=env("WQ_BASE", "pool/01"))
    ap.add_argument("--ws", action="store_true",
                    help="走 MQTT over WebSocket（PassNAT 隧道用）")
    ap.add_argument("--ws-path", default=env("WQ_MQTT_WS_PATH", "/mqtt"))
    ap.add_argument("--client-id", default=env("WQ_CLIENT_ID", "wq-nas"))
    ap.add_argument("--username", default=None)
    ap.add_argument("--password", default=None)
    ap.add_argument("--weights", default=env("WQ_WEIGHTS", "weights.json"))
    ap.add_argument("--db", default=env("WQ_DB", "data/wq_nas.sqlite"))
    ap.add_argument("--step-s", type=int, default=int(env("WQ_STEP_S", "1800")),
                    help="窗口步长（秒）。1800 = 训练口径 12 h 窗口；120 = 固件口径 48 min")
    ap.add_argument("--vote-n", type=int, default=int(env("WQ_VOTE_N", "3")))
    ap.add_argument("--min-conf", type=float,
                    default=float(env("WQ_MIN_CONF", "0.6")))
    ap.add_argument("--heartbeat-s", type=float,
                    default=float(env("WQ_HEARTBEAT_S", "30")))
    ap.add_argument("--state-topic", default=env("WQ_STATE_TOPIC", ""))
    ap.add_argument("--result-topic", default=env("WQ_RESULT_TOPIC", ""))
    ap.add_argument("--no-restore", action="store_true",
                    help="启动时不从 DB 恢复窗口")
    ap.add_argument("--verbose", "-v", action="store_true")
    a = ap.parse_args()

    logging.basicConfig(
        level=logging.DEBUG if a.verbose else logging.INFO,
        format="%(asctime)s %(levelname)-7s %(name)s | %(message)s")

    cfg = build_cfg(a).resolve()
    log = logging.getLogger("run_live")
    log.info("model=%s step=%ss base=%s db=%s",
             cfg.weights, cfg.step_s, cfg.base, cfg.db)
    log.info("in=%s  out=%s", cfg.state_topic, cfg.result_topic)

    svc = WQService(cfg)
    stop = {"done": False}

    def _sig(_s, _f):
        stop["done"] = True

    signal.signal(signal.SIGINT, _sig)
    try:
        signal.signal(signal.SIGTERM, _sig)
    except (AttributeError, ValueError):
        pass

    svc.start()
    last = time.time()
    while not stop["done"]:
        time.sleep(1.0)
        if time.time() - last >= 300:
            last = time.time()
            log.info("stats %s db=%s", svc.stats, svc.store.counts())
    log.info("shutting down")
    svc.stop()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
