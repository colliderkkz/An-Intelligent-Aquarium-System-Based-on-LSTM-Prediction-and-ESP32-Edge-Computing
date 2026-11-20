#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
智能鱼缸过滤系统 —— 一体化启动器
====================================

打包成一个 exe 后，双击会先出现选择页：

    ┌──────────────────────────────────────────┐
    │  智能鱼缸过滤系统  v1.1.0                 │
    │  [ 上位机调试台 ]      [ 烧录固件 ]        │
    └──────────────────────────────────────────┘

* 「上位机调试台」→ 打开实时监控/执行器测试/参数设置/总线工具，自动扫描串口
* 「烧录固件」    → 内置固件（bootloader + 分区表 + app），选串口一键烧录，带进度条
* 两个界面互相切换（关闭窗口 = 回首页，首页再关闭 = 退出）

开发模式下也能直接跑：  python tools/launcher.py
"""

from __future__ import annotations

import contextlib
import io
import os
import queue
import re
import sys
import tempfile
import threading
import tkinter as tk
from tkinter import messagebox, ttk

import fish_tank_debugger as ft

APP_NAME = "智能鱼缸过滤系统"
APP_VER = ft.APP_VERSION
WIN_TITLE = "%s 工具 v%s" % (APP_NAME, APP_VER)

# (烧录地址, 相对固件目录的路径, 中文名)
FIRMWARE_FILES = [
    ("0x0", "bootloader/bootloader.bin", "bootloader"),
    ("0x8000", "partition_table/partition-table.bin", "分区表"),
    ("0x10000", "fish_gundam_panzer.bin", "应用程序"),
]

FLASH_MODE = "dio"
FLASH_FREQ = "80m"
FLASH_SIZE = "16MB"
CHIP = "esp32s3"

# NVS 分区（参数表 / WiFi 账密 / MQTT 账号 / 定时任务全在里面）
NVS_OFFSET = "0x9000"
NVS_SIZE = "0x6000"
NVS_BACKUP_NAME = "fishtank_nvs_backup.bin"

COLOR_BG = "#f4f6f8"
COLOR_PRIMARY = "#1a5276"
COLOR_ACCENT = "#2ecc71"


def resource_dir() -> str:
    """打包后资源在 sys._MEIPASS/firmware；开发模式用工程 build/ 目录。"""
    base = getattr(sys, "_MEIPASS", None)
    if base:
        return os.path.join(base, "firmware")
    return os.path.normpath(
        os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "build"))


def human_size(n: int) -> str:
    if n < 1024:
        return "%d B" % n
    if n < 1024 * 1024:
        return "%.1f KB" % (n / 1024.0)
    return "%.2f MB" % (n / (1024.0 * 1024.0))


def _esptool_main(args, q=None) -> None:
    """跑一次 esptool（输出转发到烧录日志）。出错一律抛异常，由调用方判断。"""

    class Sink(io.TextIOBase):
        def write(self, s):
            if s and q is not None:
                q.put(("log", s))
            return len(s)

        def flush(self):
            pass

    import esptool

    with contextlib.redirect_stdout(Sink()), contextlib.redirect_stderr(Sink()):
        esptool.main(args)


def _nvs_backup(port: str, baud: int, q=None):
    """把 NVS 分区备份到临时文件，成功返回路径，失败返回 None（不阻断烧录）。"""
    path = os.path.join(tempfile.gettempdir(), NVS_BACKUP_NAME)
    try:
        _esptool_main(["--chip", CHIP, "--port", port, "--baud", str(baud),
                       "read-flash", NVS_OFFSET, NVS_SIZE, path], q)
    except SystemExit as exc:                  # esptool 用 SystemExit 表示退出码
        code = 0 if exc.code is None else (exc.code if isinstance(exc.code, int) else 1)
        if code != 0:
            if q is not None:
                q.put(("log", "   备份失败（退出码 %d）\n" % code))
            return False
    except Exception as exc:                   # noqa: BLE001
        if q is not None:
            q.put(("log", "   备份失败：%s\n" % exc))
        return False

    try:
        with open(path, "rb") as f:
            data = f.read()
    except OSError as exc:
        if q is not None:
            q.put(("log", "   备份文件读取失败：%s\n" % exc))
        return False

    if len(data) != int(NVS_SIZE, 16):
        if q is not None:
            q.put(("log", "   备份大小不对（%d 字节），丢弃\n" % len(data)))
        return False
    if data == b"\xff" * len(data):
        if q is not None:
            q.put(("log", "   NVS 是空白的，没有可保留的设置\n"))
        return None
    if q is not None:
        q.put(("log", "   NVS 已备份（%d 字节）\n" % len(data)))
    return path


class Launcher:
    def __init__(self, root: tk.Tk) -> None:
        self.root = root
        self.fw_dir = resource_dir()
        self.root.title(WIN_TITLE)
        self.root.configure(bg=COLOR_BG)
        self.root.minsize(820, 560)

        self._flash_q: "queue.Queue" = queue.Queue()
        self._flashing = False
        self._debugger = None

        self.show_home()

    # ------------------------------------------------------------------
    # 通用
    # ------------------------------------------------------------------
    def _clear(self) -> None:
        if self._debugger is not None:
            try:
                self._debugger._alive = False
                self._debugger.client.disconnect()
            except Exception:
                pass
            self._debugger = None
        for w in list(self.root.winfo_children()):
            w.destroy()

    def _quit(self) -> None:
        """首页的窗口关闭按钮：真正退出整个程序。"""
        try:
            self._clear()
        except Exception:
            pass
        self.root.destroy()

    def _firmware_ok(self) -> bool:
        for _, rel, _ in FIRMWARE_FILES:
            if not os.path.isfile(os.path.join(self.fw_dir, rel)):
                return False
        return True

    # ------------------------------------------------------------------
    # 首页
    # ------------------------------------------------------------------
    def show_home(self) -> None:
        self._clear()
        # 从调试台/烧录页返回时要重新接管关闭按钮：DebuggerApp 会把
        # WM_DELETE_WINDOW 指向它自己的 _on_close，不重设的话点右上角 X
        # 只是又回到首页，看起来像"关不掉"。
        self.root.protocol("WM_DELETE_WINDOW", self._quit)
        self.root.geometry("900x680")

        outer = tk.Frame(self.root, bg=COLOR_BG)
        outer.pack(fill="both", expand=True, padx=40, pady=24)

        tk.Label(outer, text=APP_NAME, font=("Microsoft YaHei UI", 24, "bold"),
                 bg=COLOR_BG, fg=COLOR_PRIMARY).pack(anchor="w")
        tk.Label(outer, text="ESP32-S3 智能鱼缸过滤与生态运维系统   v%s" % APP_VER,
                 font=("Microsoft YaHei UI", 10), bg=COLOR_BG, fg="#666666").pack(anchor="w", pady=(4, 0))
        ttk.Separator(outer).pack(fill="x", pady=16)

        # 底部信息行先 pack(side="bottom") 固定住，避免被中间的可伸缩卡片挤出窗口
        info = tk.Frame(outer, bg=COLOR_BG)
        info.pack(side="bottom", fill="x")
        fw_ok = self._firmware_ok()
        tk.Label(info, text=("固件包：已内置  " if fw_ok else "固件包：缺失（请重新打包）"),
                 font=("Microsoft YaHei UI", 9), bg=COLOR_BG,
                 fg="#1e8449" if fw_ok else "#c0392b").pack(side="left")
        tk.Label(info, text="    目标芯片 ESP32-S3 (N16R8 / 16MB Flash)",
                 font=("Microsoft YaHei UI", 9), bg=COLOR_BG, fg="#666666").pack(side="left")
        tk.Label(info, text="串口 115200 · UART 口", font=("Microsoft YaHei UI", 9),
                 bg=COLOR_BG, fg="#666666").pack(side="right")

        cards = tk.Frame(outer, bg=COLOR_BG)
        cards.pack(fill="both", expand=True, pady=(0, 12))

        self._card(
            cards, 0, "🖥", "上位机调试台",
            "实时监控 7 路传感器\n点动测试 6 路执行器\n56 个参数在线读写\nI2C / RTC / GPIO 工具",
            "打开调试台", self.open_debugger, COLOR_PRIMARY)
        self._card(
            cards, 1, "⚡", "烧录固件",
            "内置固件（%s）\nbootloader + 分区表 + 应用\n一键按地址写入\n自动校验" %
            (self._firmware_version_text()),
            "开始烧录", self.open_flasher, "#c0392b")
        cards.columnconfigure(0, weight=1)
        cards.columnconfigure(1, weight=1)

    def _firmware_version_text(self) -> str:
        p = os.path.join(self.fw_dir, "fish_gundam_panzer.bin")
        if os.path.isfile(p):
            return human_size(os.path.getsize(p))
        return "未打包"

    def _card(self, parent, col, icon, title, desc, btn_text, cmd, color):
        frame = tk.Frame(parent, bg="#ffffff", highlightbackground="#dde3e8",
                         highlightthickness=1)
        frame.grid(row=0, column=col, sticky="nsew", padx=8, pady=4)
        frame.columnconfigure(0, weight=1)

        tk.Label(frame, text=icon, font=("Segoe UI Emoji", 30), bg="#ffffff",
                 fg=color).pack(pady=(22, 4))
        tk.Label(frame, text=title, font=("Microsoft YaHei UI", 15, "bold"),
                 bg="#ffffff", fg=color).pack()
        tk.Label(frame, text=desc, font=("Microsoft YaHei UI", 9), bg="#ffffff",
                 fg="#555555", justify="center").pack(pady=(10, 16))
        btn = tk.Button(frame, text=btn_text, font=("Microsoft YaHei UI", 10, "bold"),
                        bg=color, fg="#ffffff", activebackground=color,
                        activeforeground="#ffffff", relief="flat", cursor="hand2",
                        command=cmd, height=2)
        btn.pack(fill="x", padx=26, pady=(0, 22))

    # ------------------------------------------------------------------
    # 上位机
    # ------------------------------------------------------------------
    def _pick_serial(self) -> str:
        """优先挑 USB 转串口设备（CH340/CP210x 等），跳过蓝牙虚拟串口。"""
        try:
            from serial.tools import list_ports
            usb = [p.device for p in list_ports.comports()
                   if getattr(p, "vid", None) is not None
                   or "USB" in (getattr(p, "hwid", "") or "").upper()]
            if len(usb) == 1:
                return usb[0]
        except Exception:
            pass
        return ""

    def open_debugger(self, port: str = "") -> None:
        self._clear()
        if not port:
            port = self._pick_serial()
        self._debugger = ft.DebuggerApp(self.root, port=port, on_home=self.show_home)

    # ------------------------------------------------------------------
    # 烧录
    # ------------------------------------------------------------------
    def open_flasher(self) -> None:
        self._clear()
        self.root.protocol("WM_DELETE_WINDOW", self._quit)
        self.root.geometry("900x780")

        outer = tk.Frame(self.root, bg=COLOR_BG)
        outer.pack(fill="both", expand=True, padx=24, pady=16)

        head = tk.Frame(outer, bg=COLOR_BG)
        head.pack(fill="x")
        tk.Button(head, text="← 返回", font=("Microsoft YaHei UI", 9), relief="flat",
                  bg=COLOR_BG, fg=COLOR_PRIMARY, cursor="hand2",
                  command=self.show_home).pack(side="left")
        tk.Label(head, text="烧录固件到 ESP32-S3", font=("Microsoft YaHei UI", 15, "bold"),
                 bg=COLOR_BG, fg=COLOR_PRIMARY).pack(side="left", padx=14)

        body = tk.Frame(outer, bg=COLOR_BG)
        body.pack(fill="x", pady=12)

        row1 = tk.Frame(body, bg=COLOR_BG)
        row1.pack(fill="x", pady=4)
        tk.Label(row1, text="串口", bg=COLOR_BG, font=("Microsoft YaHei UI", 9)).pack(side="left")
        self.port_var = tk.StringVar()
        ports = self._list_ports()
        self.port_combo = ttk.Combobox(row1, textvariable=self.port_var, width=26,
                                       values=ports)
        self.port_combo.pack(side="left", padx=6)
        if ports:
            self.port_var.set(self._pick_serial() or ports[0])
        tk.Button(row1, text="刷新", relief="flat", bg="#dfe6ec", cursor="hand2",
                  command=self._refresh_ports).pack(side="left")

        tk.Label(row1, text="  波特率", bg=COLOR_BG,
                 font=("Microsoft YaHei UI", 9)).pack(side="left")
        self.baud_var = tk.StringVar(value="460800")
        ttk.Combobox(row1, textvariable=self.baud_var, width=9,
                     values=("115200", "230400", "460800", "921600")).pack(side="left", padx=6)

        self.erase_var = tk.BooleanVar(value=False)
        ttk.Checkbutton(row1, text="先整片擦除（会自动备份并恢复 WiFi/参数/定时）",
                        variable=self.erase_var).pack(side="left", padx=12)

        row2 = tk.Frame(body, bg=COLOR_BG)
        row2.pack(fill="x", pady=(10, 0))
        self.flash_btn = tk.Button(row2, text="开始烧录", font=("Microsoft YaHei UI", 11, "bold"),
                                   bg="#c0392b", fg="#ffffff", activebackground="#c0392b",
                                   activeforeground="#ffffff", relief="flat",
                                   cursor="hand2", height=1, width=16,
                                   command=self.start_flash)
        self.flash_btn.pack(side="left")
        self.debug_btn = tk.Button(row2, text="烧录完成后打开调试台", relief="flat",
                                   bg="#dfe6ec", cursor="hand2",
                                   command=lambda: self.open_debugger(self.port_var.get()))
        self.debug_btn.pack(side="left", padx=12)

        self.progress = ttk.Progressbar(body, orient="horizontal", mode="determinate",
                                        maximum=100, length=100)
        self.progress.pack(fill="x", pady=(14, 4))
        self.status_var = tk.StringVar(value="就绪")
        tk.Label(body, textvariable=self.status_var, bg=COLOR_BG, fg="#555555",
                 font=("Microsoft YaHei UI", 9), anchor="w").pack(fill="x")

        # 固件清单放在日志之前 pack，保证一定可见（日志用 expand 吃掉剩余空间）
        lst = ttk.LabelFrame(outer, text="内置固件")
        lst.pack(fill="x", pady=(10, 0))
        for addr, rel, name in FIRMWARE_FILES:
            p = os.path.join(self.fw_dir, rel)
            ok = os.path.isfile(p)
            size = human_size(os.path.getsize(p)) if ok else "缺失"
            tk.Label(lst, text="%s   %-12s %-10s %s" % (addr, name, size, rel),
                     font=("Consolas", 9), bg=COLOR_BG,
                     fg="#1e8449" if ok else "#c0392b", anchor="w").pack(fill="x", padx=8, pady=1)

        logframe = ttk.LabelFrame(outer, text="烧录日志")
        logframe.pack(fill="both", expand=True, pady=(10, 0))
        self.log = tk.Text(logframe, height=10, wrap="none", font=("Consolas", 9),
                           background="#101418", foreground="#d0d0d0")
        ys = ttk.Scrollbar(logframe, orient="vertical", command=self.log.yview)
        self.log.configure(yscrollcommand=ys.set)
        self.log.pack(side="left", fill="both", expand=True)
        ys.pack(side="right", fill="y")

        # 如果上一次烧录还在后台跑（中途返回过首页），这里把日志轮询重新接上
        if self._flashing:
            self.flash_btn.configure(state="disabled", text="烧录中...")
            self.root.after(120, self._poll_flash)

    def _list_ports(self):
        # 合并 pyserial 与注册表两个来源，避免某个环境下列表为空
        ports = []
        try:
            from serial.tools import list_ports
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
        return sorted(set(ports))

    def _refresh_ports(self) -> None:
        self.port_combo.configure(values=self._list_ports())

    def _append_log(self, text: str) -> None:
        if not text:
            return
        text = text.replace("\r\n", "\n").replace("\r", "\n")
        m = re.findall(r"\((\d+)\s*%\)", text)
        try:
            if m:
                self.progress["value"] = int(m[-1])
            self.log.insert("end", text if text.endswith("\n") else text + "\n")
            self.log.see("end")
        except tk.TclError:
            pass    # 页面已经切走，控件没了：日志丢弃，别让轮询链断掉

    def start_flash(self) -> None:
        if self._flashing:
            return
        if not self._firmware_ok():
            messagebox.showerror("固件缺失", "内置固件不完整，请重新打包 exe。")
            return
        port = self.port_var.get().strip()
        if not port:
            ports = self._list_ports()
            if len(ports) == 1:
                port = ports[0]
                self.port_var.set(port)
            else:
                messagebox.showwarning("选择串口", "请先选择设备所在的串口（设备管理器里的 COMx）。")
                return
        try:
            baud = int(self.baud_var.get())
        except ValueError:
            baud = 460800

        self.log.delete("1.0", "end")
        self.progress["value"] = 0
        self._flashing = True
        self.flash_btn.configure(state="disabled", text="烧录中...")
        self.status_var.set("正在连接设备...")

        t = threading.Thread(target=self._flash_worker, args=(port, baud, self.erase_var.get()),
                             daemon=True)
        t.start()
        self.root.after(80, self._poll_flash)

    def _flash_worker(self, port: str, baud: int, erase: bool) -> None:
        q = self._flash_q
        nvs_backup = None
        if erase:
            # 整片擦除会把 NVS（WiFi 账密 / MQTT 账号密码 / 全部参数 / 定时任务）
            # 一起抹掉 —— 用户重刷后"WiFi 和参数全丢"就是这么来的。
            # 擦之前先把 NVS 那 24KB 备份出来，烧完再写回去。
            q.put(("log", "\n=== 整片擦除模式：先备份 NVS（WiFi/参数/定时）===\n"))
            nvs_backup = _nvs_backup(port, baud, q)
            if nvs_backup is False:
                # 读不出 NVS 就不能擦：擦掉之后 WiFi 账密、MQTT 账号、58 个参数、
                # 定时计划全部丢，而且从外面根本连不回去（远程用户最惨）。
                q.put(("log", "\n[!] 没能读出 NVS（设备连不上 / 串口被占用）。\n"
                              "    已取消本次整片擦除，避免 WiFi/参数/定时被抹掉。\n"
                              "    普通烧录本来就不会动 NVS —— 直接取消勾选再点烧录即可。\n"))
                q.put(("done", 1, "NVS 备份失败，已取消整片擦除（设置不会被丢）"))
                return

        args = ["--chip", CHIP, "--port", port, "--baud", str(baud),
                "write-flash", "--flash-mode", FLASH_MODE,
                "--flash-freq", FLASH_FREQ, "--flash-size", FLASH_SIZE]
        if erase:
            args.append("--erase-all")
        for addr, rel, _ in FIRMWARE_FILES:
            args += [addr, os.path.join(self.fw_dir, rel)]

        try:
            _esptool_main(args, q)
            if erase and nvs_backup:
                q.put(("log", "\n=== 回写 NVS：恢复 WiFi/参数/定时 ===\n"))
                try:
                    _esptool_main(["--chip", CHIP, "--port", port, "--baud", str(baud),
                                   "write-flash", NVS_OFFSET, nvs_backup], q)
                except BaseException as exc:    # noqa: BLE001  SystemExit 也是 BaseException
                    # 固件本身已经烧好了：NVS 回写失败只能算"设置丢了"，
                    # 不能把整次烧录报成失败（用户会以为要重来一遍）。
                    q.put(("log", "\n[!] NVS 回写失败：%s\n"
                                  "    固件已烧录成功，但 WiFi/参数/定时需要重新录入。\n" % exc))
            q.put(("done", 0, None))
        except SystemExit as exc:
            code = exc.code if isinstance(exc.code, int) else (0 if exc.code is None else 1)
            q.put(("done", code, None))
        except Exception as exc:               # noqa: BLE001
            q.put(("done", 1, "%s: %s" % (type(exc).__name__, exc)))

    def _widgets_alive(self) -> bool:
        try:
            return bool(self.log is not None and self.log.winfo_exists())
        except Exception:
            return False

    def _poll_flash(self) -> None:
        # 烧录过程中用户点了"返回首页"：控件已被销毁，继续 insert 会抛
        # TclError，把 after 轮询链打断，_flashing 永远停在 True ——
        # 之后再点"开始烧录"就完全没反应（要么等到重启程序）。
        # 这里只停轮询，不清 _flashing；回到烧录页会重新接上。
        if not self._widgets_alive():
            return
        try:
            while True:
                item = self._flash_q.get_nowait()
                if item[0] == "log":
                    self._append_log(item[1])
                elif item[0] == "done":
                    self._finish_flash(item[1], item[2])
                    return
        except queue.Empty:
            pass
        except Exception:
            pass
        if self._flashing and self._widgets_alive():
            self.root.after(80, self._poll_flash)

    def _finish_flash(self, code: int, err) -> None:
        self._flashing = False
        try:
            self.flash_btn.configure(state="normal", text="开始烧录")
        except tk.TclError:
            pass
        if code == 0:
            self.progress["value"] = 100
            self.status_var.set("✔ 烧录完成（设备已自动复位）。点右侧可直接打开调试台。")
            self._append_log("\n=== 烧录成功 ===\n设备已复位，应能看到 #READY,fw=fish_gundam_panzer,ver=%s\n"
                             "打开调试台即可连接。\n" % APP_VER)
        else:
            self.status_var.set("✘ 烧录失败（退出码 %s）" % code)
            if err:
                self._append_log("\n=== 失败：%s ===\n" % err)
            self._append_log(
                "\n排查建议：\n"
                "  1. 串口是否选对（设备管理器 → 端口）\n"
                "  2. USB 线是不是只能充电的线\n"
                "  3. 插的是核心板的 UART 口而不是 USB 口\n"
                "  4. 波特率降到 115200 再试\n"
                "  5. 按住 BOOT → 点一下 RST → 松开 BOOT，进入下载模式后重试\n")
            messagebox.showerror("烧录失败", "详见日志区的排查建议。")


def main() -> int:
    # 高分屏下让 tk 界面清晰
    try:
        import ctypes
        ctypes.windll.shcore.SetProcessDpiAwareness(1)
    except Exception:
        pass

    root = tk.Tk()
    ft._setup_chinese_font(root)
    Launcher(root)
    root.mainloop()
    return 0


if __name__ == "__main__":
    sys.exit(main())
