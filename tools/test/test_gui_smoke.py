#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
上位机 GUI 冒烟测试（不需要真实硬件，也不弹窗）
================================================
用 tkinter 构建完整界面 -> 连接内置仿真器 -> 驱动事件循环若干秒，
验证：界面能构建、仿真器能连上、传感器卡片/参数表/执行器状态都能被刷新。

用法：python tools\\test\\test_gui_smoke.py
"""
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))          # tools/

import fish_tank_debugger as ft  # noqa: E402


def pump(root, seconds: float) -> None:
    end = time.time() + seconds
    while time.time() < end:
        root.update()
        time.sleep(0.02)


def main() -> int:
    try:
        import tkinter as tk
    except Exception as exc:
        print("tkinter 不可用，跳过 GUI 测试:", exc)
        return 0

    failures = 0
    checks = 0

    def check(cond, what):
        nonlocal failures, checks
        checks += 1
        if not cond:
            failures += 1
            print("  [FAIL]", what)

    root = tk.Tk()
    root.withdraw()                       # 不弹窗
    ft._setup_chinese_font(root)
    app = ft.DebuggerApp(root, sim=True)

    # 1) 界面构建
    check(len(app.nb.tabs()) == 7, "七个功能页签（含定时计划/SD/WiFi）")
    check(hasattr(app, "wifi_tree") and hasattr(app, "wifi_scan_box"), "WiFi 面板控件就绪")
    check(len(app.sensor_vars) == 7, "7 个传感器卡片")
    for sid in ft.SENSOR_ORDER:
        check(sid in app.sensor_vars, "卡片 %s" % sid)

    # 2) 连接仿真器
    pump(root, 2.5)
    check(app.client.connected, "已连接仿真器")
    check(app.mode_var.get() != "未连接", "模式标签已更新: %s" % app.mode_var.get())

    # 3) 遥测刷新传感器
    pump(root, 1.5)
    values = {sid: app.sensor_vars[sid].get() for sid in ft.SENSOR_ORDER}
    check(all(v != "--" for v in values.values()), "传感器都有值: %s" % values)
    check(app.light_lbl.cget("text") != "", "灯光亮度标签")

    # 4) 参数表
    pump(root, 1.5)
    rows = app.param_tree.get_children()
    check(len(rows) >= 50, "参数表行数 = %d" % len(rows))
    app.param_tree.selection_set("temp_target_c")
    app._on_param_select()
    check(app.sel_var.get() == "temp_target_c", "选中参数")
    app.value_var.set("27.5")
    app._apply_param()
    pump(root, 0.8)
    check(abs(app.__class__._param_values.get("temp_target_c", 0) - 27.5) < 1e-6,
          "参数写入生效: %s" % app.__class__._param_values.get("temp_target_c"))

    # 5) 执行器点动（含新增的气泵）
    for name in ("heater", "air"):
        app.act_vars[name].set(True)
        app._toggle_act(name)
        pump(root, 0.5)
    check("MANUAL" in app.mode_var.get(), "点动后进入手动模式: %s" % app.mode_var.get())

    # 5b) 滑条拖动后不能被遥测立刻覆盖（BUG：拖动后自动归零）
    #     真实拖动时 ttk.Scale 先更新 variable 再触发 command，这里照同样顺序模拟
    app.light_var.set(60)
    app._on_light_scale(60)
    app.servo_var.set(90)
    app._on_servo_scale(90)
    pump(root, 2.0)          # 期间会收到好几帧遥测
    check(abs(app.light_var.get() - 60) < 0.6, "亮度滑条保持 60: %.1f" % app.light_var.get())
    check(abs(app.servo_var.get() - 90) < 0.6, "舵机滑条保持 90: %.1f" % app.servo_var.get())
    app._apply_light()
    app._apply_servo()
    pump(root, 1.0)
    check(not app._light_dirty, "应用后解除亮度未下发状态")
    check(not app._servo_dirty, "应用后解除舵机未下发状态")
    check(abs(app.light_var.get() - 60) < 0.6, "应用后滑条仍为 60: %.1f" % app.light_var.get())

    app._back_to_auto()
    pump(root, 0.8)
    check("AUTO" in app.mode_var.get(), "返回自动模式: %s" % app.mode_var.get())

    # 6) 自检输出
    app._send("SELFTEST")
    pump(root, 1.2)
    text = app.test_box.get("1.0", "end")
    check("自检完成" in text or "OK" in text, "自检结果输出")

    app._on_close()
    print("== GUI 冒烟测试: %d 项检查, %d 项失败 ==" % (checks, failures))
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
