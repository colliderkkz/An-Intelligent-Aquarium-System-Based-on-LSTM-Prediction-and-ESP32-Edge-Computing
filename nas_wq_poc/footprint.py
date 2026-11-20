"""资源占用实测：常驻内存 / CPU 占空比 / 单次推理耗时。

NAS 选型的真实约束是"能不能 7×24 常驻且不打扰别人"，
所以看的是常驻 RSS 和 CPU 占空比，不是峰值吞吐。
"""
from __future__ import annotations

import ctypes
import ctypes.wintypes as wt
import json
import os
import statistics
import sys
import time

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from wq_nas.model import Model  # noqa: E402


class PMC(ctypes.Structure):
    _fields_ = [
        ("cb", wt.DWORD), ("PageFaultCount", wt.DWORD),
        ("PeakWorkingSetSize", ctypes.c_size_t),
        ("WorkingSetSize", ctypes.c_size_t),
        ("QuotaPeakPagedPoolUsage", ctypes.c_size_t),
        ("QuotaPagedPoolUsage", ctypes.c_size_t),
        ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t),
        ("QuotaNonPagedPoolUsage", ctypes.c_size_t),
        ("PagefileUsage", ctypes.c_size_t),
        ("PeakPagefileUsage", ctypes.c_size_t),
    ]


def rss_mb() -> tuple[float, float]:
    """返回 (当前工作集 MB, 峰值工作集 MB)；非 Windows 返回 (-1,-1)。"""
    if os.name != "nt":
        return -1.0, -1.0
    pmc = PMC()
    pmc.cb = ctypes.sizeof(PMC)
    k32 = ctypes.WinDLL("kernel32", use_last_error=True)
    psapi = ctypes.WinDLL("psapi", use_last_error=True)
    h = k32.GetCurrentProcess()
    psapi.GetProcessMemoryInfo(ctypes.c_void_p(h), ctypes.byref(pmc), pmc.cb)
    return pmc.WorkingSetSize / 2**20, pmc.PeakWorkingSetSize / 2**20


def main() -> int:
    base_rss, _ = rss_mb()
    t0 = time.perf_counter()
    model = Model.from_json(os.path.join(HERE, "weights.json"))
    t_load = (time.perf_counter() - t0) * 1000
    after_rss, _ = rss_mb()

    win = np.tile(np.array([25.0, 7.2, 200.0, 5.0], np.float32), (24, 1))
    for _ in range(200):                       # 预热
        model.predict(win)

    N = 5000
    c0 = time.process_time()
    w0 = time.perf_counter()
    for _ in range(N):
        model.predict(win)
    cpu = time.process_time() - c0
    wall = time.perf_counter() - w0
    _, peak_rss = rss_mb()

    print("=" * 72)
    print("资源占用实测")
    print("=" * 72)
    print(f"  权重加载（含 JSON 解析） : {t_load:8.1f} ms  仅启动一次")
    print(f"  模型权重常驻             : {sum(v.nbytes for v in model._w.values())/1024:8.1f} KiB")
    print(f"  Python 进程 RSS  加载前  : {base_rss:8.1f} MiB（解释器+numpy）")
    print(f"  Python 进程 RSS  加载后  : {after_rss:8.1f} MiB")
    print(f"  Python 进程 RSS  峰值    : {peak_rss:8.1f} MiB")
    print(f"  单次推理 CPU 时间        : {cpu/N*1000:8.4f} ms")
    print(f"  单次推理 wall time       : {wall/N*1000:8.4f} ms")
    print(f"  纯推理吞吐（单核）       : {N/wall:8.0f} 次/秒")
    print()
    print("  折算到实际部署（每 1800 s 推理一次）：")
    per_hour = 3600 / 1800
    cpu_per_infer_s = cpu / N                  # 秒/次
    hour_s = 3600.0
    used_hour_s = per_hour * cpu_per_infer_s    # 每小时消耗秒数
    print(f"    推理频率   : {per_hour:.2f} 次/小时")
    print(f"    每小时 CPU : {used_hour_s * 1000:.3f} ms")
    print(f"    占单核     : {used_hour_s / hour_s * 100:.6f} %"
          f"   （即 1/{hour_s / used_hour_s:,.0f}）")
    print(f"    一天总 CPU : {24 * per_hour * cpu_per_infer_s * 1000:.2f} ms")
    print()
    print("  对照：100 元级 ARM NAS（4 核 A55 @1.7GHz）预计慢 5~8 倍，")
    slower = 8.0
    arm_hour_s = per_hour * cpu_per_infer_s * slower
    print(f"    单次 ~{cpu_per_infer_s * slower * 1000:.2f} ms，"
          f"每小时 {arm_hour_s * 1000:.2f} ms，"
          f"占单核 {arm_hour_s / hour_s * 100:.6f} %")
    print("=" * 72)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
