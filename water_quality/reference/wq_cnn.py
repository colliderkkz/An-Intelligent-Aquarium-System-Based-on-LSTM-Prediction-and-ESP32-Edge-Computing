"""水质 1D-CNN 参考实现（Python / NumPy）。

与固件 `main/ml/wq_cnn.c` 逐算子一致：
  Conv1(4->32,k5,pad2) -> ReLU -> Conv2(32->32,k5,pad2) -> ReLU
  -> MaxPool2 -> Conv3(32->64,k3,pad1) -> ReLU
  -> [mean over 12, last] (128) -> FC(128->32) -> ReLU -> FC(32->4) -> softmax

输入：24 x 4 原始工程量，最旧在前，通道顺序 [水温℃, pH, TDS ppm, 浊度 NTU]。
输出：4 类概率 + argmax（0 正常 / 1 注意 / 2 警告 / 3 严重）。
"""
from __future__ import annotations

import argparse
import base64
import json
import re
import struct

import numpy as np

# 权重头文件里数组的物理顺序（导出 base64 时也按这个顺序）
ORDER = [
    "conv1_w", "conv1_b",
    "conv2_w", "conv2_b",
    "conv3_w", "conv3_b",
    "fc1_w", "fc1_b",
    "fc2_w", "fc2_b",
    "wq_scaler_mean", "wq_scaler_std",
]

CLASS_NAMES = ["Normal", "Caution", "Warning", "Severe"]
CLASS_LABELS = ["正常", "注意", "警告", "严重"]


def parse_header(path: str) -> dict[str, np.ndarray]:
    """从 wq_cnn_weights.h 解析所有 static const float 数组。"""
    text = open(path, encoding="utf-8").read()
    out: dict[str, np.ndarray] = {}
    for m in re.finditer(r"static const float (\w+)\[(\d+)\]\s*=\s*\{(.*?)\};", text, re.S):
        name, n, body = m.group(1), int(m.group(2)), m.group(3)
        vals = [float(x) for x in re.findall(r"([-+0-9.eE]+)f", body)]
        assert len(vals) == n, f"{name}: {len(vals)} != {n}"
        out[name] = np.asarray(vals, dtype=np.float32)
    missing = [k for k in ORDER if k not in out]
    assert not missing, f"missing arrays: {missing}"
    return out


def predict(raw, w: dict[str, np.ndarray]):
    """返回 (cls, probs)。raw 形状 (24,4)，原始工程量。"""
    mean = w["wq_scaler_mean"]
    std = w["wq_scaler_std"]
    x = np.asarray(raw, dtype=np.float32).reshape(24, 4)
    x = ((x - mean[None, :]) / std[None, :]).T.astype(np.float32)  # (4,24)

    def conv_relu(inp, wname, bname, cout, k, pad):
        cin, lin = inp.shape
        wt = w[wname].reshape(cout, cin, k)
        b = w[bname]
        out = np.zeros((cout, lin), dtype=np.float32)
        for oc in range(cout):
            for t in range(lin):
                acc = float(b[oc])
                for ic in range(cin):
                    for kk in range(k):
                        pos = t + kk - pad
                        if 0 <= pos < lin:
                            acc += float(inp[ic, pos]) * float(wt[oc, ic, kk])
                out[oc, t] = acc if acc > 0.0 else 0.0
        return out

    h1 = conv_relu(x, "conv1_w", "conv1_b", 32, 5, 2)
    h2 = conv_relu(h1, "conv2_w", "conv2_b", 32, 5, 2)
    p2 = h2[:, 0:24:2].copy()
    np.maximum(p2, h2[:, 1:24:2], out=p2)
    h3 = conv_relu(p2, "conv3_w", "conv3_b", 64, 3, 1)

    pooled = np.concatenate([h3.mean(axis=1), h3[:, -1]]).astype(np.float32)
    z = w["fc1_w"].reshape(32, 128) @ pooled + w["fc1_b"]
    z = np.maximum(z, 0.0).astype(np.float32)
    logits = w["fc2_w"].reshape(4, 32) @ z + w["fc2_b"]
    e = np.exp(logits - logits.max())
    probs = (e / e.sum()).astype(np.float32)
    return int(probs.argmax()), probs


def make_cases():
    """三组可复现的验证向量：干净水 / 脏水 / 随机混合。"""
    rng = np.random.default_rng(42)
    clean = np.tile(np.array([25.0, 7.2, 200.0, 5.0], np.float32), (24, 1))
    dirty = np.tile(np.array([31.0, 8.6, 600.0, 80.0], np.float32), (24, 1))
    mix = np.empty((24, 4), np.float32)
    mix[:, 0] = 25.0 + rng.normal(0, 0.6, 24)
    mix[:, 1] = 7.2 + rng.normal(0, 0.12, 24)
    mix[:, 2] = 220.0 + rng.normal(0, 25.0, 24)
    mix[:, 3] = np.abs(rng.normal(10.0, 6.0, 24))
    return [("clean", clean), ("dirty", dirty), ("mixed", mix)]


def weights_to_json(w) -> dict:
    return {k: [float(v) for v in w[k]] for k in ORDER}


def weights_to_b64(w) -> str:
    buf = bytearray()
    for k in ORDER:
        for v in w[k]:
            buf += struct.pack("<f", float(v))
    return base64.b64encode(bytes(buf)).decode("ascii")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--header", default="wq_cnn_weights.h")
    ap.add_argument("--export-dir", default="")
    args = ap.parse_args()

    w = parse_header(args.header)
    print("arrays:", {k: int(w[k].size) for k in ORDER})
    print("params:", sum(int(w[k].size) for k in ORDER if not k.startswith("wq_scaler")))
    print("scalers:", w["wq_scaler_mean"].tolist(), w["wq_scaler_std"].tolist())

    for name, win in make_cases():
        cls, probs = predict(win, w)
        print(f"{name:6s} cls={cls}({CLASS_NAMES[cls]}) probs={np.round(probs, 6).tolist()}")

    if args.export_dir:
        import os
        os.makedirs(args.export_dir, exist_ok=True)
        with open(os.path.join(args.export_dir, "weights.json"), "w", encoding="utf-8") as f:
            json.dump(weights_to_json(w), f, ensure_ascii=False)
        with open(os.path.join(args.export_dir, "wq_weights_b64.txt"), "w", encoding="ascii") as f:
            f.write(weights_to_b64(w))
        print("exported to", args.export_dir)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
