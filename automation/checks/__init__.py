"""smoke 判定器：把设备端结果与采集到的帧转成 PASS/FAIL/SKIP。

判定与执行分离（docs/engineering/testing-design.md §8）：同一套判定器既服务于 run 结束
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
from . import benchmark

# 终态集合，与 smoke 程序协议一致 (thirdparty/wine/programs/winehua_smoke_protocol.h)
FINAL_STATUSES = ("PASS", "FAIL", "SKIP", "UNSUPPORTED")


def result_json(ctx: dict) -> dict:
    result = ctx.get("result")
    if not result:
        return {"status": "FAIL", "stage": "missing-result",
                "message": "设备端结果文件缺失"}
    status = result.get("status", "")
    if status not in FINAL_STATUSES:
        # 非终态 = 测试没跑完。程序跑测期间会反复写心跳快照 (status=RUNNING),
        # 卡死或被杀后留在盘上的就是最后那次心跳 —— 原样透传的话 RUNNING 既不
        # 等于 FAIL 也不是 PASS, judge_run 按"没有 FAIL 即 PASS"会把这颗卡死
        # 判成绿的 (实测 dxvk-modern-baseline-x64 卡在 D24S8 cube-array 180s
        # 超时, 结果文件是 RUNNING 快照, 顶上是 PASS)。判定层必须自己兜底。
        detail = " ".join(part for part in (result.get("stage", ""),
                                            result.get("message", "")) if part)
        return {"status": "FAIL", "stage": status or "unfinished",
                "message": f"结果非终态 ({status or '无 status 字段'}){': ' + detail if detail else ''}",
                "metrics": result.get("metrics", {})}
    return {
        "status": status,
        "stage": result.get("stage", ""),
        "message": result.get("message", ""),
        "metrics": result.get("metrics", {}),
    }


def vulkan_resize(ctx: dict) -> dict:
    """Check that a running Vulkan window presented through a rebuilt swapchain."""
    summary = ctx.get("summary") or {}
    tests = summary.get("tests", [])
    if len(tests) != 1:
        return {"status": "FAIL", "stage": "vulkan-resize",
                "message": f"expected one Vulkan resize result, got {len(tests)}"}
    result = tests[0]
    metrics = result.get("metrics") or {}
    initial = metrics.get("initialExtent") or []
    resized = metrics.get("resizedExtent") or []
    valid_extents = (isinstance(initial, list) and isinstance(resized, list) and
                     len(initial) == 2 and len(resized) == 2 and
                     all(isinstance(value, int) and value > 0
                         for value in [*initial, *resized]) and initial != resized)
    passed = (result.get("status") == "PASS" and
              metrics.get("resizeRequested") is True and
              metrics.get("resizeCompleted") is True and
              metrics.get("swapchainRebuilds") == 1 and
              isinstance(metrics.get("presentFrames"), int) and
              metrics["presentFrames"] >= 150 and
              metrics.get("presentQueueResult") == 0 and
              metrics.get("fallbackDetected") is False and valid_extents)
    return {"status": "PASS" if passed else "FAIL", "stage": "vulkan-resize",
            "message": (f"{metrics.get('presentFrames', 0)} frames, "
                        f"extent {initial} -> {resized}, "
                        f"rebuilds={metrics.get('swapchainRebuilds', 0)}"),
            "metrics": metrics}


def d3d11_resize(ctx: dict) -> dict:
    """Require post-resize D3D11 presents on the same device/swapchain object."""
    tests = (ctx.get("summary") or {}).get("tests", [])
    if len(tests) != 1:
        return {"status": "FAIL", "stage": "d3d11-resize",
                "message": f"expected one D3D11 resize result, got {len(tests)}"}
    result = tests[0]
    initial = result.get("initialExtent") or []
    resized = result.get("resizedExtent") or []
    valid_extents = (isinstance(initial, list) and isinstance(resized, list) and
                     len(initial) == 2 and len(resized) == 2 and
                     all(isinstance(value, int) and value > 0
                         for value in [*initial, *resized]) and initial != resized)
    passed = (result.get("status") == "PASS" and
              result.get("renderer") == "D3D11" and
              result.get("resizeRequested") is True and
              result.get("resizeCompleted") is True and
              result.get("resizeHresult") == "0x00000000" and
              result.get("swapchainRebuilds") == 1 and
              isinstance(result.get("frames"), int) and result["frames"] >= 120 and
              isinstance(result.get("postResizeFrames"), int) and
              result["postResizeFrames"] >= 60 and
              result.get("angleRegressions") == 0 and valid_extents)
    return {"status": "PASS" if passed else "FAIL", "stage": "d3d11-resize",
            "message": (f"{result.get('frames', 0)} frames, "
                        f"post-resize={result.get('postResizeFrames', 0)}, "
                        f"extent {initial} -> {resized}, "
                        f"rebuilds={result.get('swapchainRebuilds', 0)}"),
            "metrics": {key: result.get(key) for key in (
                "resizeRequested", "resizeCompleted", "resizeHresult",
                "initialExtent", "resizedExtent", "swapchainRebuilds",
                "frames", "postResizeFrames", "angleRegressions")}}


def visual(ctx: dict) -> dict:
    """对采集到的固定帧跑视觉校验。多帧任一通过即通过 —— 立方体/场景随
    动画相位波动（旋转角度不同颜色桶分布不同），单帧采样会把瞬时相位判成
    失败；采集侧按序多截，判定取最好的一帧。"""
    paths = ctx.get("frames") or []
    if not paths:
        return {"status": "FAIL", "stage": "missing-frame",
                "message": "未采集到固定帧截图（截图时机错过或测试未渲染）"}
    reports = []
    for path in paths:
        report = frame.validate(ctx["validator"], path)
        reports.append(report)
        if report["status"] == "PASS":
            return {
                "status": "PASS",
                "stage": f"visual:{ctx['validator']}",
                "message": f"{report['validator']} on {path.name}",
                "metrics": report,
            }
    last = reports[-1]
    return {
        "status": "FAIL",
        "stage": f"visual:{ctx['validator']}",
        "message": f"{last['validator']} 全部 {len(reports)} 帧未通过 ({last.get('message', '')})",
        "metrics": last,
    }


REGISTRY = {
    "result-json": result_json,
    "visual": visual,
    "vulkan-resize": vulkan_resize,
    "d3d11-resize": d3d11_resize,
    "benchmark": benchmark.check,
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
