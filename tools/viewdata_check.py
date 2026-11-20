#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Rokid CUSTOM_VIEW 视图 / 增量更新 / 遥测映射 的离线校验工具

用途：在没有眼镜真机的情况下，先把「下发给眼镜的 JSON」验到不会因为格式问题白跑一趟。
依据：Rokid CXR 文档「眼镜端自定义 View」一节的 JSON Schema（type/props/children，
      颜色 #AARRGGBB，textSize 建议不小于 16，ImageView 用 name 引用预置图标）。

用法：
    python tools/viewdata_check.py                      # 校验默认模板
    python tools/viewdata_check.py 某个 viewdata.json
    python tools/viewdata_check.py --updates up.json    # 校验增量更新数组
    python tools/viewdata_check.py --state state.json   # 遥测 JSON -> 增量更新数组
    python tools/viewdata_check.py --state s.json --prev p.json   # 只输出变化的字段
    python tools/viewdata_check.py --self-test          # 内置正反例自测

约定：
    * 退出码 0 = 通过；1 = 校验失败；2 = 用法/文件错误。
    * --state 的输入字段与固件 main/net/mqtt_app.c 的 publish_state() 一致。
"""

import argparse
import json
import os
import re
import sys

# ---------------------------------------------------------------- 常量定义

CONTAINERS = ("LinearLayout", "RelativeLayout")
LEAVES = ("TextView", "ImageView")
COMPONENTS = CONTAINERS + LEAVES

COMMON_OPTIONAL = {
    "backgroundColor", "padding", "paddingTop", "paddingBottom",
    "marginBottom", "marginStart",
}

# 每个组件允许出现的 props 键
PROPS = {
    "LinearLayout": {"id", "layout_width", "layout_height", "orientation", "gravity"} | COMMON_OPTIONAL,
    "RelativeLayout": {"id", "layout_width", "layout_height", "layout_alignParentStart",
                       "layout_centerVertical", "layout_toEndOf"} | COMMON_OPTIONAL,
    "TextView": {"id", "layout_width", "layout_height", "text", "textSize", "textColor",
                 "textStyle", "gravity"} | COMMON_OPTIONAL,
    "ImageView": {"id", "name", "width", "height", "layout_width", "layout_height",
                  "marginBottom", "marginStart"},
}

# 每个组件必填的 props 键
REQUIRED = {
    "LinearLayout": {"id", "layout_width", "layout_height"},
    "RelativeLayout": {"id", "layout_width", "layout_height"},
    "TextView": {"id", "layout_width", "layout_height", "text"},
    "ImageView": {"id", "name", "width", "height"},
}

DIM_RE = re.compile(r"^(match_parent|wrap_content|\d+(\.\d+)?(dp|dip|px|sp)?)$")
COLOR_RE = re.compile(r"^#[0-9A-Fa-f]{8}$")
SIZE_RE = re.compile(r"^(\d+(\.\d+)?)(sp|dp|dip|px)?$")
TEXT_STYLES = {"normal", "bold", "italic", "bold_italic"}
ORIENTATIONS = {"vertical", "horizontal"}
GRAVITY_HINTS = (
    "center", "center_horizontal", "center_vertical", "start", "end",
    "left", "right", "top", "bottom", "fill",
)

MIN_TEXT_SIZE = 16

DEFAULT_VIEWDATA = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
    "tools", "glasses_test", "assets", "viewdata.json",
)


# ---------------------------------------------------------------- 基础校验

def _is_number(v):
    return isinstance(v, (int, float)) and not isinstance(v, bool)


def check_color(path, value, errors):
    if not isinstance(value, str) or not COLOR_RE.match(value):
        errors.append("%s: 颜色必须是 #AARRGGBB（如 #FF00FF00），实际 %r" % (path, value))


def check_dim(path, value, errors):
    if _is_number(value):
        if value < 0:
            errors.append("%s: 尺寸不能为负数（%r）" % (path, value))
        return
    if not isinstance(value, str) or not DIM_RE.match(value.strip()):
        errors.append("%s: 尺寸应为 match_parent / wrap_content / 数字，实际 %r" % (path, value))


def check_text_size(path, value, errors):
    num = None
    if _is_number(value):
        num = float(value)
    elif isinstance(value, str):
        m = SIZE_RE.match(value.strip())
        if not m:
            errors.append("%s: textSize 格式应为 20sp / 20px / 20，实际 %r" % (path, value))
            return
        num = float(m.group(1))
    else:
        errors.append("%s: textSize 类型不合法（%r）" % (path, value))
        return
    if num < MIN_TEXT_SIZE:
        errors.append("%s: textSize=%s 偏小，文档建议不小于 %d（眼镜上难辨认）"
                      % (path, value, MIN_TEXT_SIZE))


def check_gravity(path, value, errors):
    if not isinstance(value, str):
        errors.append("%s: gravity 应为字符串，实际 %r" % (path, value))
        return
    for part in re.split(r"[|,]", value):
        part = part.strip()
        if part and part not in GRAVITY_HINTS:
            errors.append("%s: gravity 含未在文档中列出的取值 %r（请以真机验证为准）" % (path, part))


def check_props(node_type, path, props, ids, errors, warnings, icons):
    if not isinstance(props, dict):
        errors.append("%s: props 必须是对象" % path)
        return
    allowed = PROPS[node_type]
    required = REQUIRED[node_type]

    unknown = sorted(set(props) - allowed)
    if unknown:
        errors.append("%s: %s 不支持 props 键 %s（文档只保证清单内的键）"
                      % (path, node_type, unknown))

    missing = sorted(required - set(props))
    if missing:
        errors.append("%s: %s 缺少必填 props %s" % (path, node_type, missing))

    if "layout_width" in props:
        check_dim(path + ".layout_width", props["layout_width"], errors)
    if "layout_height" in props:
        check_dim(path + ".layout_height", props["layout_height"], errors)
    if "backgroundColor" in props:
        check_color(path + ".backgroundColor", props["backgroundColor"], errors)
    if "textColor" in props:
        check_color(path + ".textColor", props["textColor"], errors)
    if "textSize" in props:
        check_text_size(path + ".textSize", props["textSize"], errors)
    if "textStyle" in props and props["textStyle"] not in TEXT_STYLES:
        errors.append("%s: textStyle 只能是 %s，实际 %r"
                      % (path, sorted(TEXT_STYLES), props["textStyle"]))
    if "gravity" in props:
        check_gravity(path + ".gravity", props["gravity"], errors)
    if "orientation" in props and props["orientation"] not in ORIENTATIONS:
        errors.append("%s: orientation 只能是 vertical / horizontal，实际 %r"
                      % (path, props["orientation"]))
    for pad in ("padding", "paddingTop", "paddingBottom", "marginBottom", "marginStart"):
        if pad in props:
            check_dim(path + "." + pad, props[pad], errors)

    if node_type == "TextView" and "text" in props and not isinstance(props["text"], str):
        errors.append("%s: TextView.text 必须是字符串，实际 %r" % (path, props["text"]))

    if node_type == "ImageView":
        if "name" in props:
            name = props["name"]
            if not isinstance(name, str) or not name.strip():
                errors.append("%s: ImageView.name 必须是非空字符串" % path)
            elif icons is not None and name not in icons:
                errors.append("%s: ImageView.name=%r 未出现在 viewIconData 中" % (path, name))
        for key in ("width", "height"):
            if key in props:
                check_dim(path + "." + key, props[key], errors)

    if node_type == "RelativeLayout":
        if "layout_alignParentStart" in props and not isinstance(props["layout_alignParentStart"], bool):
            errors.append("%s: layout_alignParentStart 应为布尔值" % path)
        if "layout_centerVertical" in props and not isinstance(props["layout_centerVertical"], bool):
            errors.append("%s: layout_centerVertical 应为布尔值" % path)
        if "layout_toEndOf" in props:
            target = props["layout_toEndOf"]
            if not isinstance(target, str) or target not in ids:
                errors.append("%s: layout_toEndOf=%r 必须引用同树内其他节点的 id" % (path, target))

    if node_type in LEAVES and "children" in props:
        warnings.append("%s: %s 通常不应有 children" % (path, node_type))


def walk(node, path, ids, errors, warnings, icons, is_root=False):
    if not isinstance(node, dict):
        errors.append("%s: 节点必须是 JSON 对象" % path)
        return
    node_type = node.get("type")
    if node_type not in COMPONENTS:
        errors.append("%s: type 必须是 %s 之一，实际 %r" % (path, list(COMPONENTS), node_type))
        return
    if is_root and node_type not in CONTAINERS:
        errors.append("%s: 根节点必须是 LinearLayout / RelativeLayout，实际 %r" % (path, node_type))

    props = node.get("props")
    node_id = props.get("id") if isinstance(props, dict) else None
    if isinstance(node_id, str) and node_id:
        if node_id in ids:
            errors.append("%s: id=%r 重复（每个节点 id 必须唯一）" % (path, node_id))
        else:
            ids[node_id] = path
    check_props(node_type, path, props, ids, errors, warnings, icons)

    children = node.get("children")
    if children is None:
        return
    if not isinstance(children, list):
        errors.append("%s: children 必须是数组" % path)
        return
    if node_type in LEAVES and children:
        errors.append("%s: %s 不能包含子节点" % (path, node_type))
        return
    for i, child in enumerate(children):
        walk(child, "%s.children[%d]" % (path, i), ids, errors, warnings, icons)


def validate_viewdata(doc, icons=None):
    """返回 (errors, warnings, id_map)"""
    errors, warnings, ids = [], [], {}
    if not isinstance(doc, dict):
        return ["根节点必须是 JSON 对象"], [], {}
    if "type" not in doc:
        errors.append("根节点缺少 type（文档明确禁止 {\"title\": \"...\"} 这类结构）")
    walk(doc, "root", ids, errors, warnings, icons, is_root=True)
    return errors, warnings, ids


# ---------------------------------------------------------------- 增量更新校验

def validate_updates(updates, viewdata, icons=None):
    errors, warnings, id_map = validate_viewdata(viewdata, icons)
    if errors:
        return ["模板本身不合法，无法校验更新数组"] + errors, warnings, id_map

    if not isinstance(updates, list):
        return ["更新必须是 JSON 数组，例如 [{\"action\":\"update\",...}]"], warnings, id_map
    if not updates:
        return ["更新数组为空（至少一条 update）"], warnings, id_map

    # 收集每个 id 对应的组件类型，便于校验 props 键
    node_types = {}

    def collect(node):
        if isinstance(node, dict):
            props = node.get("props") or {}
            nid = props.get("id")
            if isinstance(nid, str):
                node_types[nid] = node.get("type")
            for child in node.get("children") or []:
                collect(child)

    collect(viewdata)

    for i, item in enumerate(updates):
        p = "updates[%d]" % i
        if not isinstance(item, dict):
            errors.append("%s: 必须是非空对象" % p)
            continue
        if item.get("action") != "update":
            errors.append("%s: action 只能是 \"update\"，实际 %r" % (p, item.get("action")))
        nid = item.get("id")
        if not isinstance(nid, str) or not nid:
            errors.append("%s: 缺少 id" % p)
        elif nid not in node_types:
            errors.append("%s: id=%r 在 viewData 中不存在（无法定位节点）" % (p, nid))
        props = item.get("props")
        if not isinstance(props, dict):
            errors.append("%s: props 必须是对象" % p)
            continue
        if not props:
            errors.append("%s: props 为空，没有可更新的字段" % p)
            continue
        if "id" in props:
            errors.append("%s: props 里不要再写 id（id 放在更新项的顶层）" % p)
        ntype = node_types.get(nid)
        if not ntype:
            continue
        allowed = PROPS.get(ntype, set()) - {"id"}
        unknown = sorted(set(props) - allowed)
        if unknown:
            errors.append("%s: %s 不支持更新字段 %s" % (p, ntype, unknown))
        for key, value in props.items():
            if key in ("textColor", "backgroundColor"):
                check_color("%s.props.%s" % (p, key), value, errors)
            elif key == "textSize":
                check_text_size("%s.props.%s" % (p, key), value, errors)
            elif key in ("layout_width", "layout_height", "width", "height",
                         "padding", "paddingTop", "paddingBottom", "marginBottom", "marginStart"):
                check_dim("%s.props.%s" % (p, key), value, errors)
            elif key == "text" and not isinstance(value, str):
                errors.append("%s.props.text: 必须是字符串" % p)
    return errors, warnings, id_map


# ---------------------------------------------------------------- 遥测映射

def _num(state, key, digits=1, suffix=""):
    v = state.get(key)
    if not _is_number(v):
        return "--"
    if digits == 0:
        return ("%d" % round(v)) + suffix
    return ("%.*f" % (digits, v)) + suffix


def state_to_texts(state, seq=1):
    """把 publish_state() 的 JSON 映射成 {节点 id: 文本}"""
    if not isinstance(state, dict):
        state = {}

    temp = state.get("temp")
    target = state.get("temp_target")
    temp_txt = _num(state, "temp", 1, " ℃")
    if _is_number(temp) and _is_number(target):
        if temp > target + 1.0:
            temp_txt += "   !高温"
        elif temp < target - 1.0:
            temp_txt += "   !低温"
    if _is_number(target):
        temp_txt += "   目标 %.1f" % target

    level = state.get("level")
    level_min = state.get("level_min")
    level_txt = _num(state, "level", 0, " %")
    if _is_number(level) and _is_number(level_min) and level < level_min:
        level_txt += "   !水位低"

    mode = state.get("mode")
    mode_txt = mode if isinstance(mode, str) and mode else "--"
    hold = state.get("manual_left_s")
    hold_txt = ("%d s" % round(hold)) if _is_number(hold) and hold > 0 else "0 s"

    beat = "链路 正常"
    if state.get("sd_on") is False:
        beat += " · SD 未挂载"
    beat += "    刷新 %d" % int(seq)

    return {
        "line_time": "同步 %s" % (state.get("time") if isinstance(state.get("time"), str) else "--:--:--"),
        "line_temp": "温度 %s" % temp_txt,
        "line_ph": "PH %s    浊度 %s" % (_num(state, "ph", 1), _num(state, "turb", 1, " NTU")),
        "line_tds": "TDS %s    水位 %s" % (_num(state, "tds", 0, " ppm"), level_txt),
        "line_co2": "CO2 %s    光照 %s" % (_num(state, "co2", 0, " ppm"), _num(state, "light", 0)),
        "line_act": "灯 %s   泵 %s   气 %s" % (
            _num(state, "light_pwm", 0, "%"),
            _num(state, "pump_pct", 0, "%"),
            _num(state, "air_pct", 0, "%"),
        ),
        "line_mode": "模式 %s    保持 %s" % (mode_txt, hold_txt),
        "line_beat": beat,
    }


def texts_to_updates(texts, prev=None):
    updates = []
    for node_id, text in texts.items():
        if prev is not None and prev.get(node_id) == text:
            continue
        updates.append({"action": "update", "id": node_id, "props": {"text": text}})
    return updates


# ---------------------------------------------------------------- 自测

def _clone(doc):
    return json.loads(json.dumps(doc))


def self_test(template):
    cases = []

    def add(name, ok, fn):
        cases.append((name, ok, fn))

    good = _clone(template)

    def case_valid():
        errs, warns, ids = validate_viewdata(good)
        return errs

    def case_missing_id():
        doc = _clone(template)
        doc["children"][1]["props"].pop("id")
        errs, _, _ = validate_viewdata(doc)
        return [] if any("缺少必填 props" in e or "id" in e for e in errs) else ["未检出缺失 id"]

    def case_bad_type():
        doc = _clone(template)
        doc["children"][1]["type"] = "Button"
        errs, _, _ = validate_viewdata(doc)
        return [] if any("type 必须是" in e for e in errs) else ["未检出非法 type"]

    def case_dup_id():
        doc = _clone(template)
        doc["children"][2]["props"]["id"] = doc["children"][1]["props"]["id"]
        errs, _, _ = validate_viewdata(doc)
        return [] if any("重复" in e for e in errs) else ["未检出重复 id"]

    def case_bad_color():
        doc = _clone(template)
        doc["children"][1]["props"]["textColor"] = "#00FF00"
        errs, _, _ = validate_viewdata(doc)
        return [] if any("#AARRGGBB" in e for e in errs) else ["未检出错误颜色"]

    def case_small_text():
        doc = _clone(template)
        doc["children"][1]["props"]["textSize"] = "12sp"
        errs, _, _ = validate_viewdata(doc)
        return [] if any("textSize" in e for e in errs) else ["未检出过小字号"]

    def case_unknown_prop():
        doc = _clone(template)
        doc["children"][1]["props"]["layout_marginTop"] = 4
        errs, _, _ = validate_viewdata(doc)
        return [] if any("不支持 props 键" in e for e in errs) else ["未检出未验证的 props 键"]

    def case_good_updates():
        ups = [{"action": "update", "id": "line_temp", "props": {"text": "温度 26.4 ℃"}}]
        errs, _, _ = validate_updates(ups, template)
        return errs

    def case_bad_update_id():
        ups = [{"action": "update", "id": "nope", "props": {"text": "x"}}]
        errs, _, _ = validate_updates(ups, template)
        return [] if any("不存在" in e for e in errs) else ["未检出未知 id"]

    def case_empty_update():
        ups = [{"action": "update", "id": "line_temp", "props": {}}]
        errs, _, _ = validate_updates(ups, template)
        return [] if any("为空" in e for e in errs) else ["未检出空 props"]

    def case_bad_action():
        ups = [{"action": "set", "id": "line_temp", "props": {"text": "x"}}]
        errs, _, _ = validate_updates(ups, template)
        return [] if any("action" in e for e in errs) else ["未检出非法 action"]

    def case_state_mapping():
        state = {
            "temp": 27.9, "temp_target": 26.0, "ph": 7.2, "tds": 318, "turb": 3.1,
            "level": 62, "level_min": 50, "co2": 615, "light": 240, "light_pwm": 55,
            "pump_pct": 100, "air_pct": 60, "mode": "自动", "manual_left_s": 0,
            "sd_on": True, "time": "2026-11-16 19:30:05",
        }
        texts = state_to_texts(state, seq=3)
        ups = texts_to_updates(texts)
        errs, _, _ = validate_updates(ups, template)
        if "%" in texts["line_temp"]:
            return ["温度行出现了 % 号"]
        return errs

    def case_state_null():
        state = {"temp": None, "ph": None, "tds": None, "turb": None,
                 "level": None, "co2": None, "light": None, "light_pwm": None,
                 "pump_pct": None, "air_pct": None}
        texts = state_to_texts(state)
        ups = texts_to_updates(texts)
        errs, _, _ = validate_updates(ups, template)
        return errs

    add("模板合法", True, case_valid)
    add("缺 id -> 报错", True, case_missing_id)
    add("非法 type -> 报错", True, case_bad_type)
    add("重复 id -> 报错", True, case_dup_id)
    add("错误颜色 -> 报错", True, case_bad_color)
    add("过小字号 -> 报错", True, case_small_text)
    add("未验证 props -> 报错", True, case_unknown_prop)
    add("合法增量更新", True, case_good_updates)
    add("更新引用未知 id -> 报错", True, case_bad_update_id)
    add("空 props -> 报错", True, case_empty_update)
    add("非法 action -> 报错", True, case_bad_action)
    add("遥测映射（含告警）", True, case_state_mapping)
    add("遥测全为 null", True, case_state_null)

    failed = 0
    for name, _, fn in cases:
        try:
            errs = fn() or []
        except Exception as exc:  # noqa: BLE001
            errs = ["自测抛异常: %s: %s" % (type(exc).__name__, exc)]
        if errs:
            failed += 1
            print("FAIL  %s" % name)
            for e in errs[:5]:
                print("        %s" % e)
        else:
            print("PASS  %s" % name)
    print("\n自测：%d/%d 通过" % (len(cases) - failed, len(cases)))
    return 0 if failed == 0 else 1


# ---------------------------------------------------------------- 入口

def _load_json(path):
    with open(path, "r", encoding="utf-8-sig") as f:
        return json.load(f)


def main(argv=None):
    ap = argparse.ArgumentParser(description="Rokid CUSTOM_VIEW 视图与增量更新离线校验")
    ap.add_argument("viewdata", nargs="?", default=DEFAULT_VIEWDATA,
                    help="viewData JSON 路径（默认 %s）" % DEFAULT_VIEWDATA)
    ap.add_argument("--updates", metavar="FILE", help="校验增量更新数组 JSON")
    ap.add_argument("--icons", metavar="FILE", help="viewIconData JSON（[{name,data}]）")
    ap.add_argument("--state", metavar="FILE", help="把遥测 JSON 映射成增量更新并输出")
    ap.add_argument("--prev", metavar="FILE", help="上一次的节点文本（只输出变化字段）")
    ap.add_argument("--pretty", action="store_true", help="--state 输出缩进 JSON")
    ap.add_argument("--self-test", action="store_true", help="跑内置正反例自测")
    args = ap.parse_args(argv)

    try:
        viewdata = _load_json(args.viewdata)
    except OSError as exc:
        print("[x] 读不到模板：%s" % exc)
        return 2
    except json.JSONDecodeError as exc:
        print("[x] 模板不是合法 JSON：%s" % exc)
        return 2

    icons = None
    if args.icons:
        try:
            icon_doc = _load_json(args.icons)
            icons = {i.get("name") for i in icon_doc if isinstance(i, dict)}
        except Exception as exc:  # noqa: BLE001
            print("[x] 读不到图标 JSON：%s" % exc)
            return 2

    if args.self_test:
        return self_test(viewdata)

    if args.state:
        try:
            state = _load_json(args.state)
        except Exception as exc:  # noqa: BLE001
            print("[x] 读不到遥测 JSON：%s" % exc)
            return 2
        prev = _load_json(args.prev) if args.prev else None
        texts = state_to_texts(state, seq=1)
        updates = texts_to_updates(texts, prev)
        print(json.dumps(updates, ensure_ascii=False, indent=2 if args.pretty else None))
        errors, _, _ = validate_updates(updates, viewdata, icons)
        if errors:
            for e in errors:
                print("[x] %s" % e, file=sys.stderr)
            return 1
        return 0

    if args.updates:
        try:
            updates = _load_json(args.updates)
        except Exception as exc:  # noqa: BLE001
            print("[x] 读不到更新数组：%s" % exc)
            return 2
        errors, warnings, _ = validate_updates(updates, viewdata, icons)
    else:
        errors, warnings, _ = validate_viewdata(viewdata, icons)

    for w in warnings:
        print("[!] %s" % w)
    if errors:
        for e in errors:
            print("[x] %s" % e)
        print("\n校验失败：%d 个问题" % len(errors))
        return 1

    print("[ok] 校验通过：%s" % args.viewdata)
    return 0


if __name__ == "__main__":
    sys.exit(main())
