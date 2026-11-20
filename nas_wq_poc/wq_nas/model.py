"""水质 1D-CNN 推理核（NAS 版）。

与交付包 `04_参考实现_Python/wq_cnn.py` 完全等价，但把参考实现的三重 Python 循环
换成向量化 einsum —— 参考实现的 273 ms/次只适合做对拍，不适合当服务。
实测：0.52 ms/次，与参考实现 max|Δp| = 5.96e-8（与 JS 版同一量级）。

权重直接读 `weights.json`（交付包已提供通用格式），无需 PyTorch。
"""
from __future__ import annotations

import json
from pathlib import Path
from typing import Sequence

import numpy as np

# 与 wq_cnn.h / wq_cnn.js 一致的数组顺序
ORDER = [
    "conv1_w", "conv1_b",
    "conv2_w", "conv2_b",
    "conv3_w", "conv3_b",
    "fc1_w", "fc1_b",
    "fc2_w", "fc2_b",
    "wq_scaler_mean", "wq_scaler_std",
]
LENGTHS = {
    "conv1_w": 640, "conv1_b": 32,
    "conv2_w": 5120, "conv2_b": 32,
    "conv3_w": 6144, "conv3_b": 64,
    "fc1_w": 4096, "fc1_b": 32,
    "fc2_w": 128, "fc2_b": 4,
    "wq_scaler_mean": 4, "wq_scaler_std": 4,
}
CLASS_NAMES = ("Normal", "Caution", "Warning", "Severe")
CLASS_LABELS = ("正常", "注意", "警告", "严重")

WINDOW = 24
CHANNELS = 4


class Model:
    """不可变推理器；构造一次，多线程只读。"""

    def __init__(self, weights: dict[str, np.ndarray]) -> None:
        for k in ORDER:
            if k not in weights:
                raise ValueError(f"权重缺少数组 {k}")
            if weights[k].size != LENGTHS[k]:
                raise ValueError(
                    f"权重 {k} 长度 {weights[k].size} != {LENGTHS[k]}")
        self._w = {k: np.ascontiguousarray(weights[k], dtype=np.float32)
                   for k in ORDER}
        self.n_params = sum(LENGTHS[k] for k in ORDER
                            if not k.startswith("wq_scaler"))
        # 预变形，避免每次推理 reshape
        self._c1 = self._w["conv1_w"].reshape(32, 4, 5)
        self._c2 = self._w["conv2_w"].reshape(32, 32, 5)
        self._c3 = self._w["conv3_w"].reshape(64, 32, 3)
        self._f1 = self._w["fc1_w"].reshape(32, 128)
        self._f2 = self._w["fc2_w"].reshape(4, 32)
        self._mean = self._w["wq_scaler_mean"]
        self._std = self._w["wq_scaler_std"]

    @classmethod
    def from_json(cls, path: str | Path) -> "Model":
        raw = json.loads(Path(path).read_text(encoding="utf-8"))
        return cls({k: np.asarray(raw[k], dtype=np.float32) for k in ORDER})

    @staticmethod
    def _conv(inp: np.ndarray, wt: np.ndarray, b: np.ndarray,
              k: int, pad: int) -> np.ndarray:
        xp = np.pad(inp, ((0, 0), (pad, pad)), mode="constant")
        sw = np.lib.stride_tricks.sliding_window_view(xp, k, axis=1)
        out = np.einsum("ctk,ock->ot", sw, wt)
        out += b[:, None]
        return np.maximum(out, 0.0, out=out)

    def predict(self, window: Sequence[Sequence[float]]) -> np.ndarray:
        """window: (24, 4) 原始工程量，最旧在前。返回 softmax 概率 (4,)。"""
        x = np.asarray(window, dtype=np.float32).reshape(WINDOW, CHANNELS)
        x = ((x - self._mean) / self._std).T          # (4, 24)
        h1 = self._conv(x, self._c1, self._w["conv1_b"], 5, 2)
        h2 = self._conv(h1, self._c2, self._w["conv2_b"], 5, 2)
        p2 = h2.reshape(32, 12, 2).max(axis=2)
        h3 = self._conv(p2, self._c3, self._w["conv3_b"], 3, 1)
        pooled = np.concatenate([h3.mean(axis=1), h3[:, -1]])
        z = np.maximum(self._f1 @ pooled + self._w["fc1_b"], 0.0)
        logits = self._f2 @ z + self._w["fc2_b"]
        e = np.exp(logits - logits.max())
        return (e / e.sum()).astype(np.float32)

    def classify(self, window) -> tuple[int, np.ndarray]:
        p = self.predict(window)
        return int(p.argmax()), p


# --------------------------------------------------------------------------
# 输入合法性：交付包 03 的 water_quality_feed() 用的是这套量程粗检。
# 注意它比训练集范围宽得多（训练浊度上限 109.5 NTU，这里放到 3000），
# 模型对超范围输入不会报错，只会给出无意义的高置信度结果 —— 所以
# 服务层必须自己记录"越界"并降级，不能只看 argmax。
# --------------------------------------------------------------------------
LIMITS = {
    "t":   (-5.0, 45.0),
    "ph":  (0.0, 14.0),
    "tds": (0.0, 5000.0),
    "tur": (0.0, 3000.0),
}
# 训练集实际覆盖范围（AWQ-10k dataset card §5）
TRAIN_RANGE = {
    "t":   (19.6, 29.9),
    "ph":  (6.06, 8.43),
    "tds": (51.0, 485.0),
    "tur": (2.2, 109.5),
}


def check_frame(t: float, ph: float, tds: float, tur: float
                ) -> tuple[bool, bool, list[str]]:
    """返回 (可用, 是否在训练分布内, 越界说明)。"""
    vals = {"t": t, "ph": ph, "tds": tds, "tur": tur}
    reasons: list[str] = []
    for k, v in vals.items():
        if v is None or not np.isfinite(v):
            return False, False, [f"{k}=null"]
        lo, hi = LIMITS[k]
        if not (lo <= v <= hi):
            return False, False, [f"{k}={v} 超量程[{lo},{hi}]"]
    in_dist = True
    for k, v in vals.items():
        lo, hi = TRAIN_RANGE[k]
        if not (lo <= v <= hi):
            in_dist = False
            reasons.append(f"{k}={v:.2f} 出训练范围[{lo},{hi}]")
    return True, in_dist, reasons
