"""手机 App 侧订阅端（模拟 Ciallo WebView 收到 MQTT 后怎么显示）。

只做三件事：
  1. 订阅结果/心跳/在线状态三个主题，retained 消息保证一打开就有内容；
  2. 把判定渲染成卡片文案（含"数据来源：NAS"标注，避免和固件 wq 混淆）；
  3. 记录消息流供断言。
"""
from __future__ import annotations

import json
import threading
import time
from dataclasses import dataclass, field
from typing import Any, Optional

import paho.mqtt.client as mqtt

ICON = {0: "OK", 1: "WARN", 2: "ALARM", 3: "CRIT"}


@dataclass
class AppSimulator:
    host: str = "127.0.0.1"
    port: int = 1883
    base: str = "pool/01"
    client_id: str = "app-sim"
    messages: list[tuple[str, dict]] = field(default_factory=list)
    results: list[dict] = field(default_factory=list)
    health: list[dict] = field(default_factory=list)
    avail: list[dict] = field(default_factory=list)
    _lock: threading.Lock = field(default_factory=threading.Lock)

    def _topics(self) -> dict[str, str]:
        return {
            "result": f"{self.base}/wq/nas",
            "health": f"{self.base}/wq/nas/health",
            "avail": f"{self.base}/availability",
        }

    def start(self) -> None:
        t = self._topics()
        self.mq = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2,
                             client_id=self.client_id, protocol=mqtt.MQTTv311)
        self.mq.on_connect = lambda c, u, f, rc, p=None: (
            [c.subscribe(x, qos=1) for x in t.values()])
        self.mq.on_message = self._on_msg
        self.mq.connect(self.host, self.port, 30)
        self.mq.loop_start()
        time.sleep(0.6)

    def _on_msg(self, c, u, m) -> None:
        try:
            d = json.loads(m.payload.decode())
        except Exception:
            return
        with self._lock:
            self.messages.append((m.topic, d))
            if m.topic.endswith("/wq/nas"):
                self.results.append(d)
            elif m.topic.endswith("/health"):
                self.health.append(d)
            elif m.topic.endswith("/availability"):
                self.avail.append(d)

    @property
    def latest(self) -> Optional[dict]:
        with self._lock:
            return self.results[-1] if self.results else None

    def render(self) -> str:
        r = self.latest
        a = self.avail[-1] if self.avail else {}
        if not r:
            return "[水质] 无数据（NAS 未发布结果）"
        if not a.get("online", False):
            return "[水质] NAS 离线 —— 无法判定，勿把空白当正常"
        st = r.get("status")
        if st != "OK":
            hint = {"WARMUP": f"采样中 {r.get('n', 0)}/{24}",
                    "STALE": "数据断流", "NO_DATA": "探头无有效读数",
                    "INIT": "初始化"}.get(st, st)
            return f"[水质] {hint}"
        conf = r.get("conf", 0)
        flag = "" if r.get("conf_ok", True) else "（观察中）"
        vote = "" if r.get("stable", True) else "（投票未定）"
        src = "NAS" if r.get("src") == "nas" else "设备"
        return (f"[水质] {ICON.get(r['cls'], '?')} {r['label']} "
                f"{conf*100:.1f}%{flag}{vote}  · 来源:{src} · "
                f"窗口{r.get('n')}点/{r.get('span_s',0)/3600:.1f}h")

    def stop(self) -> None:
        try:
            self.mq.loop_stop()
            self.mq.disconnect()
        except Exception:
            pass
