"""smoke 判定器：把设备端结果与采集到的帧转成 PASS/FAIL/SKIP。

判定与执行分离（docs/SMOKE_V2_DESIGN.md §8）：同一套判定器既服务于 run 结束
时的自动判定，也服务于 `smoke.py check <run-dir>` 对历史归档重跑判定 ——
改判定规则不需要重跑设备。

判定器接口（纯函数）:

    check(ctx) -> {"status": "PASS|FAIL|SKIP", "stage": str,
                   "message": str, "metrics": dict}

ctx = {
    "run_dir": Path,        # 归档目录
    "test_id": str,
    "test": dict,           # 测试定义（suites.json 条目）
    "result": dict | None,  # 设备端结果 JSON
    "frame": Path | None,   # 采集到的固定帧截图
    "validator": str,       # visual:* 判定器名
}

checks 声明（test.json / suite 定义）:
    "result-json"                 设备端结果 status（默认）
    "visual:<validator>"          对固定帧跑视觉校验器
"""

from __future__ import annotations

from . import coverage as _coverage
from . import frame


def result_json(ctx: dict) -> dict:
    result = ctx.get("result")
    if not result:
        return {"status": "FAIL", "stage": "missing-result",
                "message": "设备端结果文件缺失"}
    return {
        "status": result.get("status", "FAIL"),
        "stage": result.get("stage", ""),
        "message": result.get("message", ""),
        "metrics": result.get("metrics", {}),
    }


def visual(ctx: dict) -> dict:
    frame_path = ctx.get("frame")
    if not frame_path:
        return {"status": "FAIL", "stage": "missing-frame",
                "message": "未采集到固定帧截图（截图时机错过或测试未渲染）"}
    report = frame.validate(ctx["validator"], frame_path)
    return {
        "status": report["status"],
        "stage": f"visual:{ctx['validator']}",
        "message": f"{report['validator']} on {frame_path.name}",
        "metrics": report,
    }


REGISTRY = {
    "result-json": result_json,
    "visual": visual,
    # suite 级判定：读 ctx["summary"]（整份设备端结果）
    "coverage": _coverage.coverage,
}


def parse_checks(declared: list) -> list:
    """把 checks 声明展开为 (判定器名, validator) 列表。"""
    items = []
    for entry in declared or ["result-json"]:
        name, _, argument = entry.partition(":")
        items.append((name, argument))
    return items


def evaluate(declared: list, ctx: dict) -> dict:
    """执行声明的判定器并合并结论（任一 FAIL → FAIL；全 SKIP → SKIP）。"""
    verdicts = []
    for name, argument in parse_checks(declared):
        runner = REGISTRY.get(name)
        if runner is None:
            return {"status": "FAIL", "stage": "checks",
                    "message": f"未知判定器: {name}"}
        local = dict(ctx)
        local["validator"] = argument
        verdicts.append({"check": name, "argument": argument, **runner(local)})
    statuses = [item["status"] for item in verdicts]
    if not statuses:
        status = "SKIP"
    elif all(item == "SKIP" for item in statuses):
        status = "SKIP"
    else:
        status = "FAIL" if "FAIL" in statuses else "PASS"
    failed = next((item for item in verdicts if item["status"] == "FAIL"), None)
    return {
        "status": status,
        "stage": failed["stage"] if failed else "",
        "message": failed["message"] if failed else "",
        "verdicts": verdicts,
    }
