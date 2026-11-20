#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""SGP30（CO2/TVOC）串口监测工具。

默认**不复位板子**（打开串口后立刻放开 DTR/RTS），所以能看到 IAQ 算法
连续运行时的收敛过程；加 --reset 才会像上位机那样先硬复位一次。

用法:
    python tools\\monitor_sgp30.py                     # COM6, 300 秒, 每 2 秒一次
    python tools\\monitor_sgp30.py -p COM6 -s 600 -i 1
    python tools\\monitor_sgp30.py --reset             # 先复位再测

输出: 屏幕表格 + 同目录/指定路径的 CSV。
"""
from __future__ import annotations

import argparse
import csv
import os
import re
import sys
import time

try:
    import serial
except ImportError:
    print("需要 pyserial：pip install pyserial")
    sys.exit(2)


def hard_reset(ser: serial.Serial) -> None:
    ser.dtr = False
    time.sleep(0.05)
    ser.rts = True
    time.sleep(0.15)
    ser.rts = False
    time.sleep(0.6)


def open_port(port: str, baud: int, reset: bool) -> serial.Serial:
    ser = serial.Serial(port, baud, timeout=0.05, write_timeout=1.0)
    # 打开串口时 Windows 会把 DTR/RTS 拉高；对这块板子等于"准备进下载模式"。
    # 立刻放开，避免误复位/下次复位进 ROM bootloader。
    try:
        ser.dtr = False
        ser.rts = False
    except Exception:
        pass
    if reset:
        hard_reset(ser)
        time.sleep(1.5)
    ser.reset_input_buffer()
    return ser


def query(ser: serial.Serial, cmd: str, timeout: float = 1.0) -> dict:
    """发一条命令，返回第一帧匹配的 key=value。"""
    ser.reset_input_buffer()
    ser.write(("@%s\n" % cmd).encode("utf-8"))
    ser.flush()
    deadline = time.time() + timeout
    buf = b""
    while time.time() < deadline:
        chunk = ser.read(256)
        if chunk:
            buf += chunk
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                text = line.decode("utf-8", "replace").strip("\r")
                if not text.startswith("#"):
                    continue
                body = text[1:].split(",")
                if not body:
                    continue
                kv = {}
                for item in body[1:]:
                    if "=" in item:
                        k, _, v = item.partition("=")
                        kv[k.strip()] = v.strip()
                kv["__verb"] = body[0].strip()
                return kv
        time.sleep(0.01)
    return {}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("-p", "--port", default="COM6")
    ap.add_argument("-b", "--baud", type=int, default=115200)
    ap.add_argument("-s", "--seconds", type=float, default=300)
    ap.add_argument("-i", "--interval", type=float, default=2.0)
    ap.add_argument("--reset", action="store_true", help="测试前先硬复位板子")
    ap.add_argument("--csv", default="", help="CSV 输出路径（默认临时目录）")
    args = ap.parse_args()

    ser = open_port(args.port, args.baud, args.reset)

    info = query(ser, "INFO", 2.0)
    if info:
        print("板子: %s ver=%s uptime=%s boot#%s" %
              (info.get("fw", "?"), info.get("ver", "?"),
               info.get("uptime", "?"), info.get("boot_cnt", "?")))
    scan = query(ser, "I2C,SCAN", 3.0)
    if scan:
        addrs = [v for k, v in scan.items() if k.startswith("a")]
        print("I2C: %s   (SGP30 应为 0x58)" % ", ".join(addrs))

    out = args.csv or os.path.join(os.environ.get("TEMP", "."), "sgp30_trace.csv")
    rows = []
    t0 = time.time()
    print("\n  t(s)   CO2(ppm)  TVOC(ppb)  status   errs  age(ms)")
    print("  " + "-" * 52)
    while time.time() - t0 < args.seconds:
        rec = query(ser, "SENSOR,co2", 1.0)
        t = round(time.time() - t0, 1)
        if not rec or rec.get("__verb") != "SENSOR":
            print("  %6.1f   (无应答)" % t)
            rows.append([t, "", "", "NO_REPLY", "", ""])
        else:
            rows.append([t, rec.get("val", ""), rec.get("tvoc", ""),
                         rec.get("status", ""), rec.get("errs", ""),
                         rec.get("age_ms", "")])
            print("  %6.1f   %-9s %-10s %-8s %-5s %s" %
                  (t, rec.get("val", ""), rec.get("tvoc", ""),
                   rec.get("status", ""), rec.get("errs", ""),
                   rec.get("age_ms", "")))
        time.sleep(max(0.2, args.interval - 1.0))

    ser.close()

    ok = [r for r in rows if r[3] == "OK"]
    nums = []
    tvocs = []
    for r in ok:
        try:
            nums.append(float(r[1]))
            tvocs.append(float(r[2]))
        except (TypeError, ValueError):
            pass
    print("\n== 汇总 ==")
    print("采样 %d 次 / OK %d / 非OK %d" % (len(rows), len(ok), len(rows) - len(ok)))
    if nums:
        print("CO2  min/avg/max = %.0f / %.0f / %.0f ppm" %
              (min(nums), sum(nums) / len(nums), max(nums)))
        print("TVOC min/avg/max = %.0f / %.0f / %.0f ppb" %
              (min(tvocs), sum(tvocs) / len(tvocs), max(tvocs)))
        print("CO2 首末差 = %+.0f ppm" % (nums[-1] - nums[0]))
    with open(out, "w", newline="", encoding="utf-8-sig") as fh:
        w = csv.writer(fh)
        w.writerow(["t_s", "co2_ppm", "tvoc_ppb", "status", "errs", "age_ms"])
        w.writerows(rows)
    print("CSV ->", out)
    return 0


if __name__ == "__main__":
    sys.exit(main())