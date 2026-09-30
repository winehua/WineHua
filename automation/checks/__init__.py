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

# 显示序列"新鲜度"上限：程序结束前这么久内序列都没再推进 ⇒ 帧只在前段出过
# （粘性的 displayed 字段看不出来，见 presented_route）。用例本身按秒轮询，
# 正常跑时该值 ≈ 一个轮询周期 (1s)，2.5s 留出抖动余量。
MAX_DISPLAY_STALL_MS = 2500


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
    动画相位波动（旋转角度不同颜色桶分布不同），单帧相位判不过，采集侧按序
    多截、判定取最好的一帧。

    X 路线 SKIP（判据债，证据在 docs/engineering/display-route-known-issues.md
    §2.4）：displayroute 的出图面是侧栏里的**预览小框**（SmokeDevPanel 的
    XComponent，4:3、约 500×390 物理像素），而本判定器对**整屏截图**做四象限
    ⇒ 在该场景结构性失效（2026-10-01 实测：X 路线 GL 用例 4 帧全 FAIL，而
    帧内容与 [GUEST-FRAMES] 统计都证明出图正常）。SKIP 是诚实答案：这一格
    没验，不是验过了 —— 出图与否由 presented-route 与宿主显示序列门裁定。
    按区域裁剪的判定器未做（做了才能把这一格从 SKIP 变回 PASS/FAIL）。"""
    result = ctx.get("result") or {}
    if ((result.get("metrics") or {}).get("expectedRoute")) == "x11":
        return {"status": "SKIP", "stage": "visual:x11-preview-pane",
                "message": "X 路线出图面在侧栏预览框，整屏四象限判定不适用（§2.4 判据债）"}
    paths = ctx.get("frames") or []
    if not paths:
        # 固定帧只在 duration_ms >= 2500 且进入末 2 秒时渲染；结果里没有
        # fixedFrame 就说明程序根本没到那个阶段（时长不够），与"渲染失败"是
        # 两回事 —— 按现象给方向，别让人去查渲染 (review F12)。
        detail = "" if (result.get("metrics") or {}).get("fixedFrame") else \
            "；结果里没有 fixedFrame —— 用例时长不足 2.5s 时固定帧不会渲染"
        return {"status": "FAIL", "stage": "missing-frame",
                "message": f"未采集到固定帧截图（截图时机错过或测试未渲染）{detail}"}
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


def presented_route(ctx: dict) -> dict:
    """呈现归属判定：**本路线**有没有把**这个客户端的**帧送上屏。

    为什么需要它（2026-10-01 实测）：job 声明 `WINEHUA_DISPLAY_ROUTE=x11`，
    但那次启动 env 丢了路线键，GL 帧被 wayland 渲染器取走、X 路线一帧没收到，
    用例照样 PASS —— `displayFps > 0` 只证明"有人在出图"。

    判据三段（缺一段即 FAIL，不 SKIP —— 这正是要防的静默）：
      1. expectedRoute 必须在结果里：它是 smoke.py 随环境下发的期望路线
         （build_job 注入 WINEHUA_SMOKE_EXPECT_ROUTE）。**缺了就是环境被丢**，
         正是 2026-10-01 那次事故的形态 —— 不能默默当成 wayland 默认。
      2. expectedRoute == presentedRoute（宿主自报的呈现路线）；
      3. presentedRoute == x11 时还要求归属：呈现那一帧的 surface key 高 32 位
         必须等于**运行器记录的 spawn pid**（设备端 suite-summary 的
         tests[].pid，宿主的调起事实）。

    第 3 段为什么不用 guest 自报的 presentedSelf（2026-10-01 实测）：key 高 32
    位是 Unix getpid()（mesa/win32u 填），而 guest 只拿得到 Wine ptid
    （GetCurrentProcessId，实测 key pid=10765 vs ptid=596）⇒ guest 侧比对恒假、
    是根"永远说不"的假指标。归属只能由宿主两侧事实对账：key pid 来自线上自报，
    spawn pid 来自宿主自己的调起记录 —— 两者相等才证明这一帧是本进程的面。
    """
    result = ctx.get("result") or {}
    metrics = result.get("metrics") or {}
    expected = metrics.get("expectedRoute")
    presented = metrics.get("presentedRoute")
    if expected is None or presented is None:
        return {"status": "FAIL", "stage": "presented-route",
                "message": "结果里缺 expectedRoute/presentedRoute"
                           "（设备端构建过旧，或启动环境被丢 —— 后者正是本判据要抓的）"}

    if expected != presented:
        return {"status": "FAIL", "stage": "presented-route",
                "message": f"路线漂移: 期望 {expected}，宿主实际呈现 {presented}",
                "metrics": {"expected": expected, "presented": presented}}

    # 声明路线兜底（F7）：declaredRoute 是 job 文件里写的路线，smoke.py 在合并
    # CLI 覆盖**之前**抄下来随环境下发，所以它改不掉。CLI --env 可以把期望值与
    # 实跑值一起改（两者同源），但改不掉声明 —— 呈现与声明不符 = 这次跑的不是
    # job 声明的场景，不能算过。
    declared = metrics.get("declaredRoute")
    if declared and declared != "-" and declared != presented:
        return {"status": "FAIL", "stage": "presented-route",
                "message": f"呈现路线 {presented} 与 job 声明 {declared} 不符"
                           f"（CLI --env 覆盖改了 job 的语义；归档 job.json 里是合并后的 env）",
                "metrics": {"declared": declared, "presented": presented}}

    # 新鲜度（F6）：displayed 那几个字段是**粘性**的（观察到一次就不再回落），
    # 只证明"曾经出过图"。displayStallMs = 显示序列最后一次推进距程序结束的
    # 时间，只有它能否掉"头 1 秒出过、之后宿主停摆"。
    stall = metrics.get("displayStallMs")
    if isinstance(stall, (int, float)) and stall > MAX_DISPLAY_STALL_MS:
        return {"status": "FAIL", "stage": "presented-route",
                "message": f"显示序列在结束前 {stall} ms 就不再推进（帧只在前段出过；"
                           f"阈值 {MAX_DISPLAY_STALL_MS}ms）",
                "metrics": {"displayStallMs": stall}}

    if presented == "x11":
        key = metrics.get("presentedKey") or 0
        if not key:
            return {"status": "FAIL", "stage": "presented-route",
                    "message": "x11 路线在出图，但显示序列里没有 guest 面的 key "
                               "(presentedKey=0) —— 出的是 X 服务端内容，不是 guest 面"}
        spawn_pid = (ctx.get("device") or {}).get("pid")
        if not spawn_pid:
            return {"status": "FAIL", "stage": "presented-route",
                    "message": "归档缺运行器记录的 spawn pid（设备端构建过旧），"
                               "无法做 x11 归属断言"}
        key_pid = key >> 32
        if key_pid != spawn_pid:
            return {"status": "FAIL", "stage": "presented-route",
                    "message": f"呈现的不是本次运行进程的面: key pid={key_pid} "
                               f"spawn pid={spawn_pid}",
                    "metrics": {"expected": expected, "presented": presented,
                                "presentedKey": key, "spawnPid": spawn_pid}}
        return {"status": "PASS", "stage": "presented-route",
                "message": f"路线一致 ({presented}) 且归属本进程 "
                           f"(pid={spawn_pid}, key={key})",
                "metrics": {"expected": expected, "presented": presented,
                            "presentedKey": key, "spawnPid": spawn_pid}}
    return {"status": "PASS", "stage": "presented-route",
            "message": f"路线一致 ({presented}, key={metrics.get('presentedKey')})",
            "metrics": {"expected": expected, "presented": presented,
                        "presentedKey": metrics.get("presentedKey")}}


REGISTRY = {
    "result-json": result_json,
    "visual": visual,
    "marker": marker,
    "presented-route": presented_route,
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
