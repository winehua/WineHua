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

from pathlib import Path

from . import coverage as _coverage
from . import frame

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


def marker(ctx: dict) -> dict:
    """能力标记判定：读归档的探针结论文件，首行 `pass` 前缀 = PASS，其余 FAIL。

    「设备端只跑不判」的落地形态之一：app 进程内的探针（不走 wine 结果
    协议）把结论枚举写标记文件，harness 归档后由本判定器裁决 —— 判定只读
    归档，设备不必重跑。argument = 归档 device-results/ 下的文件名。

    标记缺失 = FAIL（不是 SKIP）：探针自缓存且每次 bring-up 都跑，没归档
    到标记说明 bring-up 或归档链本身出了问题，静默 SKIP 会把这类故障判绿。
    """
    name = ctx.get("validator") or ""
    path = Path(ctx["run_dir"]) / "device-results" / name if ctx.get("run_dir") else None
    if not path or not path.is_file():
        return {"status": "FAIL", "stage": "marker",
                "message": f"标记文件未归档: {name}（bring-up 未跑或归档链断）"}
    text = path.read_text(errors="replace").strip()
    if text.startswith("pass"):
        return {"status": "PASS", "stage": "marker", "message": text}
    return {"status": "FAIL", "stage": "marker",
            "message": text or "(空标记)"}


REGISTRY = {
    "result-json": result_json,
    "visual": visual,
    "marker": marker,
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
