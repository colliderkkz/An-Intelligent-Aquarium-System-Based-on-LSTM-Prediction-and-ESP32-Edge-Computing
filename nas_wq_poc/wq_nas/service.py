"""MQTT 服务：订阅原始遥测 -> 窗口推理 -> 发布结论（retained）。

主题约定（base 可配，默认 pool/01）：

    <base>/state            下行  固件/网关发的原始状态帧（本项目只读）
    <base>/wq/nas           上行  本服务发布的判定结果（retained）
    <base>/wq/nas/health    上行  心跳：窗口填充度 / 末帧年龄 / 进程健康（retained）
    <base>/availability     上行  online/offline（LWT 保证掉线可见）

三条铁律：
  1. 结果**必须 retained**，否则手机刚打开 App 时看不到任何东西；
  2. 必须有独立 heartbeat，让 App 能区分"水质正常"和"NAS 挂了"——
     这两件事都不发消息，UI 上长得一模一样，是最容易埋雷的地方；
  3. 结果主题与固件的 `state.wq` **分开**，交付包明确要求不要混用两个来源。
"""
from __future__ import annotations

import json
import logging
import threading
import time
from dataclasses import dataclass, field
from typing import Any, Callable, Optional

import paho.mqtt.client as mqtt

from .engine import EngineConfig, Result, WindowEngine
from .model import Model
from .store import Store

log = logging.getLogger("wq_nas")

ALIASES = {
    "t": ("t", "temp", "temp_c", "water_temp", "wt"),
    "ph": ("ph", "pH", "PH"),
    "tds": ("tds", "tds_ppm", "tds_mg_l", "TDS"),
    "tur": ("tur", "turb", "turbidity", "turbidity_ntu", "ntu"),
    "ts": ("ts", "timestamp", "time", "t_ms"),
}


@dataclass
class ServiceConfig:
    host: str = "127.0.0.1"
    port: int = 1883
    # 走 MQTT over WebSocket（PassNAT 隧道只开放 ws，家庭局域网用不到）
    ws: bool = False
    ws_path: str = "/mqtt"
    base: str = "pool/01"
    client_id: str = "wq-nas"
    username: Optional[str] = None
    password: Optional[str] = None
    weights: str = "weights.json"
    db: str = "wq_nas.sqlite"
    step_s: int = 1800
    vote_n: int = 3
    min_conf: float = 0.60
    heartbeat_s: float = 30.0
    qos: int = 1
    state_topic: str = ""          # 留空则用 <base>/state
    result_topic: str = ""         # 留空则用 <base>/wq/nas
    health_topic: str = ""         # 留空则用 <base>/wq/nas/health
    avail_topic: str = ""          # 留空则用 <base>/availability
    restore_window: bool = True

    def resolve(self) -> "ServiceConfig":
        if not self.state_topic:
            self.state_topic = f"{self.base}/state"
        if not self.result_topic:
            self.result_topic = f"{self.base}/wq/nas"
        if not self.health_topic:
            self.health_topic = f"{self.base}/wq/nas/health"
        if not self.avail_topic:
            self.avail_topic = f"{self.base}/availability"
        return self


def _pick(payload: dict, key: str) -> Any:
    for k in ALIASES[key]:
        if k in payload:
            v = payload[k]
            try:
                return float(v)
            except (TypeError, ValueError):
                return None
    return None


class WQService:
    def __init__(self, cfg: ServiceConfig,
                 model: Optional[Model] = None,
                 store: Optional[Store] = None,
                 on_result: Optional[Callable[[Result, dict], None]] = None):
        self.cfg = cfg.resolve()
        self.model = model or Model.from_json(self.cfg.weights)
        self.store = store or Store(self.cfg.db)
        self.engine = WindowEngine(self.model, EngineConfig(
            step_s=self.cfg.step_s, vote_n=self.cfg.vote_n,
            min_conf=self.cfg.min_conf))
        self.on_result = on_result
        self._restored = 0
        self.stats = {"frames": 0, "inferred": 0, "rejected": 0,
                      "last_frame_ts": 0}
        self._last_status: str = ""
        self._stop = threading.Event()
        self._hb_thread: Optional[threading.Thread] = None

        if self.cfg.ws:
            self.mq = mqtt.Client(
                mqtt.CallbackAPIVersion.VERSION2,
                client_id=self.cfg.client_id,
                protocol=mqtt.MQTTv311,
                transport="websockets",
            )
            self.mq.ws_set_options(path=self.cfg.ws_path)
        else:
            self.mq = mqtt.Client(
                mqtt.CallbackAPIVersion.VERSION2,
                client_id=self.cfg.client_id,
                protocol=mqtt.MQTTv311,
            )
        if self.cfg.username:
            self.mq.username_pw_set(self.cfg.username, self.cfg.password)
        # 掉线可见：broker 发现客户端异常断开时会代发 offline
        self.mq.will_set(self.cfg.avail_topic,
                         json.dumps({"online": False, "client": self.cfg.client_id}),
                         qos=1, retain=True)
        self.mq.on_connect = self._on_connect
        self.mq.on_disconnect = self._on_disconnect
        self.mq.on_message = self._on_message

    # ---------------- lifecycle ----------------
    def start(self, connect_timeout: float = 10.0) -> None:
        if self.cfg.restore_window:
            self._restored = self._restore()
        self.mq.connect(self.cfg.host, self.cfg.port, 30)
        self.mq.loop_start()
        t0 = time.time()
        while not self.mq.is_connected():
            if time.time() - t0 > connect_timeout:
                raise TimeoutError(f"连不上 broker {self.cfg.host}:{self.cfg.port}")
            time.sleep(0.05)
        log.info("connected; restored %d window points", self._restored)
        self._hb_thread = threading.Thread(target=self._heartbeat, daemon=True)
        self._hb_thread.start()

    def stop(self) -> None:
        self._stop.set()
        try:
            self.mq.publish(self.cfg.avail_topic,
                            json.dumps({"online": False,
                                        "client": self.cfg.client_id,
                                        "bye": True}), qos=1, retain=True)
            time.sleep(0.15)
        except Exception:
            pass
        self.mq.loop_stop()
        self.mq.disconnect()
        self.store.commit()
        self.store.close()

    def _restore(self) -> int:
        now = int(time.time())
        rows = self.store.rebuild(self.engine.cfg.window, self.cfg.step_s, now)
        if not rows:
            return 0
        n = self.engine.seed(rows)
        log.info("window restored from %d raw rows -> %d points", len(rows), n)
        return n

    # ---------------- paho callbacks ----------------
    def _on_connect(self, c, u, flags, reason_code, props=None):
        if reason_code != 0:
            log.error("connect refused: %s", reason_code)
            return
        c.publish(self.cfg.avail_topic,
                  json.dumps({"online": True, "client": self.cfg.client_id,
                              "model": "pool168_s42",
                              "step_s": self.cfg.step_s}),
                  qos=1, retain=True)
        c.subscribe(self.cfg.state_topic, qos=self.cfg.qos)
        log.info("subscribed %s", self.cfg.state_topic)
        # 立刻把当前（可能是 WARMUP/STALE）状态顶上去，手机不用等
        self._publish_state(self.engine.peek())

    def _on_disconnect(self, c, u, flags, reason_code, props=None):
        if not self._stop.is_set():
            log.warning("disconnected rc=%s", reason_code)

    def _on_message(self, c, u, msg):
        try:
            payload = json.loads(msg.payload.decode("utf-8"))
        except Exception:
            self.stats["rejected"] += 1
            return
        if not isinstance(payload, dict):
            self.stats["rejected"] += 1
            return

        # 某些实现把字段包在 data/state 下
        for wrap in ("data", "state", "sensors"):
            if wrap in payload and isinstance(payload[wrap], dict):
                merged = dict(payload[wrap])
                merged.update({k: v for k, v in payload.items()
                               if k not in ("data", "state", "sensors")})
                payload = merged
                break

        t, ph, tds, tur = (_pick(payload, k) for k in ("t", "ph", "tds", "tur"))
        ts = _pick(payload, "ts")
        now = time.time()
        ts_i = int(now)
        if ts and 1e9 < ts < 1e11:
            ts_i = int(ts)

        self.stats["frames"] += 1
        self.stats["last_frame_ts"] = ts_i
        from .model import check_frame
        missing = None in (t, ph, tds, tur)
        ok, in_dist, _ = (False, False, []) if missing else check_frame(
            t, ph, tds, tur)
        # 无效帧（含字段为 null 的）也必须推进逻辑时钟，
        # 否则探头掉线后状态会一直停在 OK —— 这是最危险的静默失效。
        self.engine.note(ts_i, ok)
        if not ok:
            self.stats["rejected"] += 1
            self._publish_status_if_changed()
            return

        self.store.add_raw(ts_i, t, ph, tds, tur, in_dist, now)
        res = self.engine.ingest(ts_i, t, ph, tds, tur)
        if res is not None:
            self._publish_state(res, count=True)
        if self.stats["frames"] % 50 == 0:
            self.store.commit()

    # ---------------- publish ----------------
    def _publish_status_if_changed(self) -> None:
        st = self.engine.status()
        if st != self._last_status:
            self._last_status = st
            self._publish_state(self.engine.peek(), count=False)

    def _publish_state(self, r: Result, count: bool = False) -> None:
        self._last_status = r.status
        if count and r.status == "OK" and r.cls >= 0:
            self.stats["inferred"] += 1
        body = json.dumps(r.to_json(), ensure_ascii=False, separators=(",", ":"))
        self.mq.publish(self.cfg.result_topic, body, qos=self.cfg.qos, retain=True)
        if r.seq:
            self.store.add_result(r, body)
        if self.on_result:
            self.on_result(r, r.to_json())

    def _heartbeat(self) -> None:
        while not self._stop.wait(self.cfg.heartbeat_s):
            try:
                snap = self.engine.peek()
                age = (snap.ts - self.engine._last_push_ts) \
                    if self.engine._last_push_ts else None
                hb = {
                    "ts": int(time.time()),
                    "online": True,
                    "status": snap.status,
                    "n": snap.n,
                    "need": self.engine.cfg.window,
                    "span_s": round(snap.span_s, 1),
                    "step_s": self.cfg.step_s,
                    "last_sample_age_s": age,
                    "frames": self.stats["frames"],
                    "inferred": self.stats["inferred"],
                    "rejected": self.stats["rejected"],
                    "restored": self._restored,
                    "db": self.store.counts(),
                    "model": "pool168_s42",
                    "ver": "nas-1.0.0",
                }
                self._publish_status_if_changed()
                self.mq.publish(self.cfg.health_topic,
                                json.dumps(hb, ensure_ascii=False),
                                qos=0, retain=True)
                self.store.commit()
            except Exception as e:            # 心跳失败不能拖垮主流程
                log.warning("heartbeat error: %s", e)

