"""窗口引擎：把高频遥测压成 24 点趋势窗口，跑推理，出带状态的结论。

设计要点（每一条都对着交付包里的某条限制）：
  1. **按 ts 分桶采样**，而不是"距上次入窗多久"。桶号 = ts // step，
     每个新桶取第一帧。好处：无漂移、可重放、重启后能由原始表精确重建窗口。
  2. **原始帧全量落库**，窗口只是它的一个视图。所以事后可以把 step 从
     120 s 改成 1800 s 重新回算 —— 手机和固件都做不到这件事。
  3. **缺数据不猜**：四路缺一/超量程/断流时不推进窗口，状态降级为
     NO_DATA / STALE，绝不拿旧值凑一个"看起来正常"的结论。
  4. **3 帧多数投票 + 迟滞**（交付包 README §6.4 明说"本包未内置"）：
     窗口每滑一格边界样本会跳，投票把这个抖动吃掉。
  5. **低置信度单独标**（MODEL_CARD §3 建议 <0.6 提示"观察中"）。

引擎不碰网络、不碰时钟：时间一律从帧的 ts 和 max_ts 推，便于离线重放与单测。
"""
from __future__ import annotations

import math
from collections import Counter, deque
from dataclasses import dataclass, field
from typing import Iterable, Optional

import numpy as np

from .model import CLASS_LABELS, CLASS_NAMES, WINDOW, Model, check_frame


@dataclass
class EngineConfig:
    step_s: int = 1800          # 训练节拍 30 min；不是固件默认的 120 s
    window: int = WINDOW
    vote_n: int = 3             # 多数投票帧数
    stale_mult: float = 3.0     # 超过 N 个 step 没有新样本 -> STALE
    min_conf: float = 0.60      # 低于此置信度标记为"观察中"
    gap_warn: int = 2           # 连续 N 个桶没数据 -> 记一次 gap 告警


@dataclass
class Result:
    ts: int
    seq: int
    status: str                       # OK/WARMUP/STALE/NO_DATA/INIT/OFF
    cls: int = -1
    probs: list[float] = field(default_factory=list)
    conf: float = 0.0
    conf_ok: bool = True
    vote_cls: int = -1                # 投票后的稳定类别
    vote_agree: int = 0
    stable: bool = False
    n: int = 0
    step_s: int = 0
    span_s: float = 0.0
    in_dist: bool = True
    notes: list[str] = field(default_factory=list)
    infer_ms: float = 0.0
    model: str = "pool168_s42"

    def to_json(self) -> dict:
        # 关键：状态不是 OK 时**绝不**给 cls/name/label。
        # 否则 App 只要读 cls 就会把"探头掉了"渲染成"正常"。
        ok = self.status == "OK"
        eff = self.vote_cls if self.vote_cls >= 0 else self.cls
        if not ok or eff < 0:
            cls_out, name_out, label_out = None, None, None
        else:
            cls_out, name_out, label_out = eff, CLASS_NAMES[eff], CLASS_LABELS[eff]
        return {
            "ts": self.ts,
            "seq": self.seq,
            "src": "nas",
            "model": self.model,
            "status": self.status,
            "cls": cls_out,
            "name": name_out,
            "label": label_out,
            "cls_raw": self.cls if self.cls >= 0 else None,
            "p": [round(float(x), 4) for x in self.probs] or None,
            "conf": round(float(self.conf), 4) if ok else None,
            "conf_ok": self.conf_ok if ok else None,
            "vote_agree": self.vote_agree,
            "stable": self.stable,
            "n": self.n,
            "step_s": self.step_s,
            "span_s": round(self.span_s, 1),
            "in_dist": self.in_dist,
            "infer_ms": round(self.infer_ms, 3),
            "notes": self.notes,
        }


class WindowEngine:
    def __init__(self, model: Model, cfg: EngineConfig) -> None:
        self.model = model
        self.cfg = cfg
        self._win: deque[tuple[float, float, float, float]] = deque(maxlen=cfg.window)
        self._ts: deque[int] = deque(maxlen=cfg.window)
        self._votes: deque[int] = deque(maxlen=cfg.vote_n)
        self._last_bucket: Optional[int] = None
        self._last_push_ts: Optional[int] = None
        self._max_ts: int = 0
        self._max_seen_ts: int = 0        # 任何帧（含无效帧）推进的逻辑时钟
        self._last_invalid_ts: int = 0
        self._seq = 0
        self._missed_buckets = 0
        self._last_in_dist = True
        self._last_notes: list[str] = []

    # ---------------- 状态 ----------------
    @property
    def n(self) -> int:
        return len(self._win)

    @property
    def full(self) -> bool:
        return len(self._win) >= self.cfg.window

    def snapshot_window(self) -> list[list[float]]:
        return [list(v) for v in self._win]

    def seed(self, samples: Iterable[tuple[int, float, float, float, float]]) -> int:
        """由原始历史重建窗口（重启恢复 / 离线回算用）。
        samples: (ts, t, ph, tds, tur)，按 ts 升序。返回灌入点数。"""
        loaded = 0
        for ts, t, ph, tds, tur in samples:
            ok, in_dist, notes = check_frame(t, ph, tds, tur)
            if not ok:
                continue
            bucket = int(ts) // self.cfg.step_s
            if self._last_bucket is not None and bucket == self._last_bucket:
                continue
            self._last_bucket = bucket
            self._win.append((t, ph, tds, tur))
            self._ts.append(int(ts))
            self._last_push_ts = int(ts)
            self._max_ts = max(self._max_ts, int(ts))
            self._last_in_dist = in_dist
            self._last_notes = notes
            loaded += 1
        return loaded

    # ---------------- 主入口 ----------------
    def note(self, ts: int, valid: bool) -> None:
        """登记"收到一帧"。**任何**帧（哪怕无效）都推进逻辑时钟。

        必须把这两件事分开，否则探头掉线时——一帧有效数据都没有——
        逻辑时钟会冻结，status() 一直是 OK，UI 上"水质正常"和
        "探头早断了"长得一模一样。这是整套设计里最容易埋的雷。
        """
        ts = int(ts)
        self._max_seen_ts = max(self._max_seen_ts, ts)
        if not valid:
            self._last_invalid_ts = max(self._last_invalid_ts, ts)

    def ingest(self, ts: int, t: float, ph: float, tds: float, tur: float
               ) -> Optional[Result]:
        """喂一帧原始遥测。返回 Result 表示"这一帧触发了推理"，None 表示仅落窗/被丢弃。"""
        ts = int(ts)
        if self._max_ts and ts < self._max_ts - 5 * self.cfg.step_s:
            return None                       # 明显过期/乱序的帧，不回退窗口
        self._max_ts = max(self._max_ts, ts)

        ok, in_dist, notes = check_frame(t, ph, tds, tur)
        if not ok:
            # 不可用帧：窗口不推进，只报状态
            self._last_notes = notes
            return None

        bucket = ts // self.cfg.step_s
        if self._last_bucket is None:
            self._last_bucket = bucket
        elif bucket == self._last_bucket:
            return None                       # 同桶已有样本，只更新"最近值"
        elif bucket > self._last_bucket:
            self._missed_buckets = bucket - self._last_bucket - 1
            self._last_bucket = bucket
        else:
            return None                       # 旧桶迟到帧

        self._win.append((t, ph, tds, tur))
        self._ts.append(ts)
        self._last_push_ts = ts
        self._last_in_dist = in_dist
        self._last_notes = notes

        if not self.full:
            self._seq += 1
            return Result(ts=ts, seq=self._seq, status="WARMUP", n=self.n,
                          step_s=self.cfg.step_s, notes=["窗口未满"])
        return self._infer(ts)

    def _infer(self, ts: int) -> Result:
        self._seq += 1
        import time
        t0 = time.perf_counter()
        p = self.model.predict(self._win)
        infer_ms = (time.perf_counter() - t0) * 1000.0
        cls = int(p.argmax())
        conf = float(p[cls])

        self._votes.append(cls)
        cnt = Counter(self._votes)
        vote_cls, agree = cnt.most_common(1)[0]
        need = math.ceil(self.cfg.vote_n / 2)
        stable = agree >= need

        span = float(self._ts[-1] - self._ts[0]) if len(self._ts) > 1 else 0.0
        notes = list(self._last_notes)
        if self._missed_buckets >= self.cfg.gap_warn:
            notes.append(f"窗口内漏采 {self._missed_buckets} 个节拍")
        status = "OK"
        if not stable:
            notes.append(f"投票未过半 {dict(cnt)}，沿用上一稳定类")
            vote_cls = self._last_stable_cls()
            if vote_cls < 0:
                vote_cls = cls
        else:
            self._last_stable = vote_cls   # type: ignore[attr-defined]

        return Result(
            ts=ts, seq=self._seq, status=status, cls=cls,
            probs=[float(x) for x in p], conf=conf,
            conf_ok=conf >= self.cfg.min_conf,
            vote_cls=vote_cls, vote_agree=agree, stable=stable,
            n=self.n, step_s=self.cfg.step_s, span_s=span,
            in_dist=self._last_in_dist, notes=notes, infer_ms=infer_ms,
        )

    def _last_stable_cls(self) -> int:
        return int(getattr(self, "_last_stable", -1))

    # ---------------- 无新样本时的状态 ----------------
    def status(self) -> str:
        if not self._ts:
            return "INIT"
        if not self.full:
            return "WARMUP"
        # 最近收到的帧是无效帧 -> 探头在报，但报的是垃圾
        if self._last_invalid_ts > (self._last_push_ts or 0):
            return "NO_DATA"
        if self._last_push_ts is not None:
            age = self._max_seen_ts - self._last_push_ts
            if age > self.cfg.stale_mult * self.cfg.step_s:
                return "STALE"
        return "OK"

    def peek(self) -> Result:
        """不重新推理，给出当前状态的快照（心跳用）。"""
        st = self.status()
        span = float(self._ts[-1] - self._ts[0]) if len(self._ts) > 1 else 0.0
        return Result(
            ts=self._max_seen_ts or self._max_ts, seq=self._seq, status=st,
            vote_cls=self._last_stable_cls() if self.full else -1,
            n=self.n, step_s=self.cfg.step_s, span_s=span,
            in_dist=self._last_in_dist, notes=list(self._last_notes),
        )
