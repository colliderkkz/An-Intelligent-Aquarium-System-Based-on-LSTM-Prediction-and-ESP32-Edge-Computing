#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
智能鱼缸过滤系统 —— 上位机调试工具
=====================================

功能
----
* 实时监控 7 路传感器（水温 / pH / TDS / 浊度 / 水位 / 光照 / CO2）
* 现场点动测试 7 路执行器（加热、循环泵、补水泵、CO2、紫外、水草灯调光、喂食舵机）
* 在线读取/修改 50+ 个运行参数（阈值、标定系数、定时、输出极性），支持掉电保存
* I2C 扫描/寄存器读写、RTC 校时、GPIO 读写、ADC 原始值、日志级别、自检
* 逐路点动自测（一键验证接线顺序）
* 无第三方依赖：串口用 Win32 API（ctypes）实现；已装 pyserial 时优先用 pyserial
* 内置设备仿真器（--sim），没有硬件也能演示/自测

用法
----
    python fish_tank_debugger.py                 # 打开图形界面
    python fish_tank_debugger.py --sim           # 图形界面 + 仿真设备
    python fish_tank_debugger.py --cli --port COM5
    python fish_tank_debugger.py --cmd PING --port COM5
    python fish_tank_debugger.py --cmd "PARAM,LIST" --sim

串口协议（固件 side: main/debug/*）
----------------------------------
    PC -> ESP32 :  @<VERB>[,<ARG>...]\n
    ESP32 -> PC :  #<VERB>[,key=value...]\n      数据/应答
                   !<CODE>,<MESSAGE>\n           错误
    其它不带 # 或 ! 的行是 ESP_LOG 日志，本工具会忽略（可选显示）

接线
----
    把 USB 线插到板子的 UART 口（ESP32-S3 的 GPIO43/44 经 USB 转串口芯片），
    115200 8N1，无需流控。调试协议与日志共用这一路串口。
"""

from __future__ import annotations

import argparse
import base64
import collections
import math
import os
import queue
import random
import sys
import threading
import time
from dataclasses import dataclass, field
from typing import Callable, Dict, List, Optional, Tuple

APP_TITLE = "智能鱼缸过滤系统 · 上位机调试台"
APP_VERSION = "1.1.1"
PROTOCOL_VERSION = 1
DEFAULT_BAUD = 115200

SOF_PC = "@"
SOF_RESP = "#"
SOF_ERR = "!"
SIM_PORT_NAME = "SIMULATOR (无硬件)"

# ---------------------------------------------------------------------------
# 参数表镜像（与固件 main/config/param_store.h 的 PARAM_LIST 一一对应）
#   (name, group, default, min, max, unit, decimals, 中文说明)
# 仅用于仿真模式与界面中文标签；真实设备以 PARAM,LIST 返回值为准。
# ---------------------------------------------------------------------------
PARAM_TABLE: List[Tuple[str, str, float, float, float, str, int, str]] = [
    ("auto_enable",       "sys",   1.0,     0.0,    1.0,   "",     0, "总自动控制"),
    ("manual_timeout_s",  "sys",   120.0,   10.0,   3600.0, "s",   0, "手动模式超时"),
    ("stream_period_ms",  "sys",   1000.0,  100.0,  60000.0, "ms", 0, "遥测周期"),
    ("temp_target_c",     "temp",  26.0,    15.0,   35.0,   "℃",   1, "目标水温"),
    ("temp_hys_c",        "temp",  1.0,     0.2,    5.0,    "℃",   1, "加热回差"),
    ("temp_alert_min_c",  "temp",  18.0,    0.0,    40.0,   "℃",   1, "低温告警"),
    ("temp_alert_max_c",  "temp",  34.0,    0.0,    45.0,   "℃",   1, "高温告警"),
    ("heater_max_on_s",   "temp",  3600.0,  60.0,   7200.0, "s",   0, "加热最长连续时间"),
    ("auto_heater",       "temp",  1.0,     0.0,    1.0,    "",     0, "温控使能"),
    ("ph_min",            "ph",    6.5,     0.0,    14.0,   "pH",  2, "pH 下限"),
    ("ph_max",            "ph",    7.5,     0.0,    14.0,   "pH",  2, "pH 上限"),
    ("ph_slope",          "ph",   -5.887,  -20.0,   20.0,   "pH/V", 3, "pH 斜率"),
    ("ph_offset",         "ph",   21.677,  -30.0,   40.0,   "pH",  3, "pH 截距"),
    ("ph_temp_comp",      "ph",    0.0,     -1.0,    1.0,   "pH/℃", 3, "pH 温度补偿"),
    ("tds_k",             "tds",   1.0,      0.2,    3.0,   "x",   3, "TDS 校准系数"),
    ("tds_alert",         "tds",   500.0,    0.0,    5000.0, "ppm", 0, "TDS 告警"),
    ("turb_slope",        "turb",  -865.68, -5000.0, 0.0,   "NTU/V", 2, "浊度斜率"),
    ("turb_offset",       "turb",  3347.19,  0.0,    8000.0, "NTU", 2, "浊度截距"),
    ("turb_alert",        "turb",  20.0,     0.0,    3000.0, "NTU", 0, "浊度告警"),
    ("level_min_pct",     "level", 30.0,     0.0,    100.0, "%",   0, "启动补水水位"),
    ("level_stop_pct",    "level", 95.0,     0.0,    100.0, "%",   0, "停止补水水位"),
    ("level_alert_pct",   "level", 20.0,     0.0,    100.0, "%",   0, "低水位告警"),
    ("level_min_mv",      "level", 0.0,      0.0,    3300.0, "mV", 0, "水位0%电压"),
    ("level_max_mv",      "level", 3000.0,   100.0,  3300.0, "mV", 0, "水位100%电压"),
    ("refill_max_s",      "level", 600.0,    10.0,   1800.0, "s",  0, "单次补水最长时间"),
    ("refill_cooldown_s", "level", 3600.0,   0.0,    86400.0, "s", 0, "两次补水间隔"),
    ("auto_refill",       "level", 1.0,      0.0,    1.0,    "",    0, "补水使能"),
    ("light_on_h",        "light", 8.0,      0.0,    23.0,   "h",  0, "开灯 时"),
    ("light_on_m",        "light", 0.0,      0.0,    59.0,   "m",  0, "开灯 分"),
    ("light_off_h",       "light", 18.0,     0.0,    23.0,   "h",  0, "关灯 时"),
    ("light_off_m",       "light", 0.0,      0.0,    59.0,   "m",  0, "关灯 分"),
    ("light_brightness",  "light", 100.0,    0.0,    100.0,  "%",  0, "开灯亮度"),
    ("auto_light",        "light", 1.0,      0.0,    1.0,    "",    0, "灯光定时使能"),
    ("co2_with_light",    "light", 1.0,      0.0,    1.0,    "",    0, "CO2随灯开关"),
    ("auto_co2",          "light", 1.0,      0.0,    1.0,    "",    0, "CO2使能"),
    ("co2_alert",         "light", 1200.0,   0.0,    10000.0, "ppm", 0, "CO2告警"),
    ("feed1_h",           "feed",  8.0,      0.0,    23.0,   "h",  0, "喂食1 时"),
    ("feed1_m",           "feed",  0.0,      0.0,    59.0,   "m",  0, "喂食1 分"),
    ("feed2_h",           "feed",  17.0,     0.0,    23.0,   "h",  0, "喂食2 时"),
    ("feed2_m",           "feed",  30.0,     0.0,    59.0,   "m",  0, "喂食2 分"),
    ("feed_angle",        "feed",  120.0,    0.0,    180.0,  "°",  0, "喂食角度"),
    ("feed_hold_ms",      "feed",  1500.0,   100.0,  20000.0, "ms", 0, "喂食保持时间"),
    ("auto_feeder",       "feed",  1.0,      0.0,    1.0,    "",    0, "定时喂食使能"),
    ("uv_on_h",           "uv",    1.0,      0.0,    23.0,   "h",  0, "紫外开 时"),
    ("uv_on_m",           "uv",    30.0,     0.0,    59.0,   "m",  0, "紫外开 分"),
    ("uv_off_h",          "uv",    3.0,      0.0,    23.0,   "h",  0, "紫外关 时"),
    ("uv_off_m",          "uv",    0.0,      0.0,    59.0,   "m",  0, "紫外关 分"),
    ("auto_uv",           "uv",    1.0,      0.0,    1.0,    "",    0, "紫外定时使能"),
    ("pump_always_on",    "pump",  1.0,      0.0,    1.0,    "",    0, "循环泵常开"),
    ("inv_heater",        "out",   0.0,      0.0,    1.0,    "",    0, "加热输出反相"),
    ("inv_pump",          "out",   0.0,      0.0,    1.0,    "",    0, "循环泵输出反相"),
    ("inv_refill",        "out",   0.0,      0.0,    1.0,    "",    0, "补水泵输出反相"),
    ("inv_co2",           "out",   0.0,      0.0,    1.0,    "",    0, "CO2输出反相"),
    ("inv_uv",            "out",   0.0,      0.0,    1.0,    "",    0, "紫外输出反相"),
    ("inv_light",         "out",   0.0,      0.0,    1.0,    "",    0, "灯光输出反相"),
    ("inv_air",           "out",   0.0,      0.0,    1.0,    "",    0, "气泵输出反相"),
]

GROUP_LABELS = {
    "sys": "系统", "temp": "水温", "ph": "pH", "tds": "TDS", "turb": "浊度",
    "level": "水位", "light": "灯光", "feed": "喂食", "uv": "紫外",
    "pump": "水泵", "out": "输出极性",
}

PARAM_LABELS: Dict[str, str] = {row[0]: row[7] for row in PARAM_TABLE}

SENSOR_LABELS = {
    "temp": ("水温", "℃"),
    "ph": ("pH", ""),
    "tds": ("TDS", "ppm"),
    "turb": ("浊度", "NTU"),
    "level": ("水位", "%"),
    "light": ("光照", "lux"),
    "co2": ("CO₂", "ppm"),
}
SENSOR_ORDER = ["temp", "ph", "tds", "turb", "level", "light", "co2"]

ACT_LABELS = {
    "heater": "加热丝",
    "pump": "循环泵",
    "refill": "补水泵",
    "co2": "CO₂ 阀",
    "uv": "紫外灯",
    "air": "气泵",
    "light": "水草灯",
    "feeder": "喂食舵机",
}

# 定时计划支持的目标（与固件 SCH_COUNT 顺序一致）
SCH_ORDER = ["light", "air", "pump", "uv", "co2"]


# ---------------------------------------------------------------------------
# 协议解析
# ---------------------------------------------------------------------------
@dataclass
class Frame:
    kind: str                      # 'resp' | 'err' | 'log'
    verb: str = ""
    fields: List[str] = field(default_factory=list)
    kv: Dict[str, str] = field(default_factory=dict)

    def get(self, key: str, default: str = "") -> str:
        return self.kv.get(key.lower(), default)

    def f(self, key: str, default: float = float("nan")) -> float:
        try:
            return float(self.kv.get(key.lower(), ""))
        except (TypeError, ValueError):
            return default

    def i(self, key: str, default: int = 0) -> int:
        try:
            return int(float(self.kv.get(key.lower(), "")))
        except (TypeError, ValueError):
            return default

    def b(self, key: str, default: bool = False) -> bool:
        v = self.kv.get(key.lower(), "")
        if v in ("1", "on", "true", "yes"):
            return True
        if v in ("0", "off", "false", "no"):
            return False
        return default


def parse_frame(line: str) -> Optional[Frame]:
    """把一行文本解析成 Frame；返回 None 表示是日志或空行。"""
    line = line.strip("\r\n")
    if not line:
        return None

    if line.startswith(SOF_RESP):
        fr = Frame(kind="resp")
        body = line[1:]
    elif line.startswith(SOF_ERR):
        fr = Frame(kind="err")
        body = line[1:]
    else:
        return Frame(kind="log", fields=[line])

    parts = body.split(",")
    fr.verb = parts[0].strip().upper()
    for raw in parts[1:]:
        item = raw.strip()
        if not item:
            continue
        fr.fields.append(item)
        if "=" in item:
            k, _, v = item.partition("=")
            fr.kv[k.strip().lower()] = v.strip()
    return fr


def build_command(verb: str, *args) -> str:
    """拼一条下行命令。

    空字段一律原样保留（包括末尾的）：
    NET,WIFI,ADD,<ssid>,<ent>,<user>,<pass> 里普通 WiFi 的账号就是空串。
    以前这里把所有空串都过滤掉，命令退化成 ,<ssid>,<ent>,<pass>，
    板子把 ent(0) 当成密码存进 NVS（"0,123456788"），结果 WiFi 密码明明
    正确却一直 4WAY_HANDSHAKE_TIMEOUT 连不上。
    后来的修法只丢"末尾空字段"，但 ADD 空密码（开放网络 / 只改账号）时
    又会把尾部截短，同样会被固件误读成密码 "0"。所以这里干脆一个都不丢：
    字段位置就是语义，调用方传了几个就发几个。
    """
    fields = [verb]
    for a in args:
        if a is None:
            continue
        fields.append(str(a).strip())
    return SOF_PC + ",".join(fields) + "\n"


class LineAssembler:
    """把字节流切成行。"""

    # SD,READ 一帧可到 ~570 字符（384B 数据 base64 后 512 + 头部）。
    # 原来 512 会让超长行整行丢弃 —— 下载大块数据时客户端永远收不到回复。
    MAX_LINE = 1024

    def __init__(self) -> None:
        self._buf = bytearray()

    def feed(self, data: bytes) -> List[str]:
        out: List[str] = []
        for b in data:
            if b == 0x0A:  # \n
                try:
                    out.append(self._buf.decode("utf-8", "replace"))
                except Exception:
                    pass
                self._buf.clear()
            elif b != 0x0D:
                if len(self._buf) < self.MAX_LINE:
                    self._buf.append(b)
                else:
                    self._buf.clear()
        return out

    def reset(self) -> None:
        self._buf.clear()


# ---------------------------------------------------------------------------
# 传输层：pyserial / Win32(ctypes) / 仿真器
# ---------------------------------------------------------------------------
class Transport:
    def open(self) -> None:
        raise NotImplementedError

    def close(self) -> None:
        raise NotImplementedError

    def write(self, data: bytes) -> None:
        raise NotImplementedError

    def set_listener(self, cb: Callable[[bytes], None]) -> None:
        self._listener = cb

    def set_error_cb(self, cb: Callable[[str], None]) -> None:
        """链路彻底坏掉（USB 被拔、驱动报错）时的回调。"""
        self._error_cb = cb

    def _error_cb(self, why: str) -> None:
        # 默认空实现；具体传输层通过 set_error_cb 覆盖。
        # 注意要写成普通方法：如果写成类属性 lambda，实例调用时会被绑定成
        # 多传一个 self 的 method，直接 TypeError。
        pass

    is_open: bool = False


class PySerialTransport(Transport):
    def __init__(self, port: str, baud: int) -> None:
        self.port = port
        self.baud = baud
        self._ser = None
        self._thread = None
        self._stop = threading.Event()
        self._listener: Callable[[bytes], None] = lambda d: None

    def open(self) -> None:
        import serial  # type: ignore
        self._ser = serial.Serial(self.port, self.baud, timeout=0.05, write_timeout=1.0)
        # 同 Win32 后端：不要让 DTR/RTS 保持拉高，否则 ESP32 复位会进下载模式
        self.set_dtr(False)
        self.set_rts(False)
        self.is_open = True
        self._stop.clear()
        self._thread = threading.Thread(target=self._reader, daemon=True)
        self._thread.start()

    def set_dtr(self, state: bool) -> None:
        try:
            self._ser.dtr = state
        except Exception:
            pass

    def set_rts(self, state: bool) -> None:
        try:
            self._ser.rts = state
        except Exception:
            pass

    def hard_reset(self) -> None:
        self.set_dtr(False)
        time.sleep(0.05)
        self.set_rts(True)
        time.sleep(0.15)
        self.set_rts(False)
        time.sleep(0.35)

    def _reader(self) -> None:
        fails = 0
        while not self._stop.is_set():
            if not self._ser:
                return
            try:
                n = self._ser.in_waiting
                data = self._ser.read(n if n else 1)
                fails = 0
            except Exception as exc:
                # USB 拔掉后 pyserial 会一直抛 SerialException。
                # 以前这里是 "except: sleep(0.05); continue" —— 死循环、永远不报错，
                # 界面上一直显示"已连接"，其实设备早没了。
                fails += 1
                if fails >= 5:
                    self._error_cb("串口读取失败: %s" % exc)
                    return
                time.sleep(0.05)
                continue
            if data:
                self._listener(data)

    def write(self, data: bytes) -> None:
        if not self._ser:
            raise IOError("串口未打开")
        self._ser.write(data)
        self._ser.flush()

    def close(self) -> None:
        self._stop.set()
        if self._ser:
            try:
                self._ser.close()
            except Exception:
                pass
        self._ser = None
        self.is_open = False


class Win32SerialTransport(Transport):
    """纯 ctypes 的 Windows 串口实现（无需 pyserial）。"""

    GENERIC_READ = 0x80000000
    GENERIC_WRITE = 0x40000000
    OPEN_EXISTING = 3
    FILE_ATTRIBUTE_NORMAL = 0x80
    PURGE_TXCLEAR = 0x0004
    PURGE_RXCLEAR = 0x0008
    MAXDWORD = 0xFFFFFFFF
    # EscapeCommFunction 的 function code
    SETRTS = 3
    CLRRTS = 4
    SETDTR = 5
    CLRDTR = 6

    def __init__(self, port: str, baud: int) -> None:
        self.port = port
        self.baud = baud
        self._handle = None
        self._thread = None
        self._stop = threading.Event()
        self._wlock = threading.Lock()
        self._listener: Callable[[bytes], None] = lambda d: None
        self._k32 = None

    # -- 底层封装 -------------------------------------------------------
    def _load_kernel32(self):
        import ctypes
        import ctypes.wintypes as wt

        k32 = ctypes.WinDLL("kernel32", use_last_error=True)

        k32.CreateFileW.restype = wt.HANDLE
        k32.CreateFileW.argtypes = [wt.LPCWSTR, wt.DWORD, wt.DWORD, wt.LPVOID,
                                    wt.DWORD, wt.DWORD, wt.HANDLE]
        k32.CloseHandle.restype = wt.BOOL
        k32.CloseHandle.argtypes = [wt.HANDLE]
        k32.ReadFile.restype = wt.BOOL
        k32.ReadFile.argtypes = [wt.HANDLE, wt.LPVOID, wt.DWORD,
                                 ctypes.POINTER(wt.DWORD), wt.LPVOID]
        k32.WriteFile.restype = wt.BOOL
        k32.WriteFile.argtypes = [wt.HANDLE, wt.LPCVOID, wt.DWORD,
                                  ctypes.POINTER(wt.DWORD), wt.LPVOID]
        k32.PurgeComm.restype = wt.BOOL
        k32.PurgeComm.argtypes = [wt.HANDLE, wt.DWORD]
        k32.SetCommTimeouts.restype = wt.BOOL
        k32.SetCommTimeouts.argtypes = [wt.HANDLE, wt.LPVOID]
        k32.GetCommState.restype = wt.BOOL
        k32.GetCommState.argtypes = [wt.HANDLE, wt.LPVOID]
        k32.SetCommState.restype = wt.BOOL
        k32.SetCommState.argtypes = [wt.HANDLE, wt.LPVOID]
        k32.BuildCommDCBW.restype = wt.BOOL
        k32.BuildCommDCBW.argtypes = [wt.LPCWSTR, wt.LPVOID]
        k32.EscapeCommFunction.restype = wt.BOOL
        k32.EscapeCommFunction.argtypes = [wt.HANDLE, wt.DWORD]
        self._k32 = k32
        return k32

    def open(self) -> None:
        import ctypes
        import ctypes.wintypes as wt

        if os.name != "nt":
            raise IOError("Win32 串口后端仅支持 Windows，请安装 pyserial")

        k32 = self._load_kernel32()
        path = self.port if self.port.startswith("\\\\.\\") else "\\\\.\\" + self.port
        handle = k32.CreateFileW(path, self.GENERIC_READ | self.GENERIC_WRITE,
                                 0, None, self.OPEN_EXISTING, 0, None)
        # ctypes 用 c_void_p 作为返回值时，INVALID_HANDLE_VALUE 会变成 0xFFFFFFFFFFFFFFFF
        invalid = (None, 0, -1, 0xFFFFFFFFFFFFFFFF)
        if handle in invalid:
            err = ctypes.get_last_error()
            raise IOError("打开 %s 失败 (GetLastError=%d)：端口被占用或不存在" % (self.port, err))
        self._handle = handle

        # DCB（用 BuildCommDCB 构造，字段布局交给系统）
        # 注意：BuildCommDCBW 的 argtypes 是 LPCWSTR，必须传 str（不能 encode 成 bytes），
        # 否则 ctypes 会抛 "argument 1: TypeError: wrong type"。
        dcb = ctypes.create_string_buffer(64)
        if not k32.BuildCommDCBW("baud=%d parity=N data=8 stop=1" % self.baud, dcb):
            self.close()
            raise IOError("BuildCommDCB failed")
        if not k32.SetCommState(handle, dcb):
            err = ctypes.get_last_error()
            self.close()
            raise IOError("设置串口参数失败 (GetLastError=%d)" % err)

        # COMMTIMEOUTS: 读"立即返回已收到的数据 + 最多等 100ms"；
        # 写必须给总超时（最后两个字段）。以前写超时是 0，按 MSDN 的语义
        # "WriteFile 立即返回、可能只写进去一部分" —— 拔线/驱动挂起时
        # 命令被静默截断，甚至把 UI 线程卡死。
        timeouts = (wt.DWORD * 5)(self.MAXDWORD, 0, 100, 0, 1000)
        if not k32.SetCommTimeouts(handle, timeouts):
            self.close()
            raise IOError("SetCommTimeouts failed")

        k32.PurgeComm(handle, self.PURGE_RXCLEAR | self.PURGE_TXCLEAR)

        # 关键：Windows 打开串口时默认把 DTR / RTS 都拉高（DCB 里
        # fDtrControl / fRtsControl 默认 ENABLE）。ESP32 开发板的自动下载电路
        # 把 DTR 反相后接到 GPIO0、RTS 反相后接到 EN，于是：
        #   DTR 高 -> GPIO0 低 -> 下次复位就进下载模式（ROM bootloader 只讲 SLIP，
        #   不认我们的调试协议，表现就是"连上了但一个字节都不回"）。
        # 所以打开后立刻把两根控制线都放开。
        self.set_dtr(False)
        self.set_rts(False)

        self.is_open = True
        self._stop.clear()
        self._thread = threading.Thread(target=self._reader, daemon=True)
        self._thread.start()

    # ---- DTR / RTS（用于复位设备和避免误入下载模式）------------------
    def set_dtr(self, state: bool) -> None:
        if self._handle and self._k32:
            self._k32.EscapeCommFunction(self._handle,
                                         self.SETDTR if state else self.CLRDTR)

    def set_rts(self, state: bool) -> None:
        if self._handle and self._k32:
            self._k32.EscapeCommFunction(self._handle,
                                         self.SETRTS if state else self.CLRRTS)

    def hard_reset(self) -> None:
        """把芯片硬复位并让它从 flash 正常启动（不会进下载模式）。"""
        self.set_dtr(False)      # GPIO0 保持高 = 正常启动
        time.sleep(0.05)
        self.set_rts(True)       # EN 拉低 -> 复位
        time.sleep(0.15)
        self.set_rts(False)      # 释放 -> 启动
        time.sleep(0.35)

    def _reader(self) -> None:
        import ctypes
        import ctypes.wintypes as wt

        k32 = self._k32
        buf = ctypes.create_string_buffer(2048)
        nread = wt.DWORD(0)
        fails = 0
        while not self._stop.is_set():
            if not self._handle:
                return
            ok = k32.ReadFile(self._handle, buf, 2048, ctypes.byref(nread), None)
            if not ok:
                # 设备被拔掉时 ReadFile 会一直失败。以前是死循环 + sleep，
                # 界面永远停在"已连接"。这里连续失败 20 次（约 1 秒）就上报。
                fails += 1
                if fails >= 20:
                    err = ctypes.get_last_error()
                    self._error_cb("串口读取失败 (GetLastError=%d)" % err)
                    return
                time.sleep(0.05)
                continue
            fails = 0
            if nread.value:
                self._listener(buf.raw[:nread.value])

    def write(self, data: bytes) -> None:
        import ctypes
        import ctypes.wintypes as wt

        if not self._handle:
            raise IOError("串口未打开")
        sent = 0
        with self._wlock:
            while sent < len(data):
                written = wt.DWORD(0)
                chunk = data[sent:]
                ok = self._k32.WriteFile(self._handle, chunk, len(chunk),
                                         ctypes.byref(written), None)
                if not ok:
                    raise IOError("串口写入失败")
                if written.value == 0:
                    break
                sent += written.value
        # WriteFile 成功也可能只写了一部分，命令被截断会变成乱码帧
        if sent != len(data):
            raise IOError("串口写入不完整 (%d/%d 字节)" % (sent, len(data)))

    def close(self) -> None:
        self._stop.set()
        if self._handle:
            try:
                self._k32.CloseHandle(self._handle)
            except Exception:
                pass
        self._handle = None
        self.is_open = False


class SimulatorTransport(Transport):
    """内置设备仿真器：完整实现调试协议，便于无硬件演示与自测。"""

    def __init__(self) -> None:
        self._lock = threading.Lock()
        self._out: "queue.Queue[bytes]" = queue.Queue()
        self._thread = None
        self._stop = threading.Event()
        self._listener: Callable[[bytes], None] = lambda d: None
        self.is_open = False
        self._assembler = LineAssembler()

        # 设备状态
        self.params: Dict[str, float] = {row[0]: row[2] for row in PARAM_TABLE}
        self.act = {"heater": 0, "pump": 1, "refill": 0, "co2": 0, "uv": 0, "air": 0}
        self.light = 100.0
        self.servo = 0.0
        self.mode = "AUTO"
        self.manual_left = 0.0
        self.stream_on = False
        self.stream_period = 1.0
        self.t0 = time.time()
        self.temp = 25.6
        self.ph = 7.05
        self.tds = 218.0
        self.turb = 4.2
        self.level = 72.0
        self.light_lux = 35.0
        self.co2 = 620.0

    # -- Transport API --------------------------------------------------
    def open(self) -> None:
        self.is_open = True
        self._stop.clear()
        self._thread = threading.Thread(target=self._loop, daemon=True)
        self._thread.start()
        self._emit("#READY,fw=fish_gundan_panzer,ver=%s,proto=%d,board=ESP32-S3-N16R8 (sim)"
                   % (APP_VERSION, PROTOCOL_VERSION))

    def close(self) -> None:
        self._stop.set()
        self.is_open = False

    def write(self, data: bytes) -> None:
        for line in self._assembler.feed(data):
            try:
                self._handle(line.strip())
            except Exception as exc:  # 仿真器不允许把界面搞崩
                self._emit("!SIM_ERROR,%s" % str(exc).replace(",", " "))

    def set_listener(self, cb: Callable[[bytes], None]) -> None:
        self._listener = cb

    # 让各后端接口一致（仿真器不需要真的复位）
    def set_dtr(self, state: bool) -> None:
        return

    def set_rts(self, state: bool) -> None:
        return

    def hard_reset(self) -> None:
        return

    # -- 内部 -----------------------------------------------------------
    def _emit(self, line: str) -> None:
        self._listener((line + "\r\n").encode("utf-8"))

    def _loop(self) -> None:
        last = time.time()
        while not self._stop.is_set():
            time.sleep(0.05)
            now = time.time()
            dt = now - last
            if dt < 0.2:
                continue
            last = now
            self._tick(dt)
            if self.stream_on and now - getattr(self, "_last_stream", 0.0) >= self.stream_period:
                self._last_stream = now
                self._emit(self._telemetry())

    def _tick(self, dt: float) -> None:
        w = 2 * math.pi / 90.0
        t = time.time() - self.t0
        self.temp = 25.6 + 1.4 * math.sin(w * t) + random.uniform(-0.05, 0.05)
        self.ph = 7.05 + 0.12 * math.sin(w * t * 1.7) + random.uniform(-0.01, 0.01)
        self.tds = 218 + 8 * math.sin(w * t * 0.7)
        self.turb = 4.2 + 0.8 * math.sin(w * t * 1.3)
        self.level = max(5.0, 72 - 0.02 * dt)
        self.light_lux = max(0.0, min(100.0, (self.light or 0) * 0.7 + 20 * math.sin(w * t)))
        self.co2 = 620 + 60 * math.sin(w * t * 0.9)
        if self.mode == "MANUAL":
            self.manual_left = max(0.0, self.manual_left - dt)
            if self.manual_left <= 0:
                self.mode = "AUTO"

    def _sensor_line(self, sid: str) -> str:
        table = {
            "temp": (self.temp, "℃", 2, 2000, 1720),
            "ph": (self.ph, "pH", 2, 2048, 1650),
            "tds": (self.tds, "ppm", 0, 1180, 950),
            "turb": (self.turb, "NTU", 1, 1600, 1290),
            "level": (self.level, "%", 1, 1530, 1230),
            "light": (self.light_lux, "%", 1, 2050, 1650),
            "co2": (self.co2, "ppm", 0, 0, 0),
        }
        val, unit, dec, raw, mv = table.get(sid, (0, "", 1, 0, 0))
        return ("#SENSOR,id=%s,label=%s,val=%.*f,unit=%s,status=OK,age_ms=%d,raw=%d,mv=%d,"
                "period_ms=1000,errs=0,err=OK" %
                (sid, SENSOR_LABELS.get(sid, (sid,))[0], dec, val, unit,
                 int(random.uniform(10, 200)), raw, mv))

    def _telemetry(self) -> str:
        return ("#TEL,temp=%.2f,ph=%.2f,tds=%.0f,turb=%.1f,level=%.1f,light=%.1f,co2=%.0f,"
                "heater=%d,pump=%d,refill=%d,co2v=%d,uv=%d,air=%d,light_pwm=%.0f,servo=%.0f,"
                "mode=%s,up_s=%d" %
                (self.temp, self.ph, self.tds, self.turb, self.level, self.light_lux,
                 self.co2, self.act["heater"], self.act["pump"], self.act["refill"],
                 self.act["co2"], self.act["uv"], self.act["air"], self.light, self.servo,
                 self.mode, int(time.time() - self.t0)))

    def _enter_manual(self) -> None:
        self.mode = "MANUAL"
        self.manual_left = self.params.get("manual_timeout_s", 120.0)

    def _handle(self, line: str) -> None:
        if not line:
            return
        fields = [f.strip() for f in line.split(",")]
        if fields and fields[0].startswith(SOF_PC):
            fields[0] = fields[0][1:]
        verb = fields[0].upper()
        args = fields[1:]

        if verb == "PING":
            self._emit("#PONG,uptime_ms=%d,mode=%s,proto=%d" %
                       (int((time.time() - self.t0) * 1000), self.mode, PROTOCOL_VERSION))
        elif verb == "INFO":
            self._emit("#INFO,fw=fish_gundan_panzer,ver=%s,proto=%d,idf=5.4.0(sim),"
                       "board=ESP32-S3-N16R8(sim),flash_kb=16384,heap=245000,heap_min=210000,"
                       "uptime=0d00:01:23,reset=1,mode=%s,clock=RTC,mac=AA:BB:CC:DD:EE:FF"
                       % (APP_VERSION, PROTOCOL_VERSION, self.mode))
        elif verb in ("SENSORS", "STATUS"):
            for sid in SENSOR_ORDER:
                self._emit(self._sensor_line(sid))
            self._emit("#DONE,count=%d" % len(SENSOR_ORDER))
        elif verb == "SENSOR":
            name = args[0].lower() if args else ""
            if name in SENSOR_LABELS:
                self._emit(self._sensor_line(name))
            else:
                self._emit("!UNKNOWN_ID,unknown sensor id")
        elif verb == "ACT":
            self._sim_act(args)
        elif verb == "PARAM":
            self._sim_param(args)
        elif verb == "I2C":
            if len(args) >= 1 and args[0].upper() == "READ":
                self._emit("#I2C,addr=%s,reg=%s,len=7,data=00,12,34,56,78,9A,BC"
                           % (args[1] if len(args) > 1 else "0x51",
                              args[2] if len(args) > 2 else "0x02"))
            else:
                self._emit("#I2C,count=2,a0=0x51,a1=0x58")
        elif verb == "RTC":
            if args and args[0].upper() == "SET":
                self._emit("#RTC,time=%s,epoch=%d,source=RTC,rtc_ok=1,wday=3,rtc_chip=1"
                           % (",".join(args[1:]), int(time.time())))
            else:
                self._emit("#RTC,time=%s,epoch=%d,source=RTC,rtc_ok=1,wday=3,rtc_chip=1"
                           % (time.strftime("%Y-%m-%d %H:%M:%S"), int(time.time())))
        elif verb == "GPIO":
            if args and args[0].upper() == "LS":
                self._emit("#GPIO," + ",".join("p%d=%d" % (p, random.randint(0, 1))
                                                for p in (1, 2, 3, 4, 5, 6, 7, 8, 9, 10,
                                                          11, 12, 13, 14, 15, 16, 17, 18,
                                                          19, 20, 43, 44, 47, 48)))
            elif args and args[0].upper() == "SET":
                self._emit("#OK,action=gpio_set,pin=%s,level=%s" %
                           (args[1] if len(args) > 1 else "0",
                            args[2] if len(args) > 2 else "0"))
            else:
                self._emit("#GPIO,pin=%s,level=%d" %
                           (args[1] if len(args) > 1 else "0", random.randint(0, 1)))
        elif verb == "ADC":
            names = ["ph", "tds", "turb", "level", "light"]
            want = args[1] if len(args) > 1 and args[0].upper() == "GET" else (
                args[0] if args and args[0].upper() not in ("LIST", "LS") else None)
            for n in names:
                if want and want.lower() != n:
                    continue
                self._emit("#ADC,id=%s,unit=%d,ch=%d,ok=1,raw=%d,mv=%d,err=OK" %
                           (n, 1 if n != "light" else 2, random.randint(0, 4),
                            random.randint(900, 2500), random.randint(700, 2000)))
            if not want:
                self._emit("#DONE,count=5")
        elif verb == "MODE":
            if args:
                sub = args[0].upper()
                if sub == "MANUAL":
                    self._enter_manual()
                elif sub == "AUTO":
                    self.mode = "AUTO"
                    self.manual_left = 0.0
            self._emit("#MODE,mode=%s,manual_left_s=%d,auto_enable=%d,manual_timeout_s=%d" %
                       (self.mode, int(self.manual_left),
                        int(self.params.get("auto_enable", 1)),
                        int(self.params.get("manual_timeout_s", 120))))
        elif verb == "STREAM":
            sub = args[0].upper() if args else ""
            if sub == "ON":
                self.stream_on = True
                if len(args) > 1:
                    try:
                        self.stream_period = max(0.1, float(args[1]) / 1000.0)
                    except ValueError:
                        pass
            elif sub == "OFF":
                self.stream_on = False
            elif sub == "NOW":
                self._emit(self._telemetry())
                return
            self._emit("#OK,action=stream,enabled=%d,period_ms=%d" %
                       (1 if self.stream_on else 0, int(self.stream_period * 1000)))
        elif verb == "TEST":
            self._enter_manual()
            for name in ("heater", "pump", "refill", "co2", "uv", "light", "feeder"):
                self._emit("#TEST,step=%s,on=1,level=%d,ok=1" % (name, random.randint(0, 1)))
            self._emit("#DONE,test=actuators,mode=%s" % self.mode)
        elif verb == "SELFTEST":
            checks = [("rtc_pcf8563", 1, "ack"), ("co2_sgp30", 1, "ack"),
                      ("ds18b20", 1, "%.2f C" % self.temp), ("ph", 1, "raw=2048 mv=1650"),
                      ("tds", 1, "raw=1180 mv=950"), ("turb", 1, "raw=1600 mv=1290"),
                      ("level", 1, "raw=1530 mv=1230"), ("light", 1, "raw=2050 mv=1650"),
                      ("co2", 1, "raw=0 mv=0"), ("rtc_time", 1, "rtc ok"),
                      ("sdcard", 0, "not mounted")]
            for name, ok, detail in checks:
                self._emit("#CHK,name=%s,ok=%d,detail=%s" % (name, ok, detail))
            self._emit("#DONE,test=selftest,failures=%d" %
                       sum(1 for c in checks if not c[1]))
        elif verb == "HELP":
            cmds = ["PING", "INFO", "SENSORS", "SENSOR", "ACT", "PARAM", "I2C",
                    "RTC", "GPIO", "ADC", "MODE", "STREAM", "TEST", "SELFTEST",
                    "LOG", "HELP", "REBOOT"]
            for c in cmds:
                self._emit("#HELP,verb=%s,usage=%s,..." % (c, c))
            self._emit("#DONE,count=%d,proto=%d" % (len(cmds), PROTOCOL_VERSION))
        elif verb == "LOG":
            self._emit("#OK,action=log_level,level=3")
        elif verb == "REBOOT":
            self._emit("#OK,action=reboot")
        elif verb == "SET":  # 兼容手写简写
            self._sim_act(args)
        else:
            self._emit("!UNKNOWN_CMD,unknown verb %s" % verb)

    def _sim_act(self, args: List[str]) -> None:
        if not args or args[0].upper() in ("LIST", "GET"):
            self._emit("#ACT,heater=%d,heater_lvl=%d,pump=%d,pump_lvl=%d,refill=%d,refill_lvl=%d,"
                       "co2=%d,co2_lvl=%d,uv=%d,uv_lvl=%d,air=%d,air_lvl=%d,light=%.0f,light_duty=%d,"
                       "feeder=%.0f,feeder_duty=%d,mode=%s" %
                       (self.act["heater"], self.act["heater"], self.act["pump"],
                        self.act["pump"], self.act["refill"], self.act["refill"],
                        self.act["co2"], self.act["co2"], self.act["uv"], self.act["uv"],
                        self.act["air"], self.act["air"],
                        self.light, int(self.light * 81.9), self.servo,
                        int(205 + self.servo / 180.0 * 819), self.mode))
            return

        name = args[0].lower()
        if name == "feed":
            self._enter_manual()
            self.servo = self.params.get("feed_angle", 120.0)
            self._emit("#OK,action=feed,ok=1")
            return
        if name not in ACT_LABELS:
            self._emit("!UNKNOWN_ID,unknown actuator")
            return
        value = args[1] if len(args) > 1 else "on"
        try:
            numeric = float(value)
            is_bool_word = value.lower() in ("on", "off", "true", "false")
        except ValueError:
            numeric = None
            is_bool_word = False

        if name == "light":
            self.light = numeric if (numeric is not None and not is_bool_word) else (
                100.0 if value.lower() in ("on", "true", "1") else 0.0)
        elif name == "feeder":
            self.servo = numeric if (numeric is not None and not is_bool_word) else 120.0
        else:
            on = value.lower() in ("on", "1", "true", "yes")
            self.act[name] = 1 if on else 0

        self._enter_manual()
        if name == "light":
            self._emit("#OK,act=light,light=%.0f,mode=%s,manual_left_s=%d" %
                       (self.light, self.mode, int(self.manual_left)))
        elif name == "feeder":
            self._emit("#OK,act=feeder,feeder=%.0f,mode=%s,manual_left_s=%d" %
                       (self.servo, self.mode, int(self.manual_left)))
        else:
            self._emit("#OK,act=%s,state=%d,mode=%s,manual_left_s=%d" %
                       (name, self.act[name], self.mode, int(self.manual_left)))

    def _sim_param(self, args: List[str]) -> None:
        sub = args[0].upper() if args else "LIST"
        if sub in ("LIST", "ALL"):
            for row in PARAM_TABLE:
                name, grp, default, lo, hi, unit, dec, _ = row
                self._emit("#P,name=%s,val=%.*f,min=%.*f,max=%.*f,def=%.*f,unit=%s,grp=%s,dec=%d"
                           % (name, dec, self.params.get(name, default),
                              dec, lo, dec, hi, dec, default, unit, grp, dec))
            self._emit("#DONE,count=%d,dirty=0" % len(PARAM_TABLE))
            return
        if sub in ("SAVE", "LOAD", "RESET", "DEFAULT"):
            if sub in ("RESET", "DEFAULT"):
                self.params = {row[0]: row[2] for row in PARAM_TABLE}
            self._emit("#OK,action=%s,ok=1" % sub.lower())
            return
        if len(args) < 2:
            self._emit("!BAD_ARG,missing parameter name")
            return
        name = args[1]
        found = next((row for row in PARAM_TABLE if row[0] == name), None)
        if not found:
            self._emit("!UNKNOWN_ID,unknown parameter name")
            return
        if sub == "GET":
            dec = found[6]
            self._emit("#P,name=%s,val=%.*f,min=%.*f,max=%.*f,def=%.*f,unit=%s,grp=%s,dec=%d"
                       % (name, dec, self.params.get(name, found[2]), dec, found[3],
                          dec, found[4], dec, found[2], found[5], found[1], dec))
            return
        if sub == "SET":
            if len(args) < 3:
                self._emit("!BAD_ARG,usage: PARAM,SET,<name>,<value>")
                return
            try:
                value = float(args[2])
            except ValueError:
                self._emit("!BAD_ARG,value must be numeric")
                return
            lo, hi = found[3], found[4]
            value = max(lo, min(hi, value))
            self.params[name] = value
            if name == "manual_timeout_s" and self.mode == "MANUAL":
                self.manual_left = value
            self._emit("#OK,name=%s,val=%.*f,dirty=1" % (name, found[6], value))
            return
        self._emit("!BAD_ARG,usage: PARAM,<LIST|GET|SET|SAVE|LOAD|RESET>,...")


# ---------------------------------------------------------------------------
# 客户端：连接、收发、事件分发
# ---------------------------------------------------------------------------
class DeviceClient:
    def __init__(self, on_frame: Callable[[Frame], None],
                 on_state: Callable[[str, str], None]) -> None:
        self._on_frame = on_frame
        self._on_state = on_state
        self._queue: "queue.Queue[bytes]" = queue.Queue()
        self._transport: Optional[Transport] = None
        self._assembler = LineAssembler()
        self.connected = False
        self.port = ""
        self.info: Dict[str, str] = {}
        # 传输层在后台线程里发现链路坏了只置标志，由 UI 线程取走处理
        # （Tk 控件不能从非主线程碰）
        self._lost_reason = ""

    def take_lost(self) -> str:
        """取走"链路已断"事件（UI 线程调用）。空串表示没有。"""
        why = self._lost_reason
        self._lost_reason = ""
        return why

    def _on_link_error(self, why: str) -> None:
        if not self._lost_reason:
            self._lost_reason = str(why)[:200]

    # -- 连接管理 -------------------------------------------------------
    def connect(self, port: str, baud: int = DEFAULT_BAUD) -> None:
        self.disconnect()
        if port == SIM_PORT_NAME:
            transport: Transport = SimulatorTransport()
        else:
            try:
                import serial  # noqa: F401
                transport = PySerialTransport(port, baud)
            except Exception:
                transport = Win32SerialTransport(port, baud)

        transport.set_listener(self._on_bytes)
        transport.set_error_cb(self._on_link_error)
        transport.open()
        self._transport = transport
        self.port = port
        self.connected = True
        self._lost_reason = ""
        self._assembler.reset()
        # 打开串口后主动复位一次：既能让设备从 flash 正常启动（而不是被
        # 打开串口时的 DTR 电平留在下载模式），也能保证协议从干净状态开始。
        try:
            transport.hard_reset()
        except Exception:
            pass
        self._on_state("connected", port)

    def hard_reset(self) -> None:
        """手动复位设备（USB 串口 RTS/DTR 序列）。"""
        if self._transport:
            self._transport.hard_reset()

    def disconnect(self) -> None:
        if self._transport:
            try:
                self._transport.close()
            except Exception:
                pass
        if self.connected:
            self._on_state("disconnected", self.port)
        self._transport = None
        self.connected = False

    # -- 收发 -----------------------------------------------------------
    def _on_bytes(self, data: bytes) -> None:
        self._queue.put(data)

    def send(self, verb: str, *args) -> None:
        if not self.connected or not self._transport:
            raise IOError("未连接")
        try:
            self._transport.write(build_command(verb, *args).encode("utf-8"))
        except Exception as exc:
            self._on_link_error("串口写入失败: %s" % exc)
            raise

    def send_raw(self, text: str) -> None:
        if not self.connected or not self._transport:
            raise IOError("未连接")
        if not text.endswith("\n"):
            text += "\n"
        try:
            self._transport.write(text.encode("utf-8"))
        except Exception as exc:
            self._on_link_error("串口写入失败: %s" % exc)
            raise

    def pump(self, max_frames: int = 200) -> int:
        """把已收到的字节解析成 Frame 并回调（必须在 UI 线程调用）。"""
        n = 0
        while n < max_frames:
            try:
                data = self._queue.get_nowait()
            except queue.Empty:
                break
            for line in self._assembler.feed(data):
                frame = parse_frame(line)
                if frame is None:
                    continue
                if frame.kind == "resp" and frame.verb == "INFO":
                    self.info = dict(frame.kv)
                self._on_frame(frame)
                n += 1
        return n


# ---------------------------------------------------------------------------
# 图形界面
# ---------------------------------------------------------------------------
def _setup_chinese_font(root) -> None:
    try:
        import tkinter.font as tkfont
        families = set(tkfont.families(root))
        for name in ("Microsoft YaHei UI", "Microsoft YaHei", "SimHei", "SimSun"):
            if name in families:
                for fname in ("TkDefaultFont", "TkTextFont", "TkMenuFont", "TkHeadingFont"):
                    tkfont.nametofont(fname).configure(family=name, size=10)
                return
    except Exception:
        pass


class SparkChart:
    """极简折线图：三条序列各自归一化显示。"""

    def __init__(self, canvas, series: List[Tuple[str, str, str]], points: int = 120) -> None:
        self.canvas = canvas
        self.series = series       # (key, label, color)
        self.points = points
        self.data = {k: collections.deque(maxlen=points) for k, _, _ in series}
        self.canvas.bind("<Configure>", lambda e: self.redraw())

    def push(self, key: str, value: float) -> None:
        if key in self.data and value == value:  # 排除 NaN
            self.data[key].append(float(value))

    def redraw(self) -> None:
        c = self.canvas
        c.delete("all")
        w = max(50, c.winfo_width())
        h = max(50, c.winfo_height())
        pad_l, pad_r, pad_t, pad_b = 34, 8, 8, 18

        c.create_rectangle(0, 0, w, h, fill="#ffffff", outline="#c8c8c8")
        for i in range(5):
            y = pad_t + (h - pad_t - pad_b) * i / 4.0
            c.create_line(pad_l, y, w - pad_r, y, fill="#eeeeee")

        iw = w - pad_l - pad_r
        ih = h - pad_t - pad_b
        for key, label, color in self.series:
            vals = list(self.data[key])
            if len(vals) < 2:
                continue
            lo, hi = min(vals), max(vals)
            if hi - lo < 1e-6:
                lo, hi = lo - 1.0, hi + 1.0
            step = iw / float(self.points - 1)
            pts = []
            for i, v in enumerate(vals):
                x = pad_l + i * step
                y = pad_t + ih - (v - lo) / (hi - lo) * ih
                pts.extend([x, y])
            c.create_line(*pts, fill=color, width=2, smooth=False)

        # 图例 + 当前值
        x = pad_l + 6
        for key, label, color in self.series:
            vals = list(self.data[key])
            text = "%s --" % label if not vals else "%s %.1f" % (label, vals[-1])
            c.create_rectangle(x, 6, x + 10, 14, fill=color, outline="")
            c.create_text(x + 14, 10, text=text, anchor="w", font=("Consolas", 8))
            x += 92


class DebuggerApp:
    def __init__(self, root, port: str = "", sim: bool = False, start_tab: int = 0,
                 on_home=None) -> None:
        """
        on_home: 传入回调时，关闭窗口/点“返回”会回到启动器的首页而不是退出程序
                 （打包成 exe 后由 launcher.py 使用）。
        """
        import tkinter as tk
        from tkinter import ttk

        self.tk = tk
        self.ttk = ttk
        self.root = root
        self._on_home = on_home
        self._alive = True
        self._frame_seen = False
        # 设备 RTC 时间显示：记录一次 (设备时刻, 本机时刻) 基准，之后本地推算
        self._rtc_base = None
        self._rtc_wall = 0.0
        self._rtc_ok = False
        self._rtc_source = ""
        self._auto_sync_last = 0.0
        self._clock_tick = 0
        # SD 文件读取游标：-1 表示"从末尾开始"，拿到第一段后变成具体偏移
        self.sd_offsets = {"sensors": -1, "log": -1}
        self.root.title("%s v%s" % (APP_TITLE, APP_VERSION))
        self.root.geometry("1200x830")
        self.root.minsize(1000, 700)

        self.client = DeviceClient(self._on_frame, self._on_state)
        # USB 掉线后的自动重连状态
        self._auto_reconnect_left = 0
        self._reconnect_port = ""
        # 传感器最后更新时间：TEL 只上报"有效"的传感器，探头掉线后
        # 这一路会直接从遥测里消失，界面却还停在最后一次的 OK 绿字。
        self._sensor_seen: Dict[str, float] = {}
        # 逐路点动期间暂停周期性轮询（否则 SD,STATUS 会把测试打断）
        self._test_until = 0.0
        self.param_desc: Dict[str, Tuple[str, float, float, float, str, int, str]] = {}
        for row in PARAM_TABLE:
            self.param_desc[row[0]] = (row[1], row[3], row[4], row[2], row[5], row[6], row[7])

        self.sensor_vars: Dict[str, "tk.StringVar"] = {}
        self.sensor_status: Dict[str, "tk.Label"] = {}
        self.act_vars: Dict[str, "tk.BooleanVar"] = {}
        self.mode_var = tk.StringVar(value="未连接")
        self.status_var = tk.StringVar(value="就绪")
        self.show_tel = tk.BooleanVar(value=False)
        self.stream_on = tk.BooleanVar(value=True)
        self.light_var = tk.DoubleVar(value=0.0)
        self.servo_var = tk.DoubleVar(value=0.0)

        # 滑条“用户已修改但还没下发”状态。
        # 遥测每 500ms 就会带一次设备当前值，如果无条件 set() 滑条，
        # 用户刚拖到的位置会被立刻拉回去（看起来像"自动归零"）。
        self._light_dirty = False
        self._servo_dirty = False
        self._light_dirty_ts = 0.0
        self._servo_dirty_ts = 0.0
        self._syncing_slider = False
        self._dirty_timeout_s = 30.0

        self._build_ui()
        self._pump_loop()
        self._chart_loop()

        if 0 <= start_tab < len(self.nb.tabs()):
            self.nb.select(start_tab)

        if sim:
            self.port_var.set(SIM_PORT_NAME)
            self.root.after(200, self.toggle_connection)
        elif port:
            self.port_var.set(port)
            self.root.after(200, self.toggle_connection)

        self.root.protocol("WM_DELETE_WINDOW", self._on_close)

    # -- 界面 -----------------------------------------------------------
    def _build_ui(self) -> None:
        tk = self.tk
        ttk = self.ttk

        top = ttk.Frame(self.root, padding=(8, 6))
        top.pack(fill="x")

        ttk.Label(top, text="端口").pack(side="left")
        self.port_var = tk.StringVar(value="")
        self.port_combo = ttk.Combobox(top, textvariable=self.port_var, width=26, values=self._list_ports())
        self.port_combo.pack(side="left", padx=4)
        ttk.Button(top, text="刷新", width=6, command=self._refresh_ports).pack(side="left")
        ttk.Label(top, text="波特率").pack(side="left", padx=(10, 2))
        self.baud_var = tk.StringVar(value=str(DEFAULT_BAUD))
        ttk.Combobox(top, textvariable=self.baud_var, width=8,
                     values=("9600", "57600", "115200", "230400", "460800", "921600")).pack(side="left")
        self.connect_btn = ttk.Button(top, text="连接", width=10, command=self.toggle_connection)
        self.connect_btn.pack(side="left", padx=8)
        ttk.Button(top, text="复位", width=6, command=self._reset_device).pack(side="left", padx=(0, 8))
        self.clock_var = tk.StringVar(value="RTC --:--:--")
        self.clock_lbl = ttk.Label(top, textvariable=self.clock_var, width=17,
                                   font=("Consolas", 10))
        self.clock_lbl.pack(side="left", padx=(2, 10))

        self.led = tk.Canvas(top, width=14, height=14, highlightthickness=0)
        self.led.pack(side="left", padx=(4, 2))
        self.led_id = self.led.create_oval(2, 2, 12, 12, fill="#bbbbbb", outline="")
        ttk.Label(top, textvariable=self.status_var).pack(side="left", padx=4)

        self.mode_label = tk.Label(top, textvariable=self.mode_var, bg="#e0e0e0",
                                   fg="#333333", padx=8, pady=2, font=("Microsoft YaHei UI", 9, "bold"))
        self.mode_label.pack(side="right", padx=4)
        ttk.Button(top, text="返回自动", width=10, command=self._back_to_auto).pack(side="right", padx=4)

        # 日志区先 pack(side=bottom) 预留高度，否则 Notebook 的 expand 会把它挤扁
        self._build_log()

        self.nb = ttk.Notebook(self.root)
        self.nb.pack(fill="both", expand=True, padx=8, pady=(0, 4))

        self._build_tab_monitor()
        self._build_tab_actuator()
        self._build_tab_schedule()
        self._build_tab_sd()
        self._build_tab_param()
        self._build_tab_wifi()
        self._build_tab_tools()

    def _build_log(self) -> None:
        tk = self.tk
        ttk = self.ttk

        bottom = ttk.Frame(self.root, padding=(8, 0, 8, 6))
        bottom.pack(side="bottom", fill="x")
        logframe = ttk.LabelFrame(bottom, text="串口日志（# 应答 / ! 错误 / > 发送）")
        logframe.pack(fill="both", expand=True)

        self.log = tk.Text(logframe, height=8, wrap="none", font=("Consolas", 9),
                           background="#101418", foreground="#d0d0d0")
        yscroll = ttk.Scrollbar(logframe, orient="vertical", command=self.log.yview)
        self.log.configure(yscrollcommand=yscroll.set)
        self.log.pack(side="left", fill="both", expand=True)
        yscroll.pack(side="right", fill="y")
        self.log.tag_configure("tx", foreground="#5ab0ff")
        self.log.tag_configure("rx", foreground="#9adf9a")
        self.log.tag_configure("err", foreground="#ff6b6b")
        self.log.tag_configure("info", foreground="#c8b6ff")
        self.log.tag_configure("log", foreground="#7a7a7a")

        sendbar = ttk.Frame(bottom)
        sendbar.pack(fill="x", pady=(4, 0))
        ttk.Label(sendbar, text="原始命令").pack(side="left")
        self.cmd_var = tk.StringVar(value="")
        entry = ttk.Entry(sendbar, textvariable=self.cmd_var)
        entry.pack(side="left", fill="x", expand=True, padx=4)
        entry.bind("<Return>", lambda e: self.send_raw())
        ttk.Button(sendbar, text="发送", width=8, command=self.send_raw).pack(side="left")
        ttk.Checkbutton(sendbar, text="显示遥测帧", variable=self.show_tel).pack(side="left", padx=8)
        ttk.Button(sendbar, text="清空日志", width=10,
                   command=lambda: self.log.delete("1.0", "end")).pack(side="left")

    def _build_tab_monitor(self) -> None:
        tk = self.tk
        ttk = self.ttk
        tab = ttk.Frame(self.nb, padding=8)
        self.nb.add(tab, text="  实时监控  ")

        cards = ttk.Frame(tab)
        cards.pack(fill="x")
        for idx, sid in enumerate(SENSOR_ORDER):
            label, unit = SENSOR_LABELS[sid]
            frame = tk.LabelFrame(cards, text=label, padx=8, pady=6,
                                  font=("Microsoft YaHei UI", 9))
            frame.grid(row=idx // 4, column=idx % 4, sticky="nsew", padx=5, pady=5)
            var = tk.StringVar(value="--")
            self.sensor_vars[sid] = var
            tk.Label(frame, textvariable=var, font=("Consolas", 22, "bold"),
                     fg="#1a5276", width=7, anchor="e").pack(side="left")
            tk.Label(frame, text=unit if unit else "  ", font=("Consolas", 10)).pack(side="left", padx=(2, 0))
            status = tk.Label(frame, text="等待", font=("Microsoft YaHei UI", 8), fg="#888888")
            status.pack(side="bottom", anchor="w")
            self.sensor_status[sid] = status
        for c in range(4):
            cards.columnconfigure(c, weight=1)

        chart_frame = ttk.LabelFrame(tab, text="趋势（最近 120 次遥测，各序列独立归一化）")
        chart_frame.pack(fill="both", expand=True, pady=(10, 0))
        self.chart_canvas = tk.Canvas(chart_frame, height=190, background="#ffffff",
                                      highlightthickness=0)
        self.chart_canvas.pack(fill="both", expand=True, padx=4, pady=4)
        self.chart = SparkChart(self.chart_canvas,
                                [("temp", "水温℃", "#e74c3c"),
                                 ("ph", "pH", "#27ae60"),
                                 ("tds", "TDS", "#2980b9")])

        bar = ttk.Frame(tab)
        bar.pack(fill="x", pady=(8, 0))
        ttk.Checkbutton(bar, text="开启遥测流", variable=self.stream_on,
                        command=self._apply_stream).pack(side="left")
        ttk.Label(bar, text="  周期(ms)").pack(side="left")
        self.period_var = tk.StringVar(value="500")
        ttk.Combobox(bar, textvariable=self.period_var, width=8,
                     values=("100", "200", "500", "1000", "2000")).pack(side="left", padx=4)
        ttk.Button(bar, text="应用", width=6, command=self._apply_stream).pack(side="left")
        ttk.Button(bar, text="立即刷新全部", width=14,
                   command=lambda: self._send("SENSORS")).pack(side="left", padx=10)

    def _build_tab_schedule(self) -> None:
        tk = self.tk
        ttk = self.ttk
        tab = ttk.Frame(self.nb, padding=8)
        self.nb.add(tab, text="  定时计划  ")

        head = ttk.LabelFrame(tab, text="执行器定时计划（每天重复；启用后该路完全由计划接管，手动模式下暂停）")
        head.pack(fill="x")

        hdr = ttk.Frame(head)
        hdr.pack(fill="x", padx=6, pady=(6, 2))
        for txt, w in (("目标", 10), ("启用", 6), ("开始时间", 10), ("持续(分)", 9),
                       ("强度(%)", 9), ("状态", 14)):
            ttk.Label(hdr, text=txt, width=w, font=("Microsoft YaHei UI", 9, "bold")).pack(side="left", padx=2)

        self.sch_vars = {}
        self.sch_state = {}
        for name in SCH_ORDER:
            row = ttk.Frame(head)
            row.pack(fill="x", padx=6, pady=1)
            ttk.Label(row, text=ACT_LABELS.get(name, name), width=10).pack(side="left", padx=2)
            en = tk.BooleanVar(value=False)
            ttk.Checkbutton(row, variable=en).pack(side="left", padx=2)
            start = tk.StringVar(value="08:00")
            ttk.Entry(row, textvariable=start, width=10).pack(side="left", padx=2)
            dur = tk.StringVar(value="600")
            ttk.Entry(row, textvariable=dur, width=9).pack(side="left", padx=2)
            pct = tk.StringVar(value="80")
            ttk.Entry(row, textvariable=pct, width=9).pack(side="left", padx=2)
            state = ttk.Label(row, text="未启用", width=14, foreground="#888888")
            state.pack(side="left", padx=2)
            ttk.Button(row, text="应用", width=6,
                       command=lambda n=name: self._apply_sch(n)).pack(side="left", padx=4)
            self.sch_vars[name] = {"en": en, "start": start, "dur": dur, "pct": pct}
            self.sch_state[name] = state

        btns = ttk.Frame(head)
        btns.pack(fill="x", padx=6, pady=8)
        ttk.Button(btns, text="全部应用", command=self._apply_all_sch).pack(side="left")
        ttk.Button(btns, text="保存到 NVS", command=lambda: self._send("SCH", "SAVE")).pack(side="left", padx=8)
        ttk.Button(btns, text="刷新", command=lambda: self._send("SCH", "LIST")).pack(side="left")
        ttk.Button(btns, text="全部清除", command=self._clear_sch).pack(side="left", padx=8)
        ttk.Label(btns, text="（时间填 HH:MM，也可填 0-1439 的分钟数）",
                  foreground="#666666").pack(side="left", padx=6)

        sd = ttk.LabelFrame(tab, text="SD 卡记录（传感器秒级 CSV + 板子工作日志）")
        sd.pack(fill="x", pady=10)
        self.sd_var = tk.StringVar(value="尚未查询")
        tk.Label(sd, textvariable=self.sd_var, justify="left", anchor="w",
                 font=("Consolas", 9), bg="#f4f6f8").pack(fill="x", padx=8, pady=6)
        sdrow = ttk.Frame(sd)
        sdrow.pack(fill="x", padx=6, pady=(0, 8))
        ttk.Button(sdrow, text="刷新状态", command=lambda: self._send("SD", "STATUS")).pack(side="left")
        ttk.Button(sdrow, text="立即刷盘", command=lambda: self._send("SD", "FLUSH")).pack(side="left", padx=8)
        ttk.Label(sdrow, text="  /sdcard/data/日期.csv   /sdcard/log/日期.log",
                  foreground="#666666").pack(side="left", padx=6)

        tip = ttk.LabelFrame(tab, text="说明")
        tip.pack(fill="both", expand=True)
        tk.Label(tip, justify="left", anchor="nw", bg="#f4f6f8",
                 font=("Microsoft YaHei UI", 9),
                 text=(
                     "· 计划按 RTC 时间每天重复，可跨午夜（开始 22:00、持续 240 分钟 → 到次日 02:00）。\n"
                     "· 某一路一旦勾选“启用”，该路的自动策略（水草灯/紫外的时间窗）就让位给计划。\n"
                     "· 强度只对水草灯生效（PWM 调光）；气泵/循环泵/紫外/CO₂ 是开关量，强度 >0 即开。\n"
                     "· 手动模式（MANUAL）期间计划暂停，用户点动优先；返回自动后按计划恢复。\n"
                     "· 改完点“全部应用”，再点“保存到 NVS”才会掉电保留。"
                 )).pack(fill="both", expand=True, padx=8, pady=6)

    def _apply_sch(self, name: str) -> None:
        v = self.sch_vars.get(name)
        if not v:
            return
        self._send("SCH", "SET", name, "1" if v["en"].get() else "0",
                   v["start"].get().strip(), v["dur"].get().strip(), v["pct"].get().strip())

    def _apply_all_sch(self) -> None:
        for name in SCH_ORDER:
            self._apply_sch(name)
        self.root.after(400, lambda: self._send("SCH", "SAVE"))

    def _clear_sch(self) -> None:
        if messagebox.askyesno("全部清除", "把所有定时计划恢复为未启用？"):
            self._send("SCH", "CLR")

    def _on_sch(self, fr: Frame) -> None:
        name = fr.get("name", "")
        v = self.sch_vars.get(name)
        if not v:
            return
        v["en"].set(fr.b("en"))
        v["start"].set(fr.get("start", "00:00"))
        v["dur"].set(fr.get("dur", "60"))
        v["pct"].set(fr.get("pct", "80"))
        st = self.sch_state.get(name)
        if st is not None:
            if fr.b("en"):
                if fr.b("in_window"):
                    st.configure(text="生效中 剩%d分" % fr.i("remain_min"),
                                 foreground="#1e8449")
                else:
                    st.configure(text="%d分后开始" % fr.i("until_start_min"),
                                 foreground="#b9770e")
            else:
                st.configure(text="未启用", foreground="#888888")

    def _sd_probe(self, attempt: int = 0) -> None:
        """启动后自动探测 SD 卡。

        SD 卡从复位到挂载完成大约要 10 秒（SPI 初始化 + FAT 挂载 + 建文件），
        所以不能在 2 秒时只问一次就完事。这里每 3 秒重问一次，最多问 12 次
        （约 36 秒）；一旦确认挂载就停止，避免一直刷协议帧。
        """
        if not self._connected():
            return
        # 注意：mounted=1 时文件可能还没打开（记录任务晚 3~4 秒才建文件），
        # 所以不能只看挂载标志，要拿到真实路径才算探测完成
        if attempt > 0 and getattr(self, "_sd_mounted", False) \
                and self._sd_paths.get("sensors"):
            return
        self._send("SD", "STATUS")
        if attempt == 0:
            try:
                self.sd_info.set("SD：检测中…（卡挂载需要约 10 秒）")
            except Exception:
                pass
        # 前 20 次每 1 秒密集探测（覆盖 SD 挂载窗口，插卡后 1 秒内就能看到状态）；
        # 之后每 3 秒一次，中途插拔也能快速识别。
        delay = 1000 if attempt < 20 else 3000
        if attempt < 400:      # 大约 20 分钟后停止，避免永远刷协议帧
            self.root.after(delay, lambda: self._sd_probe(attempt + 1))

    def _on_sd(self, fr: Frame) -> None:
        mounted = fr.b("mounted")
        active = fr.b("active")
        rows = fr.i("rows")
        was_mounted = getattr(self, "_sd_mounted", False)
        self._sd_mounted = mounted
        # SD,STREAM 的应答里没有 data/log 字段，不能无条件覆盖，
        # 否则刚拿到的路径会被清空成 "-"
        if "data" in fr.kv:
            self._sd_paths["sensors"] = fr.get("data") or ""
        if "log" in fr.kv:
            self._sd_paths["log"] = fr.get("log") or ""
        if "data_size" in fr.kv:
            self._sd_sizes["sensors"] = fr.i("data_size")
        if "log_size" in fr.kv:
            self._sd_sizes["log"] = fr.i("log_size")
        self._sd_stream = fr.b("stream")
        self._sd_sync_stream_btn()

        if mounted:
            self._sd_update_info()
            if not was_mounted:
                self._log("SD 卡已挂载%s（已记录 %d 行）"
                          % ("，正在记录" if active else "，但记录未启动", rows), "info")
        else:
            hint = (fr.get("mount_hint") or "").strip()
            err = (fr.get("mount_err") or "").strip()
            seen = fr.b("card_seen")
            if seen:
                # 卡被识别到了，但文件系统挂不上 —— 最常见就是 exFAT/NTFS/PE 启动盘
                self.sd_info.set(
                    "SD：卡已识别，但文件系统挂载失败\n"
                    "  原因：%s（%s）\n"
                    "  ESP32 只支持 FAT12/16/32 单分区；exFAT、NTFS、\n"
                    "  多分区 PE 启动盘（如 easyu/优启通）都挂不上。\n"
                    "  解决：把卡格式化成 FAT32（单分区）后重插。" % (hint, err))
            else:
                self.sd_info.set(
                    "SD：未检测到卡\n"
                    "  卡可能没插、没插紧，或卡座供电没接\n"
                    "  （每 1~3 秒自动探测；热插拔插回来自动恢复记录）"
                    + (("\n  错误码：%s" % err) if err and err != "ESP_OK" else ""))

    def _build_tab_actuator(self) -> None:
        tk = self.tk
        ttk = self.ttk
        tab = ttk.Frame(self.nb, padding=8)
        self.nb.add(tab, text="  执行器测试  ")

        note = ttk.Label(tab, foreground="#8a5a00",
                         text="提示：任何手动动作都会让固件进入 MANUAL 模式（默认 120s 超时），"
                              "期间自动控制暂停；点右上角“返回自动”可立即恢复。")
        note.pack(fill="x", pady=(0, 8))

        grid = ttk.LabelFrame(tab, text="开关量输出")
        grid.pack(fill="x")
        names = ["heater", "pump", "refill", "co2", "uv", "air"]
        self.act_state_labels: Dict[str, "tk.Label"] = {}
        for i, name in enumerate(names):
            row, col = divmod(i, 3)
            frame = ttk.Frame(grid, padding=4)
            frame.grid(row=row, column=col, sticky="we", padx=2, pady=2)
            self.act_vars[name] = tk.BooleanVar(value=False)
            cb = tk.Checkbutton(frame, text=ACT_LABELS[name], variable=self.act_vars[name],
                                indicatoron=False, width=12, height=2,
                                selectcolor="#2ecc71", activebackground="#a9dfbf",
                                font=("Microsoft YaHei UI", 9),
                                command=lambda n=name: self._toggle_act(n))
            cb.pack()
            lbl = ttk.Label(frame, text="状态: 未知", foreground="#888888",
                            font=("Microsoft YaHei UI", 8))
            lbl.pack(pady=(3, 0))
            self.act_state_labels[name] = lbl
        for c in range(3):
            grid.columnconfigure(c, weight=1)

        pwm = ttk.LabelFrame(tab, text="水草灯调光 / 喂食舵机（拖动后不会被遥测覆盖，点“应用”下发）")
        pwm.pack(fill="x", pady=10)

        ttk.Label(pwm, text="亮度 %").grid(row=0, column=0, sticky="w", padx=6, pady=6)
        self.light_scale = ttk.Scale(pwm, from_=0, to=100, variable=self.light_var,
                                     length=280, command=self._on_light_scale)
        self.light_scale.grid(row=0, column=1, sticky="w")
        self.light_lbl = ttk.Label(pwm, text="0%", width=6)
        self.light_lbl.grid(row=0, column=2, padx=6)
        ttk.Button(pwm, text="应用亮度", command=self._apply_light).grid(row=0, column=3, padx=6)
        self.light_dirty_tag = ttk.Label(pwm, text="", foreground="#c0392b")
        self.light_dirty_tag.grid(row=0, column=4, padx=4)

        ttk.Label(pwm, text="舵机 °").grid(row=1, column=0, sticky="w", padx=6, pady=6)
        self.servo_scale = ttk.Scale(pwm, from_=0, to=180, variable=self.servo_var,
                                     length=280, command=self._on_servo_scale)
        self.servo_scale.grid(row=1, column=1, sticky="w")
        self.servo_lbl = ttk.Label(pwm, text="0°", width=6)
        self.servo_lbl.grid(row=1, column=2, padx=6)
        ttk.Button(pwm, text="应用角度", command=self._apply_servo).grid(row=1, column=3, padx=6)
        ttk.Button(pwm, text="立即喂食一次", command=lambda: self._send("ACT", "FEED")
                   ).grid(row=1, column=5, padx=6)
        self.servo_dirty_tag = ttk.Label(pwm, text="", foreground="#c0392b")
        self.servo_dirty_tag.grid(row=1, column=4, padx=4)

        auto = ttk.LabelFrame(tab, text="自动测试")
        auto.pack(fill="x", pady=(0, 8))
        ttk.Button(auto, text="逐路点动（验证接线顺序）",
                   command=self._run_actuator_test).pack(side="left", padx=6, pady=8)
        ttk.Button(auto, text="硬件自检 SELFTEST",
                   command=lambda: self._send("SELFTEST")).pack(side="left", padx=6)
        ttk.Button(auto, text="读取全部状态",
                   command=lambda: self._send("ACT", "LIST")).pack(side="left", padx=6)
        ttk.Label(auto, text="（点动过程中发任意命令即可中断）",
                  foreground="#8a5a00").pack(side="left", padx=10)

        self.test_box = tk.Text(tab, height=10, font=("Consolas", 9), background="#fbfbfb")
        self.test_box.pack(fill="both", expand=True, pady=(4, 0))

    def _build_tab_sd(self) -> None:
        tk = self.tk
        ttk = self.ttk
        tab = ttk.Frame(self.nb, padding=8)
        self.nb.add(tab, text="  SD 记录  ")

        self._sd_paths = {"sensors": "", "log": ""}
        self._sd_mounted = False
        self._sd_sizes = {"sensors": 0, "log": 0}
        self._sd_stream = False
        self._sd_csv_n = 0
        self._sd_log_n = 0

        bar = ttk.Frame(tab)
        bar.pack(fill="x")
        ttk.Button(bar, text="刷新状态",
                   command=lambda: self._send("SD", "STATUS")).pack(side="left")
        self.sd_stream_btn = ttk.Button(bar, text="开始实时推送",
                                        command=self._sd_toggle_stream)
        self.sd_stream_btn.pack(side="left", padx=6)
        ttk.Button(bar, text="清空显示", command=self._sd_clear).pack(side="left")

        self.sd_info = tk.StringVar(value="未查询")
        ttk.Label(tab, textvariable=self.sd_info, justify="left", anchor="w",
                  font=("Consolas", 9)).pack(fill="x", pady=(8, 6))

        panes = ttk.Frame(tab)
        panes.pack(fill="both", expand=True)
        panes.columnconfigure(0, weight=1, uniform="sd")
        panes.columnconfigure(1, weight=1, uniform="sd")
        panes.rowconfigure(0, weight=1)

        f1 = ttk.LabelFrame(panes, text="传感器数据（每秒一行，边写边显示）")
        f1.grid(row=0, column=0, sticky="nsew", padx=(0, 4))
        self.sd_csv_box = tk.Text(f1, height=18, wrap="none", font=("Consolas", 9),
                                  background="#101418", foreground="#a5d6a7")
        s1 = ttk.Scrollbar(f1, orient="vertical", command=self.sd_csv_box.yview)
        self.sd_csv_box.configure(yscrollcommand=s1.set)
        self.sd_csv_box.pack(side="left", fill="both", expand=True)
        s1.pack(side="right", fill="y")

        f2 = ttk.LabelFrame(panes, text="板子工作日志（边写边显示）")
        f2.grid(row=0, column=1, sticky="nsew", padx=(4, 0))
        self.sd_log_box = tk.Text(f2, height=18, wrap="none", font=("Consolas", 9),
                                  background="#101418", foreground="#b0bec5")
        s2 = ttk.Scrollbar(f2, orient="vertical", command=self.sd_log_box.yview)
        self.sd_log_box.configure(yscrollcommand=s2.set)
        self.sd_log_box.pack(side="left", fill="both", expand=True)
        s2.pack(side="right", fill="y")

        self.sd_dl_var = tk.StringVar(value="已接收：传感器 0 行 / 日志 0 行")
        ttk.Label(tab, textvariable=self.sd_dl_var, anchor="w",
                  foreground="#666666").pack(fill="x", pady=(6, 0))

    def _run_actuator_test(self) -> None:
        """逐路点动：8 路 × (800ms + 150ms) ≈ 7.6 秒。
        这段时间要屏住周期性轮询 —— 固件把"串口上有任何字节"当成中止信号。"""
        self._test_until = time.time() + 12.0
        self.test_box.insert("end", "开始逐路点动（期间暂停轮询，约 8 秒）…\n")
        self.test_box.see("end")
        self._send("TEST", "ACTUATORS", "800")

    def _sd_toggle_stream(self) -> None:
        self._sd_stream = not self._sd_stream
        self._send("SD", "STREAM", "ON" if self._sd_stream else "OFF")
        self._sd_sync_stream_btn()

    def _sd_sync_stream_btn(self) -> None:
        try:
            self.sd_stream_btn.configure(
                text="停止实时推送" if self._sd_stream else "开始实时推送")
        except Exception:
            pass

    def _sd_auto_stream(self) -> None:
        """连上设备后自动开启实时推送，用户不用点按钮。"""
        if not self._connected():
            return
        self._sd_stream = True
        self._send("SD", "STREAM", "ON")
        self._sd_sync_stream_btn()

    def _sd_clear(self) -> None:
        self.sd_csv_box.delete("1.0", "end")
        self.sd_log_box.delete("1.0", "end")
        self._sd_csv_n = 0
        self._sd_log_n = 0
        self._sd_refresh_counter()

    def _sd_refresh_counter(self) -> None:
        self.sd_dl_var.set("已接收：传感器 %d 行 / 日志 %d 行"
                           % (self._sd_csv_n, self._sd_log_n))

    def _sd_append(self, box, text: str, limit: int = 3000) -> None:
        box.insert("end", text + "\n")
        try:
            total = int(box.index("end-1c").split(".")[0])
            if total > limit:
                box.delete("1.0", "%d.0" % (total - limit + 300))
        except Exception:
            pass
        box.see("end")

    def _on_sdline(self, fr: Frame) -> None:
        """板子每写入一行就推一帧过来，这里解码后追加到对应窗格。"""
        kind = (fr.get("kind") or "").lower()
        payload = fr.get("text") or ""
        try:
            text = base64.b64decode(payload).decode("utf-8", "replace")
        except Exception:
            text = payload
        if kind == "csv":
            self._sd_csv_n += 1
            self._sd_append(self.sd_csv_box, text)
        else:
            self._sd_log_n += 1
            self._sd_append(self.sd_log_box, text)
        self._sd_refresh_counter()

    def _sd_update_info(self) -> None:
        mounted = "是" if self._sd_mounted else "否"
        lines = [
            "SD：%s    实时推送：%s" % (mounted, "开" if self._sd_stream else "关"),
            "  传感器  %s" % (self._sd_paths["sensors"] or "-"),
            "  日志    %s" % (self._sd_paths["log"] or "-"),
        ]
        self.sd_info.set("\n".join(lines))









    def _build_tab_param(self) -> None:
        tk = self.tk
        ttk = self.ttk
        tab = ttk.Frame(self.nb, padding=8)
        self.nb.add(tab, text="  参数设置  ")

        bar = ttk.Frame(tab)
        bar.pack(fill="x", pady=(0, 6))
        ttk.Button(bar, text="从设备读取", command=lambda: self._send("PARAM", "LIST")).pack(side="left")
        ttk.Button(bar, text="保存到 NVS", command=lambda: self._send("PARAM", "SAVE")).pack(side="left", padx=6)
        ttk.Button(bar, text="重新载入", command=lambda: self._send("PARAM", "LOAD")).pack(side="left")
        ttk.Button(bar, text="恢复默认", command=self._reset_params).pack(side="left", padx=6)
        ttk.Label(bar, text="分组").pack(side="left", padx=(16, 4))
        self.group_var = tk.StringVar(value="全部")
        self.group_combo = ttk.Combobox(bar, textvariable=self.group_var, width=10,
                                        values=["全部"] + list(GROUP_LABELS.values()),
                                        state="readonly")
        self.group_combo.pack(side="left")
        self.group_combo.bind("<<ComboboxSelected>>", lambda e: self._fill_param_tree())

        columns = ("name", "label", "value", "range", "unit")
        self.param_tree = ttk.Treeview(tab, columns=columns, show="headings", height=14)
        for col, text, width, anchor in (
                ("name", "参数名", 190, "w"),
                ("label", "说明", 170, "w"),
                ("value", "当前值", 100, "e"),
                ("range", "范围", 160, "w"),
                ("unit", "单位", 80, "w")):
            self.param_tree.heading(col, text=text)
            self.param_tree.column(col, width=width, anchor=anchor)
        self.param_tree.pack(fill="both", expand=True)
        self.param_tree.bind("<<TreeviewSelect>>", self._on_param_select)
        self.param_tree.bind("<Double-1>", self._on_param_select)

        edit = ttk.Frame(tab)
        edit.pack(fill="x", pady=6)
        self.sel_var = tk.StringVar(value="")
        ttk.Label(edit, text="选中").pack(side="left")
        ttk.Label(edit, textvariable=self.sel_var, width=26,
                  foreground="#1a5276").pack(side="left")
        ttk.Label(edit, text="新值").pack(side="left", padx=(10, 4))
        self.value_var = tk.StringVar(value="")
        ttk.Entry(edit, textvariable=self.value_var, width=12).pack(side="left")
        ttk.Button(edit, text="写入设备", command=self._apply_param).pack(side="left", padx=6)
        ttk.Button(edit, text="读取该参数", command=self._read_param).pack(side="left")
        self.dirty_lbl = ttk.Label(edit, text="", foreground="#c0392b")
        self.dirty_lbl.pack(side="left", padx=12)

    # ------------------------------------------------------------------
    # WiFi 管理（多网络记忆）
    # ------------------------------------------------------------------
    def _build_tab_wifi(self) -> None:
        tk = self.tk
        ttk = self.ttk
        tab = ttk.Frame(self.nb, padding=8)
        self.nb.add(tab, text="  WiFi  ")

        self._wifi_list_rows = []      # [(idx, ssid, active)]
        self._wifi_scanning = False
        self._wifi_scan_rows = []      # [(ssid, rssi, ch, auth)]
        self._wifi_status = {}

        note = ttk.Label(
            tab, foreground="#8a5a00",
            text="板子最多记 6 组 WiFi 账密。上电/掉线后会先扫一遍，"
                 "只在“当前看得见的”里按信号强弱挑一个连，一个都看不见才按顺序逐个试。")
        note.pack(fill="x", pady=(0, 8))

        # ---- 当前状态 ----
        status = ttk.LabelFrame(tab, text="当前状态")
        status.pack(fill="x")
        row = ttk.Frame(status, padding=(6, 4))
        row.pack(fill="x")
        self.wifi_cur_lbl = ttk.Label(row, text="未连接", font=("Microsoft YaHei UI", 10, "bold"))
        self.wifi_cur_lbl.pack(side="left")
        self.wifi_ip_lbl = ttk.Label(row, text="", foreground="#666666")
        self.wifi_ip_lbl.pack(side="left", padx=12)
        self.wifi_n_lbl = ttk.Label(row, text="已保存 0 组", foreground="#666666")
        self.wifi_n_lbl.pack(side="left")
        ttk.Button(row, text="刷新", width=8, command=self._wifi_refresh).pack(side="right", padx=4)
        ttk.Button(row, text="断开重连", width=10, command=self._wifi_reconnect).pack(side="right", padx=4)

        # ---- 已保存列表 ----
        saved = ttk.LabelFrame(tab, text="已保存的 WiFi（选中后可删除；带 ★ 的是当前在用）")
        saved.pack(fill="both", expand=True, pady=8)

        cols = ("idx", "ssid", "mode", "user", "cur")
        self.wifi_tree = ttk.Treeview(saved, columns=cols, show="headings", height=6)
        self.wifi_tree.heading("idx", text="#")
        self.wifi_tree.heading("ssid", text="SSID")
        self.wifi_tree.heading("mode", text="认证")
        self.wifi_tree.heading("user", text="账号")
        self.wifi_tree.heading("cur", text="状态")
        self.wifi_tree.column("idx", width=40, anchor="center")
        self.wifi_tree.column("ssid", width=200)
        self.wifi_tree.column("mode", width=80, anchor="center")
        self.wifi_tree.column("user", width=120)
        self.wifi_tree.column("cur", width=80, anchor="center")
        self.wifi_tree.pack(side="left", fill="both", expand=True, padx=(4, 0), pady=4)

        btns = ttk.Frame(saved, padding=(4, 4))
        btns.pack(side="right", fill="y")
        ttk.Button(btns, text="删除所选", width=10, command=self._wifi_del).pack(pady=2)
        ttk.Button(btns, text="全部清除", width=10, command=self._wifi_clear).pack(pady=2)

        # ---- 新增 ----
        add = ttk.LabelFrame(tab, text="添加 WiFi（保存后会立即尝试连接）")
        add.pack(fill="x")
        r1 = ttk.Frame(add, padding=(6, 6))
        r1.pack(fill="x")
        ttk.Label(r1, text="SSID").pack(side="left")
        self.wifi_ssid_var = tk.StringVar(value="")
        ttk.Entry(r1, textvariable=self.wifi_ssid_var, width=28).pack(side="left", padx=6)
        self.wifi_ent_var = tk.BooleanVar(value=False)
        chk = ttk.Checkbutton(r1, text="企业网(账号+密码)", variable=self.wifi_ent_var,
                              command=self._wifi_sync_ent)
        chk.pack(side="left", padx=(12, 4))
        ttk.Label(r1, text="账号").pack(side="left")
        self.wifi_user_var = tk.StringVar(value="")
        self.wifi_user_entry = ttk.Entry(r1, textvariable=self.wifi_user_var, width=14)
        self.wifi_user_entry.pack(side="left", padx=4)
        ttk.Label(r1, text="密码").pack(side="left", padx=(6, 0))
        self.wifi_pass_var = tk.StringVar(value="")
        ttk.Entry(r1, textvariable=self.wifi_pass_var, width=20, show="*").pack(side="left", padx=4)
        ttk.Button(r1, text="保存并连接", width=12, command=self._wifi_add).pack(side="left", padx=8)

        # ---- MQTT 服务器账号 ----
        # 账号密码存在 NVS 里，整片擦除/换板后会变空，空着就一直是
        # "Connection refused, not authorized"，以前只能靠手敲命令救回来。
        broker = ttk.LabelFrame(tab, text="服务器账号（NAS Mosquitto，写进 NVS）")
        broker.pack(fill="x", pady=(8, 0))
        br = ttk.Frame(broker, padding=(6, 4))
        br.pack(fill="x")
        ttk.Label(br, text="账号").pack(side="left")
        self.mqtt_user_var = tk.StringVar(value="")
        ttk.Entry(br, textvariable=self.mqtt_user_var, width=16).pack(side="left", padx=4)
        ttk.Label(br, text="密码").pack(side="left", padx=(6, 0))
        self.mqtt_pass_var = tk.StringVar(value="")
        ttk.Entry(br, textvariable=self.mqtt_pass_var, width=22, show="*").pack(side="left", padx=4)
        ttk.Button(br, text="写入板子", width=10, command=self._mqtt_cred_save).pack(side="left", padx=8)
        self.mqtt_cred_lbl = ttk.Label(br, text="", foreground="#666666")
        self.mqtt_cred_lbl.pack(side="left", padx=6)

        r2 = ttk.Frame(add, padding=(6, 0, 6, 6))
        r2.pack(fill="x")
        ttk.Label(r2, text="（也可直接点下面扫到的网络自动填 SSID；"
                           "校园网请勾上\"企业网\"并填学号/工号 + 密码）",
                  foreground="#888888").pack(side="left")

        # ---- 扫描 ----
        scan = ttk.LabelFrame(tab, text="附近 WiFi")
        scan.pack(fill="both", expand=True, pady=(8, 0))
        top = ttk.Frame(scan, padding=(6, 4))
        top.pack(fill="x")
        ttk.Button(top, text="扫描", width=8, command=self._wifi_scan).pack(side="left")
        self.wifi_scan_lbl = ttk.Label(top, text="", foreground="#666666")
        self.wifi_scan_lbl.pack(side="left", padx=10)

        lb_frame = ttk.Frame(scan)
        lb_frame.pack(fill="both", expand=True, padx=4, pady=(0, 4))
        self.wifi_scan_box = tk.Listbox(lb_frame, height=6, font=("Consolas", 9))
        sb = ttk.Scrollbar(lb_frame, orient="vertical", command=self.wifi_scan_box.yview)
        self.wifi_scan_box.configure(yscrollcommand=sb.set)
        self.wifi_scan_box.pack(side="left", fill="both", expand=True)
        sb.pack(side="right", fill="y")
        self.wifi_scan_box.bind("<<ListboxSelect>>", self._wifi_pick_scan)
        self._wifi_sync_ent()

    # ---- 动作 ----
    def _wifi_refresh(self) -> None:
        self._send("NET", "STATUS")
        self._send("NET", "WIFI", "LIST")

    def _wifi_scan(self) -> None:
        self.wifi_scan_lbl.configure(text="扫描中…（约 2~4 秒）")
        self.wifi_scan_box.delete(0, "end")
        self._wifi_scan_rows = []
        self._wifi_scanning = True
        self._send("NET", "SCAN")

    def _wifi_sync_ent(self) -> None:
        """企业网才需要账号，普通 WiFi 把账号框置灰"""
        ent = bool(self.wifi_ent_var.get())
        try:
            self.wifi_user_entry.configure(state="normal" if ent else "disabled")
        except Exception:
            pass

    def _wifi_add(self) -> None:
        ssid = self.wifi_ssid_var.get().strip()
        pw = self.wifi_pass_var.get()
        ent = bool(self.wifi_ent_var.get())
        user = self.wifi_user_var.get().strip()
        if not ssid:
            self.wifi_scan_lbl.configure(text="SSID 不能为空")
            return
        if ent and not user:
            self.wifi_scan_lbl.configure(text="企业网必须填账号（学号/工号）")
            return
        # 格式: NET,WIFI,ADD,<ssid>,<ent>,<user>,<pass>
        # 密码放最后，允许带逗号
        self._send("NET", "WIFI", "ADD", ssid, "1" if ent else "0", user, pw)
        self._send("NET", "SAVE")
        # 密码框先别清空：万一板子回了 ok=0（列表满/参数不合法），
        # 用户还得重打一遍。真正添加成功后由 _wifi_after_ok 清掉。
        self.wifi_scan_lbl.configure(text="已发送，正在连接…", foreground="#666666")
        self.root.after(600, lambda: self._send("NET", "CONNECT"))
        self.root.after(4000, self._wifi_refresh)

    def _mqtt_cred_save(self) -> None:
        user = self.mqtt_user_var.get().strip()
        pw = self.mqtt_pass_var.get()
        if not user:
            self.mqtt_cred_lbl.configure(text="账号不能为空", foreground="#c0392b")
            return
        self._send("NET", "SET", "mqtt_user", user)
        self._send("NET", "SET", "mqtt_pass", pw)
        self._send("NET", "SAVE")
        self.mqtt_pass_var.set("")
        self.mqtt_cred_lbl.configure(text="已写入并保存；重启板子后生效",
                                     foreground="#1e8449")
        self.root.after(1500, self._wifi_refresh)

    def _wifi_del(self) -> None:
        sel = self.wifi_tree.selection()
        if not sel:
            self.wifi_scan_lbl.configure(text="先在列表里选一条")
            return
        idx = self.wifi_tree.item(sel[0], "values")[0]
        self._send("NET", "WIFI", "DEL", str(idx))
        self._send("NET", "SAVE")
        self.root.after(800, self._wifi_refresh)

    def _wifi_clear(self) -> None:
        # 一键清空会直接把板子从所有网络里踢出去（在外地就只能跑一趟现场），
        # 必须先确认。
        try:
            from tkinter import messagebox
            if not messagebox.askyesno(
                    "确认清空",
                    "清空后板子将忘记所有 WiFi（最多 6 组），\n"
                    "如果现在不在现场，就再也连不回来了。\n\n确定要清空吗？",
                    parent=self.root):
                return
        except Exception:
            pass
        self._send("NET", "WIFI", "CLR")
        self._send("NET", "SAVE")
        self.root.after(800, self._wifi_refresh)

    def _wifi_reconnect(self) -> None:
        self._send("NET", "CONNECT")
        self.root.after(6000, self._wifi_refresh)

    def _wifi_pick_scan(self, _event=None) -> None:
        sel = self.wifi_scan_box.curselection()
        if not sel:
            return
        i = sel[0]
        if 0 <= i < len(self._wifi_scan_rows):
            self.wifi_ssid_var.set(self._wifi_scan_rows[i][0])

    # ---- 协议 ----
    def _on_net(self, fr: Frame) -> None:
        self._wifi_status = dict(fr.kv)
        ssid = fr.get("ssid") or ""
        up = fr.b("wifi_up")
        if up:
            self.wifi_cur_lbl.configure(text="已连接：%s" % ssid, foreground="#1e8449")
        else:
            self.wifi_cur_lbl.configure(text="未连接（目标 %s）" % (ssid or "-"), foreground="#c0392b")
        self.wifi_ip_lbl.configure(text="IP %s" % (fr.get("ip") or "-"))
        n = fr.i("wifi_n")
        idx = fr.i("wifi_idx", -1)
        self.wifi_n_lbl.configure(text="已保存 %d 组（当前第 %d 组）" % (n, idx + 1) if n else "已保存 0 组")
        # 板上现有的 MQTT 账号回填到输入框（密码不回显），方便核对是否为空
        mu = fr.get("mqtt_user") or ""
        if mu and mu != "-" and not self.mqtt_user_var.get().strip():
            self.mqtt_user_var.set(mu)

    def _on_wifi(self, fr: Frame) -> None:
        # 列表条目带 idx，扫描结果带 rssi
        if "rssi" in fr.kv:
            self._wifi_scan_rows.append((
                fr.get("ssid"),
                fr.i("rssi"),
                fr.i("ch"),
                fr.i("auth"),
            ))
            self.wifi_scan_box.insert(
                "end", "%-28s %4d dBm  ch%-3d auth%d"
                       % (fr.get("ssid") or "<隐藏>", fr.i("rssi"), fr.i("ch"), fr.i("auth")))
        elif "idx" in fr.kv:
            self._wifi_list_rows.append((
                fr.i("idx"),
                fr.get("ssid"),
                fr.b("active"),
                fr.i("ent"),
                fr.get("user"),
            ))

    def _wifi_after_ok(self, action: str) -> None:
        if action == "wifi_list":
            self.wifi_tree.delete(*self.wifi_tree.get_children())
            for row in sorted(self._wifi_list_rows):
                idx, ssid, active = row[0], row[1], row[2]
                ent = row[3] if len(row) > 3 else 0
                user = row[4] if len(row) > 4 else ""
                self.wifi_tree.insert("", "end", values=(
                    idx, ssid, "账号+密码" if ent else "密码", user,
                    "★ 当前" if active else ""))
            self._wifi_list_rows = []
        elif action in ("wifi_add", "wifi_del", "wifi_clear"):
            if action == "wifi_add":
                self.wifi_pass_var.set("")   # 确认加成功了才清密码
                self.wifi_ssid_var.set("")
                try:
                    self.wifi_scan_lbl.configure(text="已添加并保存", foreground="#1e8449")
                except Exception:
                    pass
            self.root.after(500, self._wifi_refresh)

    def _wifi_after_done(self, fr: Frame) -> None:
        # NET,SCAN 结束：标出哪些是已经记过密码的
        self._wifi_scanning = False
        known = set()
        for row in self.wifi_tree.get_children():
            vals = self.wifi_tree.item(row, "values")
            if len(vals) >= 2:
                known.add(vals[1])
        hit = 0
        for i, (ssid, rssi, ch, auth) in enumerate(self._wifi_scan_rows):
            if ssid in known:
                hit += 1
                self.wifi_scan_box.delete(i)
                self.wifi_scan_box.insert(
                    i, "%-28s %4d dBm  ch%-3d auth%d  *已记录" % (ssid, rssi, ch, auth))
        self.wifi_scan_lbl.configure(text="扫到 %d 个，其中 %d 个已记录"
                                          % (len(self._wifi_scan_rows), hit))
        if not known:
            self._send("NET", "WIFI", "LIST")

    def _build_tab_tools(self) -> None:
        tk = self.tk
        ttk = self.ttk
        tab = ttk.Frame(self.nb, padding=8)
        self.nb.add(tab, text="  总线与工具  ")

        # I2C
        i2c = ttk.LabelFrame(tab, text="I2C")
        i2c.pack(fill="x", pady=4)
        ttk.Button(i2c, text="扫描总线", command=lambda: self._send("I2C", "SCAN")).grid(
            row=0, column=0, padx=6, pady=6, sticky="w")
        ttk.Label(i2c, text="地址").grid(row=0, column=1, padx=(10, 2))
        self.i2c_addr = tk.StringVar(value="0x51")
        ttk.Entry(i2c, textvariable=self.i2c_addr, width=7).grid(row=0, column=2)
        ttk.Label(i2c, text="寄存器").grid(row=0, column=3, padx=(10, 2))
        self.i2c_reg = tk.StringVar(value="0x02")
        ttk.Entry(i2c, textvariable=self.i2c_reg, width=7).grid(row=0, column=4)
        ttk.Label(i2c, text="长度").grid(row=0, column=5, padx=(10, 2))
        self.i2c_len = tk.StringVar(value="7")
        ttk.Entry(i2c, textvariable=self.i2c_len, width=5).grid(row=0, column=6)
        ttk.Button(i2c, text="读寄存器", command=self._i2c_read).grid(row=0, column=7, padx=6)
        ttk.Label(i2c, text="写入字节").grid(row=0, column=8, padx=(10, 2))
        self.i2c_data = tk.StringVar(value="0x00")
        ttk.Entry(i2c, textvariable=self.i2c_data, width=10).grid(row=0, column=9)
        ttk.Button(i2c, text="写寄存器", command=self._i2c_write).grid(row=0, column=10, padx=6)

        # RTC
        rtc = ttk.LabelFrame(tab, text="RTC（掉电后 VL=1 需要用这里校时）")
        rtc.pack(fill="x", pady=4)
        ttk.Button(rtc, text="读取时间", command=lambda: self._send("RTC", "GET")).grid(
            row=0, column=0, padx=6, pady=6)
        self.rtc_var = tk.StringVar(value="")
        ttk.Entry(rtc, textvariable=self.rtc_var, width=22).grid(row=0, column=1)
        ttk.Button(rtc, text="按此校准", command=self._rtc_set).grid(row=0, column=2, padx=6)
        ttk.Button(rtc, text="用电脑时间", command=self._rtc_set_now).grid(row=0, column=3)

        # GPIO
        gpio = ttk.LabelFrame(tab, text="GPIO（26~37 为 Flash/PSRAM，固件已禁止）")
        gpio.pack(fill="x", pady=4)
        ttk.Button(gpio, text="读取全部电平", command=lambda: self._send("GPIO", "LS")).grid(
            row=0, column=0, padx=6, pady=6)
        ttk.Label(gpio, text="引脚").grid(row=0, column=1, padx=(10, 2))
        self.gpio_pin = tk.StringVar(value="16")
        ttk.Entry(gpio, textvariable=self.gpio_pin, width=6).grid(row=0, column=2)
        ttk.Button(gpio, text="读", width=4,
                   command=lambda: self._send("GPIO", "GET", self.gpio_pin.get())).grid(
            row=0, column=3, padx=4)
        ttk.Button(gpio, text="置高", width=6,
                   command=lambda: self._send("GPIO", "SET", self.gpio_pin.get(), "1")).grid(
            row=0, column=4, padx=4)
        ttk.Button(gpio, text="置低", width=6,
                   command=lambda: self._send("GPIO", "SET", self.gpio_pin.get(), "0")).grid(
            row=0, column=5, padx=4)

        # ADC
        adc = ttk.LabelFrame(tab, text="ADC 原始值（标定时用）")
        adc.pack(fill="x", pady=4)
        ttk.Button(adc, text="读取全部 ADC", command=lambda: self._send("ADC", "LIST")).pack(
            side="left", padx=6, pady=6)
        for n in ("ph", "tds", "turb", "level", "light"):
            ttk.Button(adc, text=n, width=6,
                       command=lambda n=n: self._send("ADC", "GET", n)).pack(side="left", padx=2)

        # 其它
        misc = ttk.LabelFrame(tab, text="其它")
        misc.pack(fill="x", pady=4)
        ttk.Button(misc, text="设备信息 INFO", command=lambda: self._send("INFO")).pack(
            side="left", padx=6, pady=6)
        ttk.Button(misc, text="命令表 HELP", command=lambda: self._send("HELP")).pack(side="left")
        ttk.Label(misc, text="日志级别").pack(side="left", padx=(16, 4))
        self.log_level = tk.StringVar(value="info")
        ttk.Combobox(misc, textvariable=self.log_level, width=9,
                     values=("none", "error", "warn", "info", "debug", "verbose"),
                     state="readonly").pack(side="left")
        ttk.Button(misc, text="应用", width=6,
                   command=lambda: self._send("LOG", self.log_level.get())).pack(side="left", padx=4)
        ttk.Button(misc, text="重启设备", command=self._reboot).pack(side="left", padx=16)
        ttk.Button(misc, text="下载完整参数报告", command=self._save_report).pack(side="left")

        self.tool_box = tk.Text(tab, height=12, font=("Consolas", 9), background="#fbfbfb")
        self.tool_box.pack(fill="both", expand=True, pady=(6, 0))

    # -- 支持函数 -------------------------------------------------------
    def _list_ports(self) -> List[str]:
        # 两个来源合并：pyserial（打包后有） + 注册表（永远可用）。
        # 只用 pyserial 的话，某些环境下 comports() 会返回空而不抛异常，
        # 导致下拉框空白。
        ports: List[str] = []
        try:
            from serial.tools import list_ports  # type: ignore
            ports.extend(p.device for p in list_ports.comports())
        except Exception:
            pass
        try:
            import winreg
            key = winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r"HARDWARE\DEVICEMAP\SERIALCOMM")
            i = 0
            while True:
                try:
                    _, value, _ = winreg.EnumValue(key, i)
                except OSError:
                    break
                ports.append(value)
                i += 1
        except Exception:
            pass
        return sorted(set(ports)) + [SIM_PORT_NAME]

    def _refresh_ports(self) -> None:
        self.port_combo.configure(values=self._list_ports())

    def _connected(self) -> bool:
        return self.client.connected

    def _send(self, verb: str, *args) -> None:
        if not self._connected():
            self._log("未连接设备，先点击“连接”", "err")
            return
        try:
            self.client.send(verb, *args)
            self._log("> " + build_command(verb, *args).strip(), "tx")
        except Exception as exc:
            self._log("发送失败: %s" % exc, "err")

    def send_raw(self) -> None:
        text = self.cmd_var.get().strip()
        if not text:
            return
        if not self._connected():
            self._log("未连接设备", "err")
            return
        try:
            self.client.send_raw(text)
            self._log("> " + text, "tx")
            self.cmd_var.set("")
        except Exception as exc:
            self._log("发送失败: %s" % exc, "err")

    def _log(self, text: str, tag: str = "") -> None:
        self.log.insert("end", text + "\n", tag or ())
        self.log.see("end")
        if int(self.log.index("end-1c").split(".")[0]) > 3000:
            self.log.delete("1.0", "500.0")

    def toggle_connection(self) -> None:
        # 手动操作时先取消"自动重连"计数，避免刚断开又被拉起来
        self._auto_reconnect_left = 0
        self._reconnect_port = ""
        if self._connected():
            self.client.disconnect()
            return
        port = self.port_var.get().strip()
        if not port:
            self._log("请选择串口（或选择 SIMULATOR）", "err")
            return
        try:
            baud = int(self.baud_var.get())
        except ValueError:
            baud = DEFAULT_BAUD
        try:
            self.client.connect(port, baud)
        except Exception as exc:
            self._log("连接失败: %s" % exc, "err")
            self.status_var.set("连接失败")

    def _on_state(self, state: str, port: str) -> None:
        if state == "connected":
            self._auto_reconnect_left = 0
            self._reconnect_port = ""
            self.led.itemconfigure(self.led_id, fill="#2ecc71")
            self.connect_btn.configure(text="断开")
            self.status_var.set("已连接 %s，等待设备启动..." % port)
            self._log("已连接 %s（已发送硬复位）" % port, "info")
            self._frame_seen = False
            # 复位后固件要重新初始化（约 0.6~1.5s），之后才开始发命令
            self.root.after(1300, lambda: self._send("INFO"))
            self.root.after(1400, lambda: self._send("PARAM", "LIST"))
            self.root.after(1500, lambda: self._send("SENSORS"))
            self.root.after(1600, lambda: self._send("MODE", "GET"))
            self.root.after(1700, self._apply_stream)
            self.root.after(1800, self._auto_time_sync)
            self.root.after(1800, lambda: self._send("RTC", "GET"))
            self.root.after(1900, lambda: self._send("SCH", "LIST"))
            self._clock_tick = 0
            self.root.after(2000, self._clock_loop)
            self.root.after(2200, lambda: self._sd_probe(0))
            self.root.after(2400, self._sd_auto_stream)   # 自动开启实时推送
            self.root.after(5000, self._check_alive)
        else:
            self.led.itemconfigure(self.led_id, fill="#bbbbbb")
            self.connect_btn.configure(text="连接")
            self.status_var.set("未连接")
            self.mode_var.set("未连接")
            self.mode_label.configure(bg="#e0e0e0", fg="#333333")
            for sid in SENSOR_ORDER:
                self.sensor_status[sid].configure(text="等待", fg="#888888")
            self._sd_stream = False
            try:
                self._sd_sync_stream_btn()
            except Exception:
                pass
            self._log("已断开 %s" % port, "info")

    def _on_close(self) -> None:
        self._alive = False
        # 退出前把设备交还给自动控制：否则它会一直停在"手动暂停"里，
        # 还开着串口实时推送，和手机 App 抢设备。
        if self._connected():
            try:
                self._send("STREAM", "OFF")
                self._send("MODE", "AUTO")
            except Exception:
                pass
            time.sleep(0.15)
        try:
            self.client.disconnect()
        except Exception:
            pass
        if self._on_home is not None:
            self._on_home()          # 打包版：回到启动器首页
        else:
            self.root.destroy()

    # ---- 设备时间显示 -------------------------------------------------
    def _absorb_rtc(self, fr: Frame) -> None:
        """把 RTC 帧里的时间记为基准，后面靠本地时钟推算，避免每秒都去问设备。"""
        tstr = fr.get("time") or ""
        self._rtc_ok = fr.b("rtc_ok")
        self._rtc_source = fr.get("source") or ""
        try:
            base = time.mktime(time.strptime(tstr, "%Y-%m-%d %H:%M:%S"))
        except Exception:
            return
        self._rtc_base = base
        self._rtc_wall = time.time()
        self._paint_clock()

    def _paint_clock(self) -> None:
        if self._rtc_base is None:
            # 没连设备（或刚连上还没拿到时间）时不要显示死板的 --:--:--，
            # 未连接就退化成显示本机时间，至少是个会走的钟
            if self._connected():
                self.clock_var.set("RTC --:--:--")
            else:
                self.clock_var.set("PC  " + time.strftime("%H:%M:%S"))
            return
        cur = self._rtc_base + (time.time() - self._rtc_wall)
        if self._rtc_source == "SNTP":
            prefix = "NET "   # 网络对时，时间可靠
            color = "#186a3b"
        elif self._rtc_source == "RTC-FROZEN":
            prefix = "SW! "   # 芯片能读但走时停止（晶振不起振）
            color = "#c0392b"
        elif self._rtc_ok:
            prefix = "RTC "
            color = "#1a5276"
        else:
            prefix = "SW  "
            color = "#b9770e"
        self.clock_var.set(prefix + time.strftime("%H:%M:%S", time.localtime(cur)))
        try:
            self.clock_lbl.configure(foreground=color)
        except Exception:
            pass

    def _auto_time_sync(self) -> None:
        """把电脑时间写到板子。

        PCF8563 一旦晶振不起振，板子的软件时钟只能靠 tick 累计，会慢慢漂，
        所以每次连上都强制对一次，运行中每分钟检查一次偏差。
        """
        if not self._connected() or self._rtc_base is None:
            return
        now = time.time()
        device_now = self._rtc_base + (now - self._rtc_wall)
        drift = abs(device_now - now)
        if drift <= 3.0:
            return
        lt = time.localtime(now)
        self._send("RTC", "SET", time.strftime("%Y-%m-%d", lt), time.strftime("%H:%M:%S", lt))
        self._log("自动对时：板子偏差 %.1f 秒，已写入电脑时间" % drift, "info")

    def _clock_loop(self) -> None:
        if not self._alive:
            return
        self._paint_clock()
        self._clock_tick += 1
        # 逐路点动（TEST,ACTUATORS）期间屏住所有周期性命令：固件把"串口收到
        # 任何字节"当作中止信号，我们每 2 秒一个 SD,STATUS 会把它打断。
        testing = time.time() < self._test_until
        if self._connected() and not testing:
            if self._rtc_base is None:
                # 还没拿到设备时间：每 3 秒重试（单次 RTC,GET 可能丢帧）
                if self._clock_tick % 3 == 0:
                    self._send("RTC", "GET")
            elif self._clock_tick % 30 == 0:
                self._send("RTC", "GET")   # 已同步，每 30 秒校准一次
            if self._clock_tick % 60 == 0:
                self._auto_time_sync()     # 每分钟纠正一次漂移
            if self._clock_tick % 2 == 0:
                self._send("SD", "STATUS")  # 每 2 秒刷新 SD 状态与文件大小
        self._mark_stale_sensors()
        self.root.after(1000, self._clock_loop)

    def _mark_stale_sensors(self) -> None:
        """遥测里"消失"的传感器 = 探头掉线，把它标灰，别一直显示绿色的 OK。"""
        now = time.time()
        for sid in SENSOR_ORDER:
            seen = self._sensor_seen.get(sid, 0.0)
            if not seen or now - seen <= 12.0:
                continue
            lbl = self.sensor_status.get(sid)
            if lbl is None:
                continue
            try:
                if lbl.cget("text") == "OK":
                    lbl.configure(text="STALE", fg="#b7950b")
            except Exception:
                pass

    def _reset_device(self) -> None:
        if not self._connected():
            self._log("未连接设备", "err")
            return
        try:
            self.client.hard_reset()
            self._frame_seen = False
            self.status_var.set("已发送复位，等待设备启动...")
            self._log("→ 已发送硬复位", "tx")
            self.root.after(1500, lambda: self._send("INFO"))
            self.root.after(2500, self._check_alive)
        except Exception as exc:
            self._log("复位失败: %s" % exc, "err")

    def _check_alive(self) -> None:
        """连接后如果迟迟收不到任何帧，多半是设备停在下载模式。"""
        if not self._connected() or self._frame_seen:
            return
        self.status_var.set("已连接但设备无应答")
        self._log(
            "连上了串口但 5 秒内没收到任何应答 —— 设备多半停在下载模式（ROM bootloader）。\n"
            "  解决办法：点顶部「复位」按钮，或按一下板子上的 RST 键。\n"
            "  （若日志里有 'waiting for download' 就是这种情况）", "err")

    # -- 串口掉线（USB 被拔 / 驱动异常）--------------------------------
    def _handle_link_lost(self, why: str) -> None:
        """由 _pump_loop 在 UI 线程里调用，安全碰控件。"""
        self._log("串口连接丢失（%s）" % why, "err")
        port = self.client.port
        try:
            self.client.disconnect()
        except Exception:
            pass
        self.status_var.set("串口已断开")
        if not port or port == SIM_PORT_NAME:
            return
        # 自动重连：插回去就能继续用，不用手动点"连接"
        self._reconnect_port = port
        self._auto_reconnect_left = 4
        self._log("插回 USB 后会自动重连（最多试 4 次）", "info")
        self.root.after(1500, self._auto_reconnect_tick)

    def _auto_reconnect_tick(self) -> None:
        if not self._alive or self._connected():
            return
        port = self._reconnect_port
        if not port:
            return
        if self._auto_reconnect_left <= 0:
            self._log("自动重连已放弃，请检查 USB 线后点「连接」", "err")
            return
        self._auto_reconnect_left -= 1
        try:
            present = port in self._list_ports()
        except Exception:
            present = True
        if not present:
            self.status_var.set("等待 %s 重新出现…" % port)
            if self._auto_reconnect_left > 0:
                self.root.after(1500, self._auto_reconnect_tick)
            else:
                self._log("没找到 %s，自动重连放弃" % port, "err")
            return
        try:
            baud = int(self.baud_var.get())
        except Exception:
            baud = DEFAULT_BAUD
        self._log("自动重连 %s …" % port, "info")
        try:
            self.client.connect(port, baud)
        except Exception as exc:
            self._log("自动重连失败: %s" % exc, "err")
            if self._auto_reconnect_left > 0:
                self.root.after(2000, self._auto_reconnect_tick)

    # -- 帧处理 ---------------------------------------------------------
    def _on_frame(self, fr: Frame) -> None:
        self._frame_seen = True
        if fr.kind == "err":
            code = fr.verb
            msg = ",".join(fr.fields[1:]) if len(fr.fields) > 1 else ""
            self._log("! %s %s" % (code, msg), "err")
            self.tool_box.insert("end", "[错误] %s %s\n" % (code, msg))
            return

        if fr.kind == "log":
            if not fr.fields:
                return
            text = fr.fields[0]
            if not text.startswith("#") and not text.startswith("!"):
                self._log(text, "log")
            return

        verb = fr.verb
        if verb == "TEL":
            self._on_telemetry(fr)
        elif verb == "READY":
            self._log("设备就绪：%s" % ", ".join(fr.fields), "info")
            self.status_var.set("设备就绪")
            # 连上后拿一次设备时间，偏差大就自动校准
            self.root.after(1200, lambda: self._send("RTC", "CHIP"))
            self.root.after(2600, self._auto_time_sync)
        elif verb == "SENSOR":
            self._on_sensor(fr)
        elif verb == "ACT":
            self._on_act_list(fr)
        elif verb == "OK":
            self._log("# " + ",".join(fr.fields), "rx")
            self._on_ok(fr)
        elif verb == "P":
            self._on_param(fr)
        elif verb == "DONE":
            self._log("# DONE " + ",".join(fr.fields), "rx")
            self._on_done(fr)
        elif verb == "MODE":
            self._on_mode(fr)
        elif verb == "INFO":
            self._log("[设备信息] " + ", ".join("%s=%s" % kv for kv in fr.kv.items()), "info")
            self.tool_box.insert("end", "[INFO] " + ", ".join(
                "%s=%s" % kv for kv in fr.kv.items()) + "\n")
        elif verb == "TEST":
            line = "点动: %s -> 电平 %s" % (fr.get("step"), fr.get("level"))
            self.test_box.insert("end", line + "\n")
            self.test_box.see("end")
            self._log("# " + line, "rx")
        elif verb == "CHK":
            ok = fr.b("ok")
            line = "[%s] %-12s %s" % ("OK" if ok else "FAIL", fr.get("name"), fr.get("detail"))
            self.test_box.insert("end", line + "\n",
                                 ("ok",) if ok else ("bad",))
            self.test_box.see("end")
            self.test_box.tag_configure("ok", foreground="#1e8449")
            self.test_box.tag_configure("bad", foreground="#c0392b")
            self._log("# " + line, "rx")
        elif verb == "RTC":
            self.rtc_var.set(fr.get("time"))
            self._absorb_rtc(fr)
            src = fr.get("source") or ""
            extra = ""
            if src == "SNTP":
                extra = "  << 网络对时，时间可靠"
            elif src == "RTC-FROZEN":
                extra = "  << RTC 走时停止（晶振不起振），靠网络对时+软件时钟"
            self.tool_box.insert("end", "[RTC] %s (source=%s rtc_ok=%s)%s\n"
                                 % (fr.get("time"), src, fr.get("rtc_ok"), extra))
            self.tool_box.see("end")
        elif verb == "SCH":
            self._on_sch(fr)
        elif verb == "SD":
            self._on_sd(fr)
        elif verb == "NET":
            self._on_net(fr)
            self._log("# NET " + ",".join(fr.fields), "rx")
        elif verb == "WIFI":
            self._on_wifi(fr)
        elif verb == "SDLINE":
            self._on_sdline(fr)
        elif verb in ("I2C", "GPIO", "ADC", "PONG", "HELP"):
            self.tool_box.insert("end", "#%s %s\n" % (verb, ",".join(fr.fields)))
            self.tool_box.see("end")
            if verb == "ADC":
                self._log("# ADC %s" % ",".join(fr.fields), "rx")
        else:
            self._log("#%s %s" % (verb, ",".join(fr.fields)), "rx")

    def _on_telemetry(self, fr: Frame) -> None:
        self._update_mode(fr.get("mode", "AUTO"), fr.i("manual_left_s"))
        decimals = {"temp": 2, "ph": 2, "tds": 0, "turb": 1, "level": 1, "light": 1, "co2": 0}
        now = time.time()
        for sid in SENSOR_ORDER:
            if sid in fr.kv:
                self._sensor_seen[sid] = now
                self._set_sensor_value(sid, fr.f(sid), decimals.get(sid, 1), "OK")
                self.chart.push(sid, fr.f(sid))

        states = {"heater": fr.b("heater"), "pump": fr.b("pump"), "refill": fr.b("refill"),
                  "co2": fr.b("co2v"), "uv": fr.b("uv"), "air": fr.b("air")}
        for name, on in states.items():
            if name in self.act_vars:
                self.act_vars[name].set(on)
                self._set_act_label(name, on)
        self._sync_light_slider(fr.f("light_pwm", 0.0))
        self._sync_servo_slider(fr.f("servo", 0.0))

        if self.show_tel.get():
            self._log("# " + ",".join(fr.fields), "rx")

    def _set_sensor_value(self, sid: str, value: float, decimals: int, status: str) -> None:
        if value != value:  # NaN
            self.sensor_vars[sid].set("--")
            self.sensor_status[sid].configure(text="无效", fg="#c0392b")
            return
        self.sensor_vars[sid].set("%.*f" % (decimals, value))
        color = {"OK": "#1e8449", "STALE": "#b7950b", "ERROR": "#c0392b"}.get(status, "#888888")
        self.sensor_status[sid].configure(text=status, fg=color)

    def _on_sensor(self, fr: Frame) -> None:
        sid = fr.get("id")
        if sid not in self.sensor_vars:
            return
        try:
            value = float(fr.get("val"))
        except ValueError:
            value = float("nan")
        dec = 2 if sid in ("temp", "ph") else (0 if sid in ("tds", "co2") else 1)
        self._set_sensor_value(sid, value, dec, fr.get("status", "OK"))
        if value == value:
            self.chart.push(sid, value)
        self._log("# SENSOR %s=%s %s (raw=%s mv=%s)"
                  % (sid, fr.get("val"), fr.get("status"), fr.get("raw"), fr.get("mv")), "rx")

    def _on_act_list(self, fr: Frame) -> None:
        for name in ("heater", "pump", "refill", "co2", "uv", "air"):
            if name in self.act_vars:
                on = fr.b(name)
                self.act_vars[name].set(on)
                self._set_act_label(name, on)
        self._sync_light_slider(fr.f("light", 0.0))
        self._sync_servo_slider(fr.f("feeder", 0.0))
        self._update_mode(fr.get("mode", "AUTO"), 0)
        self._log("# ACT " + ",".join(fr.fields), "rx")

    def _set_act_label(self, name: str, on: bool) -> None:
        lbl = getattr(self, "act_state_labels", {}).get(name)
        if lbl is not None:
            lbl.configure(text="状态: 开" if on else "状态: 关",
                          foreground="#1e8449" if on else "#888888")

    def _on_ok(self, fr: Frame) -> None:
        action = fr.get("action")
        if action in ("wifi_add", "wifi_del", "wifi_clear") and fr.get("ok") == "0":
            # 板子明确回了 ok=0（例如 WiFi 已存满 6 组 / 下标越界）：
            # 以前这里完全没看 ok，用户看到的还是"已添加"，密码框也被清空了。
            try:
                self.wifi_scan_lbl.configure(
                    text="操作失败：板子没接受（最多记 6 组，先删一条再试）",
                    foreground="#c0392b")
            except Exception:
                pass
            return
        if action in ("wifi_list", "wifi_add", "wifi_del", "wifi_clear"):
            try:
                self._wifi_after_ok(action)
            except Exception:
                pass
        if action == "feed" or fr.get("act") == "feeder":
            self.test_box.insert("end", "喂食完成\n")
        # 执行器下发确认后，解除滑条的“未下发”状态，让它重新跟随设备
        act = fr.get("act")
        if act == "light":
            self._light_dirty = False
            self.light_dirty_tag.configure(text="")
        elif act in ("feeder", "feed"):
            self._servo_dirty = False
            self.servo_dirty_tag.configure(text="")
        # PARAM,SET 的应答带 name/val，直接刷新表格，省一次往返
        name = fr.get("name")
        if name and "val" in fr.kv:
            try:
                self.__class__._param_values[name] = float(fr.get("val"))
            except ValueError:
                pass
            self._fill_param_tree()
            self._select_param_row(name)
        if fr.get("dirty") == "1":
            self.dirty_lbl.configure(text="● 有未保存的修改，记得点“保存到 NVS”")

    def _on_param(self, fr: Frame) -> None:
        name = fr.get("name")
        if not name:
            return
        grp = fr.get("grp", "sys")
        lo, hi = fr.f("min"), fr.f("max")
        dec = fr.i("dec", 1)
        default = fr.f("def")
        unit = fr.get("unit", "")
        label = PARAM_LABELS.get(name, name)
        self.param_desc[name] = (grp, lo, hi, default, unit, dec, label)
        self.__class__._param_values[name] = fr.f("val")
        self._param_dirty = fr.get("dirty", "")
        # 55 条参数逐条重建界面太慢，等 PARAM,LIST 发完 DONE 再刷新一次

    _param_values: Dict[str, float] = {}
    _param_dirty: str = ""

    def _on_done(self, fr: Frame) -> None:
        if getattr(self, "_wifi_scanning", False):
            self._wifi_after_done(fr)
        # PARAM,LIST 的 DONE 带 dirty 字段，用它区分是哪条命令的结束
        if "dirty" in fr.kv:
            self.dirty_lbl.configure(
                text="● 有未保存的修改，记得点“保存到 NVS”" if fr.b("dirty") else "参数已同步")
            self._fill_param_tree()
        if fr.get("test") == "selftest":
            fails = fr.i("failures")
            self.test_box.insert("end", "自检完成，失败项: %d\n" % fails)
        if fr.get("test") == "actuators":
            self._test_until = 0.0
            self.test_box.insert("end", "点动测试结束%s\n"
                                 % ("（已中止）" if fr.b("aborted") else ""))
            self.test_box.see("end")

    def _update_mode(self, mode: str, manual_left: int) -> None:
        if mode.upper() == "MANUAL":
            text = "手动 MANUAL %ds" % manual_left if manual_left else "手动 MANUAL"
            self.mode_var.set(text)
            self.mode_label.configure(bg="#f39c12", fg="#ffffff")
        else:
            self.mode_var.set("自动 AUTO")
            self.mode_label.configure(bg="#2ecc71", fg="#ffffff")

    def _on_mode(self, fr: Frame) -> None:
        self._update_mode(fr.get("mode", "AUTO"), fr.i("manual_left_s"))
        self._log("# MODE " + ",".join(fr.fields), "rx")

    # -- 参数表 ---------------------------------------------------------
    def _fill_param_tree(self) -> None:
        if not hasattr(self, "param_tree"):
            return
        sel = self.group_var.get() if hasattr(self, "group_var") else "全部"
        self.param_tree.delete(*self.param_tree.get_children())
        for name, (grp, lo, hi, default, unit, dec, label) in self.param_desc.items():
            if sel != "全部" and GROUP_LABELS.get(grp, grp) != sel:
                continue
            value = self.__class__._param_values.get(name)
            vtext = "" if value is None else ("%.*f" % (dec, value))
            rng = ("%.*f ~ %.*f" % (dec, lo, dec, hi))
            self.param_tree.insert("", "end", iid=name,
                                   values=(name, label, vtext, rng, unit))

    def _on_param_select(self, _event=None) -> None:
        sel = self.param_tree.selection()
        if not sel:
            return
        name = sel[0]
        self.sel_var.set(name)
        value = self.__class__._param_values.get(name)
        if value is None:
            info = self.param_desc.get(name)
            value = info[3] if info else 0.0
        info = self.param_desc.get(name)
        self.value_var.set(("%.*f" % (info[5], value)) if info else str(value))

    def _select_param_row(self, name: str) -> None:
        if not hasattr(self, "param_tree"):
            return
        if self.param_tree.exists(name):
            self.param_tree.selection_set(name)
            self.param_tree.see(name)
            self.sel_var.set(name)

    def _apply_param(self) -> None:
        name = self.sel_var.get().strip()
        if not name:
            self._log("先在列表里选一个参数", "err")
            return
        text = self.value_var.get().strip()
        try:
            float(text)
        except ValueError:
            self._log("新值必须是数字", "err")
            return
        self._send("PARAM", "SET", name, text)

    def _read_param(self) -> None:
        name = self.sel_var.get().strip()
        if name:
            self._send("PARAM", "GET", name)

    def _reset_params(self) -> None:
        from tkinter import messagebox
        if messagebox.askyesno("恢复默认", "把所有参数恢复为出厂默认值？\n（还需要点“保存到 NVS”才会掉电保持）"):
            self._send("PARAM", "RESET")

    # -- 执行器 ---------------------------------------------------------
    def _toggle_act(self, name: str) -> None:
        on = self.act_vars[name].get()
        self._send("ACT", name.upper(), "on" if on else "off")

    # ---- 滑条：拖动后不被遥测覆盖 -------------------------------------
    def _on_light_scale(self, value) -> None:
        if self._syncing_slider:
            return
        self._light_dirty = True
        self._light_dirty_ts = time.time()
        self.light_lbl.configure(text="%.0f%%" % float(value))
        tag = getattr(self, "light_dirty_tag", None)
        if tag is not None:
            tag.configure(text="● 未下发")

    def _on_servo_scale(self, value) -> None:
        if self._syncing_slider:
            return
        self._servo_dirty = True
        self._servo_dirty_ts = time.time()
        self.servo_lbl.configure(text="%.0f°" % float(value))
        tag = getattr(self, "servo_dirty_tag", None)
        if tag is not None:
            tag.configure(text="● 未下发")

    def _sync_light_slider(self, value: float) -> None:
        if self._light_dirty and (time.time() - self._light_dirty_ts) < self._dirty_timeout_s:
            return          # 用户正在调整，别抢
        self._light_dirty = False
        self._syncing_slider = True
        try:
            self.light_var.set(value)
            self.light_lbl.configure(text="%.0f%%" % value)
            self.light_dirty_tag.configure(text="")
        finally:
            self._syncing_slider = False

    def _sync_servo_slider(self, value: float) -> None:
        if self._servo_dirty and (time.time() - self._servo_dirty_ts) < self._dirty_timeout_s:
            return
        self._servo_dirty = False
        self._syncing_slider = True
        try:
            self.servo_var.set(value)
            self.servo_lbl.configure(text="%.0f°" % value)
            self.servo_dirty_tag.configure(text="")
        finally:
            self._syncing_slider = False

    def _apply_light(self) -> None:
        self._send("ACT", "LIGHT", "%.0f" % self.light_var.get())

    def _apply_servo(self) -> None:
        self._send("ACT", "FEEDER", "%.0f" % self.servo_var.get())

    def _back_to_auto(self) -> None:
        self._send("MODE", "AUTO")

    def _apply_stream(self) -> None:
        if not self._connected():
            return
        if self.stream_on.get():
            self._send("STREAM", "ON", self.period_var.get())
        else:
            self._send("STREAM", "OFF")

    # -- 工具 -----------------------------------------------------------
    def _i2c_read(self) -> None:
        self._send("I2C", "READ", self.i2c_addr.get(), self.i2c_reg.get(), self.i2c_len.get())

    def _i2c_write(self) -> None:
        self._send("I2C", "WRITE", self.i2c_addr.get(), self.i2c_reg.get(), self.i2c_data.get())

    def _rtc_set(self) -> None:
        text = self.rtc_var.get().strip()
        parts = text.replace("T", " ").split()
        if len(parts) != 2:
            self._log("时间格式应为 YYYY-MM-DD HH:MM:SS", "err")
            return
        self._send("RTC", "SET", parts[0], parts[1])

    def _rtc_set_now(self) -> None:
        now = time.strftime("%Y-%m-%d %H:%M:%S")
        self.rtc_var.set(now)
        parts = now.split()
        self._send("RTC", "SET", parts[0], parts[1])

    def _reboot(self) -> None:
        from tkinter import messagebox
        if messagebox.askyesno("重启", "确认重启设备？"):
            self._send("REBOOT")

    def _save_report(self) -> None:
        try:
            from tkinter import filedialog
            path = filedialog.asksaveasfilename(defaultextension=".txt",
                                                filetypes=[("文本", "*.txt")])
        except Exception:
            path = ""
        if not path:
            return
        lines = ["智能鱼缸上位机参数报告 %s" % time.strftime("%Y-%m-%d %H:%M:%S"), ""]
        for name, (grp, lo, hi, default, unit, dec, label) in self.param_desc.items():
            value = self.__class__._param_values.get(name)
            lines.append("%-22s %-14s %-10s (%.*f ~ %.*f) %s" %
                         (name, label, "" if value is None else "%.*f" % (dec, value),
                          dec, lo, dec, hi, unit))
        try:
            with open(path, "w", encoding="utf-8") as fh:
                fh.write("\n".join(lines))
            self._log("报告已保存: %s" % path, "info")
        except Exception as exc:
            self._log("保存失败: %s" % exc, "err")

    # -- 定时器 ---------------------------------------------------------
    def _pump_loop(self) -> None:
        if not self._alive:
            return
        try:
            self.client.pump()
        except Exception as exc:
            self._log("解析异常: %s" % exc, "err")
        try:
            lost = self.client.take_lost()
        except Exception:
            lost = ""
        if lost:
            self._handle_link_lost(lost)
        self.root.after(40, self._pump_loop)

    def _chart_loop(self) -> None:
        if not self._alive:
            return
        try:
            self.chart.redraw()
        except Exception:
            pass
        self.root.after(1000, self._chart_loop)


# ---------------------------------------------------------------------------
# 命令行模式
# ---------------------------------------------------------------------------
CLI_HELP = """
可用命令（直接发送给设备，或使用本地快捷指令）：
  sensors                 读取全部传感器
  act <name> <on|off|值>  点动执行器 (heater/pump/refill/co2/uv/light/feeder)
  feed                    立即喂食
  params                  列出全部参数
  get <name>              读取参数
  set <name> <value>      设置参数
  save                    保存参数到 NVS
  i2c                     扫描 I2C
  rtc                     读 RTC；rtc set 自动用当前电脑时间
  auto | manual [秒]      切换自动/手动模式
  watch <on|off>          遥测流开关
  raw <verb,args...>      原样发送一帧
  help                    本帮助
  quit                    退出
"""


def cli_main(port: str, baud: int, stop_on_eof: bool = False) -> int:
    frames: "queue.Queue[Frame]" = queue.Queue()
    state: List[str] = ["disconnected"]

    def on_state(kind: str, p: str) -> None:
        state[0] = kind
        print("[%s] %s" % (kind, p))

    def on_frame(fr: Frame) -> None:
        frames.put(fr)

    client = DeviceClient(on_frame, on_state)
    try:
        client.connect(port, baud)
    except Exception as exc:
        print("连接失败:", exc)
        return 2

    client.send("INFO")
    client.send("STREAM", "ON", "1000")

    def pump() -> None:
        client.pump()
        while True:
            try:
                fr = frames.get_nowait()
            except queue.Empty:
                break
            if fr.kind == "err":
                print("!! %s %s" % (fr.verb, ",".join(fr.fields[1:])))
            elif fr.kind == "log":
                if not fr.fields[0].startswith("#"):
                    print("   " + fr.fields[0])
            else:
                print("#%s %s" % (fr.verb, ",".join(fr.fields)))

    print(CLI_HELP)
    try:
        while True:
            try:
                line = input("fish> ").strip()
            except EOFError:
                break
            if not line:
                pump()
                continue
            low = line.lower()
            if low in ("quit", "exit", "q"):
                break
            if low == "help":
                print(CLI_HELP)
                continue
            try:
                if low == "sensors":
                    client.send("SENSORS")
                elif low.startswith("act "):
                    parts = line.split()
                    client.send("ACT", parts[1].upper(), parts[2] if len(parts) > 2 else "on")
                elif low == "feed":
                    client.send("ACT", "FEED")
                elif low == "params":
                    client.send("PARAM", "LIST")
                elif low.startswith("get "):
                    client.send("PARAM", "GET", line.split()[1])
                elif low.startswith("set "):
                    parts = line.split()
                    client.send("PARAM", "SET", parts[1], parts[2])
                elif low == "save":
                    client.send("PARAM", "SAVE")
                elif low == "i2c":
                    client.send("I2C", "SCAN")
                elif low.startswith("rtc set"):
                    now = time.strftime("%Y-%m-%d %H:%M:%S")
                    client.send("RTC", "SET", *now.split())
                elif low == "rtc":
                    client.send("RTC", "GET")
                elif low == "auto":
                    client.send("MODE", "AUTO")
                elif low.startswith("manual"):
                    parts = line.split()
                    client.send("MODE", "MANUAL", parts[1] if len(parts) > 1 else "")
                elif low.startswith("watch"):
                    parts = line.split()
                    client.send("STREAM", "ON" if len(parts) > 1 and parts[1] == "on" else "OFF")
                elif low.startswith("raw "):
                    client.send_raw(line[4:])
                else:
                    client.send_raw(line)
            except Exception as exc:
                print("命令失败:", exc)
            time.sleep(0.25)
            pump()
    finally:
        client.disconnect()
    return 0


def cmd_once(port: str, baud: int, command: str, wait: float = 1.5) -> int:
    frames: "queue.Queue[Frame]" = queue.Queue()
    client = DeviceClient(lambda f: frames.put(f), lambda k, p: None)
    try:
        client.connect(port, baud)
    except Exception as exc:
        print("连接失败:", exc)
        return 2

    time.sleep(0.3)
    client.pump()
    while not frames.empty():
        frames.get()

    if command.strip().lower() == "help":
        client.send("HELP")
    else:
        client.send_raw(command)

    deadline = time.time() + wait
    while time.time() < deadline:
        client.pump()
        time.sleep(0.05)
        while not frames.empty():
            fr = frames.get()
            if fr.kind == "err":
                print("!%s,%s" % (fr.verb, ",".join(fr.fields)))
            elif fr.kind == "resp":
                print("#%s,%s" % (fr.verb, ",".join(fr.fields)))
    client.disconnect()
    return 0


# ---------------------------------------------------------------------------
def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(description="智能鱼缸 上位机调试工具")
    parser.add_argument("--port", "-p", default="", help="串口号，例如 COM5")
    parser.add_argument("--baud", "-b", type=int, default=DEFAULT_BAUD, help="波特率，默认 115200")
    parser.add_argument("--sim", action="store_true", help="使用内置设备仿真器（无需硬件）")
    parser.add_argument("--cli", action="store_true", help="命令行交互模式")
    parser.add_argument("--cmd", default="", help="发送单条命令后退出（配合 --port）")
    parser.add_argument("--wait", type=float, default=1.5, help="--cmd 收集应答的时长(秒)")
    parser.add_argument("--list-ports", action="store_true", help="列出可用串口")
    parser.add_argument("--start-tab", type=int, default=0,
                        help="启动时显示的页签序号 0=监控 1=执行器 2=参数 3=工具")
    parser.add_argument("--selftest", action="store_true", help="运行工具自检（协议/仿真器）")
    args = parser.parse_args(argv)

    if args.list_ports:
        app_ports = []
        try:
            app_ports = DebuggerApp._list_ports(None)  # type: ignore[arg-type]
        except Exception:
            pass
        for p in app_ports:
            print(p)
        return 0

    if args.selftest:
        return run_selftest()

    port = SIM_PORT_NAME if args.sim else args.port

    if args.cmd:
        if not port:
            print("需要 --port COMx 或 --sim")
            return 2
        return cmd_once(port, args.baud, args.cmd, args.wait)

    if args.cli:
        if not port:
            print("需要 --port COMx 或 --sim")
            return 2
        return cli_main(port, args.baud)

    try:
        import tkinter as tk
    except Exception as exc:
        print("无法加载 tkinter:", exc)
        print("可以改用命令行模式： --cli --port COM5")
        return 3

    root = tk.Tk()
    _setup_chinese_font(root)
    DebuggerApp(root, port=port, sim=args.sim, start_tab=args.start_tab)
    root.mainloop()
    return 0


def run_selftest() -> int:
    """不依赖界面的自检：协议解析 + 仿真器往返。"""
    failures = 0
    checks = 0

    def check(cond: bool, what: str) -> None:
        nonlocal failures, checks
        checks += 1
        if not cond:
            failures += 1
            print("  [FAIL] %s" % what)

    print("== 协议解析自检 ==")
    fr = parse_frame("#PONG,uptime_ms=1234,mode=AUTO,proto=1")
    check(fr is not None and fr.verb == "PONG", "verb 解析")
    check(fr is not None and fr.i("uptime_ms") == 1234, "整数字段")
    check(fr is not None and fr.get("mode") == "AUTO", "字符串字段")

    fr = parse_frame("!UNKNOWN_CMD,unknown verb")
    check(fr is not None and fr.kind == "err" and fr.verb == "UNKNOWN_CMD", "错误帧")

    fr = parse_frame("I (1234) MAIN: system ready")
    check(fr is not None and fr.kind == "log", "日志行识别")

    check(parse_frame("#SENSOR,val=nan,id=ph").f("val") != parse_frame("#SENSOR,val=nan").f("val"),
          "NaN 处理")

    asm = LineAssembler()
    lines = asm.feed(b"#A,1\r\n#B,2\npartial")
    check(lines == ["#A,1", "#B,2"], "分包成行")
    lines = asm.feed(b" rest\n")
    check(lines == ["partial rest"], "跨包拼接")

    print("== 命令拼装自检 ==")
    check(build_command("PING") == "@PING\n", "无参数命令")
    # 企业网账号为空时，这个空字段一旦被吞掉，板子会把 ent 当成密码
    # （存成 "0,123456788"），WiFi 就永远连不上。必须原样保留。
    check(build_command("NET", "WIFI", "ADD", "407", "0", "", "123456788")
          == "@NET,WIFI,ADD,407,0,,123456788\n", "中间空字段保留")
    check(build_command("NET", "WIFI", "ADD", "407", "0", "", "")
          == "@NET,WIFI,ADD,407,0,,\n", "尾部空字段也保留（空密码=开放网络）")
    check(build_command("PARAM", "SET", "x", None) == "@PARAM,SET,x\n", "None 参数忽略")

    print("== 仿真器往返自检 ==")
    got: List[Frame] = []
    client = DeviceClient(lambda f: got.append(f), lambda k, p: None)
    client.connect(SIM_PORT_NAME)
    time.sleep(0.2)
    client.pump()
    got.clear()

    for verb, args, expect in (
            ("PING", (), "PONG"),
            ("SENSORS", (), "SENSOR"),
            ("PARAM", ("LIST",), "P"),
            ("ACT", ("LIST",), "ACT"),
            ("MODE", ("GET",), "MODE"),
            ("I2C", ("SCAN",), "I2C"),
            ("RTC", ("GET",), "RTC"),
            ("ADC", ("LIST",), "ADC"),
            ("SELFTEST", (), "CHK"),
    ):
        got.clear()
        client.send(verb, *args)
        deadline = time.time() + 2.0
        while time.time() < deadline:
            client.pump()
            if any(f.verb == expect for f in got):
                break
            time.sleep(0.02)
        check(any(f.verb == expect for f in got), "仿真器 %s -> %s" % (verb, expect))

    # 参数设置往返
    got.clear()
    client.send("PARAM", "SET", "temp_target_c", "27.5")
    client.send("PARAM", "GET", "temp_target_c")
    deadline = time.time() + 2.0
    p_val = None
    while time.time() < deadline:
        client.pump()
        for f in got:
            if f.verb == "P" and f.get("name") == "temp_target_c":
                p_val = f.f("val")
        if p_val is not None:
            break
        time.sleep(0.02)
    check(p_val is not None and abs(p_val - 27.5) < 1e-6, "参数读写往返 (%.2f)" % (p_val or -1))

    # 超限钳位
    got.clear()
    client.send("PARAM", "SET", "temp_target_c", "999")
    client.send("PARAM", "GET", "temp_target_c")
    deadline = time.time() + 2.0
    p_val = None
    while time.time() < deadline:
        client.pump()
        for f in got:
            if f.verb == "P" and f.get("name") == "temp_target_c":
                p_val = f.f("val")
        if p_val is not None:
            break
        time.sleep(0.02)
    check(p_val is not None and abs(p_val - 35.0) < 1e-6, "参数上限钳位 (%s)" % p_val)

    # 执行器点动进入手动模式
    got.clear()
    client.send("ACT", "HEATER", "on")
    deadline = time.time() + 2.0
    mode = None
    while time.time() < deadline:
        client.pump()
        for f in got:
            if f.verb == "OK" and f.get("act") == "heater":
                mode = f.get("mode")
        if mode is not None:
            break
        time.sleep(0.02)
    check(mode == "MANUAL", "手动模式进入 (%s)" % mode)

    # 遥测帧
    got.clear()
    client.send("STREAM", "ON", "100")
    deadline = time.time() + 2.0
    tel = None
    while time.time() < deadline:
        client.pump()
        for f in got:
            if f.verb == "TEL":
                tel = f
        if tel is not None:
            break
        time.sleep(0.02)
    check(tel is not None and tel.f("temp") == tel.f("temp"), "遥测帧")
    client.disconnect()

    print("== %d 项检查, %d 项失败 ==" % (checks, failures))
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
