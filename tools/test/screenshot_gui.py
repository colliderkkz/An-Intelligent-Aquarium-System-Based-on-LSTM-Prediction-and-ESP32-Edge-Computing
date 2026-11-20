#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
把上位机界面截图保存为 PNG（仅 Windows，用于文档/自检）
========================================================
会以 --sim 模式启动 GUI，置前窗口后截取窗口区域。
用法：python tools\\test\\screenshot_gui.py [输出png]
"""
import ctypes
import ctypes.wintypes as wt
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.dirname(HERE)
SCRIPT = os.path.join(TOOLS, "fish_tank_debugger.py")
TITLE = "智能鱼缸过滤系统"


def find_window(title_part: str):
    user32 = ctypes.WinDLL("user32", use_last_error=True)
    found = []

    @ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
    def cb(hwnd, _lparam):
        length = user32.GetWindowTextLengthW(hwnd)
        if length > 0:
            buf = ctypes.create_unicode_buffer(length + 1)
            user32.GetWindowTextW(hwnd, buf, length + 1)
            if title_part in buf.value and user32.IsWindowVisible(hwnd):
                found.append(hwnd)
        return True

    user32.EnumWindows(cb, 0)
    return found[0] if found else None


def main() -> int:
    args = [a for a in sys.argv[1:]]
    tab = "0"
    if "--tab" in args:
        i = args.index("--tab")
        tab = args[i + 1]
        del args[i:i + 2]
    out = args[0] if args else os.path.join(TOOLS, "gui_screenshot.png")

    # 高分屏下必须声明 DPI 感知，否则 GetWindowRect 返回的是虚拟坐标，
    # 和 ImageGrab 抓到的物理像素对不上，截出来只有窗口左上角一块。
    try:
        ctypes.windll.shcore.SetProcessDpiAwareness(2)
    except Exception:
        try:
            ctypes.windll.user32.SetProcessDPIAware()
        except Exception:
            pass

    proc = subprocess.Popen([sys.executable, SCRIPT, "--sim", "--start-tab", tab],
                            cwd=os.path.dirname(TOOLS))
    try:
        hwnd = None
        for _ in range(60):
            time.sleep(0.25)
            hwnd = find_window(TITLE)
            if hwnd:
                break
        if not hwnd:
            print("未找到窗口")
            return 1

        user32 = ctypes.WinDLL("user32", use_last_error=True)
        user32.ShowWindow(hwnd, 9)            # SW_RESTORE
        user32.SetForegroundWindow(hwnd)
        time.sleep(2.5)                       # 等遥测刷新几帧

        rect = wt.RECT()
        user32.GetWindowRect(hwnd, ctypes.byref(rect))
        bbox = (rect.left, rect.top, rect.right, rect.bottom)

        from PIL import ImageGrab
        img = ImageGrab.grab(bbox=bbox, all_screens=True)
        img.save(out)
        print("saved:", out, img.size)
        return 0
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=3)
        except Exception:
            proc.kill()


if __name__ == "__main__":
    sys.exit(main())
