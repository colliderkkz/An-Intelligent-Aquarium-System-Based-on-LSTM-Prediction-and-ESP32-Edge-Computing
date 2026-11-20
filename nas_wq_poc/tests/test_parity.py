"""对拍：NAS 向量化实现 vs 交付包官方 Python 参考实现。

验收标准（与交付包 05_验证/results.md 同一口径）：
  - 三组官方验证向量：argmax 一致，max|Δp| < 1e-5
  - 2000 组随机窗口（含越界值）：argmax 一致率 100%，max|Δp| < 1e-5
随机化是为了覆盖"三组固定用例碰巧一致"的假阳性。
"""
from __future__ import annotations

import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
PKG = os.path.join(os.path.dirname(ROOT), "water_quality")
sys.path.insert(0, ROOT)

from wq_nas.model import Model  # noqa: E402


def load_reference():
    ref_dir = os.path.join(PKG, "reference")
    sys.path.insert(0, ref_dir)
    import wq_cnn  # type: ignore
    w = wq_cnn.parse_header(os.path.join(PKG, "esp32",
                                         "wq_cnn_weights.h"))
    return wq_cnn, w


def main() -> int:
    if not os.path.isdir(PKG):
        print(f"SKIP: 找不到交付包 {PKG}")
        return 1
    wq_cnn, wref = load_reference()
    model = Model.from_json(os.path.join(ROOT, "weights.json"))

    print("=" * 76)
    print("A. 官方三组验证向量")
    print("=" * 76)
    ok_all = True
    for name, win in wq_cnn.make_cases():
        cls_ref, p_ref = wq_cnn.predict(win, wref)
        cls_nas, p_nas = model.classify(win)
        d = float(np.abs(p_nas - p_ref).max())
        ok = cls_ref == cls_nas and d < 1e-5
        ok_all &= ok
        print(f"  {name:<7} ref={cls_ref} nas={cls_nas} max|Δp|={d:.2e} "
              f"{'PASS' if ok else 'FAIL'}")

    print()
    print("=" * 76)
    print("B. 2000 组随机窗口（含训练分布内外的值）")
    print("=" * 76)
    rng = np.random.default_rng(2026)
    worst = 0.0
    mism = 0
    N = 2000
    for i in range(N):
        # 一半在训练分布内，一半刻意拉到量程边界附近
        if i % 2 == 0:
            win = np.empty((24, 4), np.float32)
            win[:, 0] = rng.uniform(20, 30, 24)
            win[:, 1] = rng.uniform(6.1, 8.4, 24)
            win[:, 2] = rng.uniform(55, 480, 24)
            win[:, 3] = rng.uniform(2.5, 109, 24)
        else:
            win = np.empty((24, 4), np.float32)
            win[:, 0] = rng.uniform(-4, 44, 24)
            win[:, 1] = rng.uniform(0.1, 13.9, 24)
            win[:, 2] = rng.uniform(0, 4900, 24)
            win[:, 3] = rng.uniform(0, 2900, 24)
        cr, pr = wq_cnn.predict(win, wref)
        cn, pn = model.classify(win)
        d = float(np.abs(pn - pr).max())
        worst = max(worst, d)
        if cr != cn:
            mism += 1
    print(f"  argmax 不一致: {mism}/{N}")
    print(f"  最大概率误差 : {worst:.2e}  (阈值 1e-5)")
    ok_b = (mism == 0 and worst < 1e-5)
    print(f"  {'PASS' if ok_b else 'FAIL'}")

    print()
    print("=" * 76)
    print(f"总判定: {'PASS' if (ok_all and ok_b) else 'FAIL'}")
    print("=" * 76)
    return 0 if (ok_all and ok_b) else 1


if __name__ == "__main__":
    raise SystemExit(main())
