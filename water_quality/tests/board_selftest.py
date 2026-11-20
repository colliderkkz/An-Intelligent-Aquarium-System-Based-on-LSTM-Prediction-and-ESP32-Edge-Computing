"""上板校验：通过串口灌 24 帧干净水 / 脏水，读回模型概率，与 Python 期望对比。

用法：python board_selftest.py --port COM4
"""
from __future__ import annotations

import argparse
import os
import re
import sys
import time

try:
    import serial  # pip install pyserial
except ImportError:
    # 兼容把 pyserial 放在包内 _libs/ 的离线用法
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "_libs"))
    import serial


def read_ml(sp: serial.Serial, timeout: float = 1.2) -> str:
    end = time.time() + timeout
    buf = ""
    while time.time() < end:
        chunk = sp.read_all().decode("utf-8", "replace")
        if chunk:
            buf += chunk
            if "#ML," in buf:
                break
        time.sleep(0.05)
    return buf


def parse_ml(text: str):
    for line in text.splitlines():
        if line.startswith("#ML,"):
            d = {}
            for part in line[4:].split(","):
                if "=" in part:
                    k, v = part.split("=", 1)
                    d[k.strip()] = v.strip()
            return d
    return None


def run_case(sp: serial.Serial, name: str, values: str, n: int = 24):
    sp.write(b"ML,RESET\r\n")
    time.sleep(0.3)
    sp.reset_input_buffer()
    for _ in range(n):
        sp.write(f"ML,FEED,{values}\r\n".encode())
        time.sleep(0.08)
    time.sleep(0.3)
    sp.reset_input_buffer()
    sp.write(b"ML,PREDICT\r\n")
    text = read_ml(sp)
    d = parse_ml(text)
    print(f"[{name}] {text.strip()[:400]}")
    return d


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="COM4")
    args = ap.parse_args()

    sp = serial.Serial(args.port, 115200, timeout=0.2)
    time.sleep(3.0)
    sp.reset_input_buffer()

    clean = run_case(sp, "clean", "25.0,7.2,200,5")
    dirty = run_case(sp, "dirty", "31.0,8.6,600,80")

    sp.write(b"ML,RESET\r\n")
    time.sleep(0.3)
    sp.close()

    ok = True
    for name, d, cls in (("clean", clean, 0), ("dirty", dirty, 3)):
        if not d:
            print(f"[FAIL] {name}: 没有解析到 #ML")
            ok = False
            continue
        got = int(d.get("cls", -1))
        p = float(d.get("p", "0"))
        passed = got == cls and p >= 0.9
        print(f"[{'PASS' if passed else 'FAIL'}] {name}: cls={got} p={p}")
        ok = ok and passed

    print("RESULT:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
