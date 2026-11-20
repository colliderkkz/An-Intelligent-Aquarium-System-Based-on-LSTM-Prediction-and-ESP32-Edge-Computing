"""SQLite 持久化：原始遥测 + 推理结果。

为什么要落原始帧而不是只落窗口样本：
  - 重启后能按 step 精确重建窗口（不用等 12 h 重新灌满）；
  - 事后可以把 120 s 节拍的存量数据重算成 1800 s 节拍，做口径对比；
  - 出事时能回答"当时模型看到了什么"。
存储量：2 min 一帧约 16 MiB/年，任何 NAS 都不构成压力。
"""
from __future__ import annotations

import sqlite3
import threading
from pathlib import Path
from typing import Iterable, Optional

SCHEMA = """
CREATE TABLE IF NOT EXISTS raw (
    ts       INTEGER NOT NULL,
    t        REAL    NOT NULL,
    ph       REAL    NOT NULL,
    tds      REAL    NOT NULL,
    tur      REAL    NOT NULL,
    in_dist  INTEGER NOT NULL DEFAULT 1,
    recv_ts  REAL    NOT NULL,
    PRIMARY KEY (ts, t, ph, tds, tur)
);
CREATE INDEX IF NOT EXISTS idx_raw_ts ON raw(ts);

CREATE TABLE IF NOT EXISTS result (
    seq      INTEGER PRIMARY KEY,
    ts       INTEGER NOT NULL,
    status   TEXT    NOT NULL,
    cls      INTEGER NOT NULL,
    cls_raw  INTEGER NOT NULL,
    conf     REAL    NOT NULL,
    agree    INTEGER,
    stable   INTEGER,
    n        INTEGER,
    step_s   INTEGER,
    span_s   REAL,
    probs    TEXT,
    payload  TEXT
);
CREATE INDEX IF NOT EXISTS idx_result_ts ON result(ts);
"""


class Store:
    def __init__(self, path: str | Path) -> None:
        self.path = str(path)
        Path(self.path).parent.mkdir(parents=True, exist_ok=True)
        self._lock = threading.Lock()
        self._db = sqlite3.connect(self.path, check_same_thread=False)
        self._db.execute("PRAGMA journal_mode=WAL")
        self._db.execute("PRAGMA synchronous=NORMAL")
        self._db.executescript(SCHEMA)
        self._db.commit()

    def add_raw(self, ts: int, t: float, ph: float, tds: float, tur: float,
                in_dist: bool, recv_ts: float) -> None:
        with self._lock:
            self._db.execute(
                "INSERT OR IGNORE INTO raw(ts,t,ph,tds,tur,in_dist,recv_ts)"
                " VALUES(?,?,?,?,?,?,?)",
                (int(ts), float(t), float(ph), float(tds), float(tur),
                 1 if in_dist else 0, float(recv_ts)))

    def add_result(self, r, payload: str) -> None:
        import json
        with self._lock:
            self._db.execute(
                "INSERT OR REPLACE INTO result(seq,ts,status,cls,cls_raw,conf,"
                "agree,stable,n,step_s,span_s,probs,payload)"
                " VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?)",
                (r.seq, r.ts, r.status, r.vote_cls, r.cls, r.conf,
                 r.vote_agree, int(r.stable), r.n, r.step_s, r.span_s,
                 json.dumps([round(float(x), 6) for x in r.probs]), payload))

    def commit(self) -> None:
        with self._lock:
            self._db.commit()

    def rebuild(self, window: int, step_s: int, now_ts: int
                ) -> list[tuple[int, float, float, float, float]]:
        """取最近 window 个"每 step 一桶"的样本，用于重启恢复。

        锚点用表里真实的 MAX(ts)，不用墙钟——否则设备时钟偏移或离线回放时
        会一条都查不到（这是很实际的一个坑：固件 NTP 没同步就重启）。
        """
        span = window * step_s * 3
        with self._lock:
            rows = self._db.execute(
                "SELECT ts,t,ph,tds,tur FROM raw"
                " WHERE ts >= (SELECT COALESCE(MAX(ts),0) FROM raw) - ?"
                " ORDER BY ts ASC", (span,)).fetchall()
        out: list[tuple[int, float, float, float, float]] = []
        last_bucket: Optional[int] = None
        for ts, t, ph, tds, tur in rows:
            b = int(ts) // step_s
            if b == last_bucket:
                continue
            last_bucket = b
            out.append((int(ts), t, ph, tds, tur))
        return out[-window:]

    def counts(self) -> dict:
        with self._lock:
            r = self._db.execute("SELECT COUNT(*) FROM raw").fetchone()[0]
            q = self._db.execute("SELECT COUNT(*) FROM result").fetchone()[0]
        return {"raw": r, "result": q}

    def close(self) -> None:
        with self._lock:
            self._db.commit()
            self._db.close()
