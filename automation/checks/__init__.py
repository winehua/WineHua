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

import json
import re
from pathlib import Path

from . import auto_retry as _auto_retry
from . import coverage as _coverage
from . import frame
from . import wine_trace as _wine_trace

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


def fusion_state(ctx: dict) -> dict:
    """M4b-T4: 状态序列判定 —— result JSON 的 metrics.stateSeq。
    判据: 5 步全 true (minimized/restored/fullscreen/unfullscreen/
    modal_b_owned, 探针 win32 状态确认驱动上报, 见 fusion_probe.c
    state_seq_*)。stateSeq 缺失 = env WINEHUA_SMOKE_PROBE_STATE 未到
    guest (同 fusion-clicks 的 expectedRoute 抓点)。stateNote 非空 =
    确认超时, 报超时步骤。全屏视觉帧判定已砍 (T3 Ruling: 帧采集时序
    竞争不可靠)。本判定只覆盖 win32 侧事实: host 侧事件到达 (FWM
    hilog 的 minimize/show/fullscreen/restore 时序) 不进自动判定, 以
    FWM 日志人工核验, capture 不归档 (final review I3)。"""
    result = ctx.get("result") or {}
    metrics = result.get("metrics") or {}
    seq = metrics.get("stateSeq")
    if not isinstance(seq, dict) or not seq:
        return {"status": "FAIL", "stage": "fusion-state",
                "message": "stateSeq 缺失 —— WINEHUA_SMOKE_PROBE_STATE env "
                           "未到 guest (job env 未下发或探针构建过旧)"}
    expected = ["minimized_a", "restored_a", "fullscreen_a",
                "unfullscreen_a", "modal_b_owned_a"]
    missing = [k for k in expected if not seq.get(k)]
    if missing:
        note = metrics.get("stateNote", "")
        return {"status": "FAIL", "stage": "fusion-state",
                "message": f"状态序列未达成: 缺 {','.join(missing)}"
                           f"{f' ({note})' if note else ''}"}
    return {"status": "PASS", "stage": "fusion-state",
            "message": "状态序列 5/5 (win32 确认驱动)"}


def fusion_clicks(ctx: dict) -> dict:
    """M4a-T6: 按窗点击计数判定 —— result JSON 的 metrics.clickCounts。
    判据: a >= 1 (窗 A 至少收到一次点击) 且 b == 0 (窗 B 未收到 —— 点击
    命中窗 A 时不泄漏给窗 B, 按窗路由的排他性)。windowsCreated=false 时
    FAIL (探针窗都没建出, 计数无意义)。"""
    result = ctx.get("result") or {}
    metrics = result.get("metrics") or {}
    clicks = metrics.get("clickCounts") or {}
    if metrics.get("windowsCreated") is False:
        return {"status": "FAIL", "stage": "fusion-clicks",
                "message": "探针双窗未创建 (windowsCreated=false)"}
    if not clicks:
        return {"status": "FAIL", "stage": "fusion-clicks",
                "message": "结果缺少 clickCounts 指标"}
    a = int(clicks.get("a", 0))
    b = int(clicks.get("b", 0))
    if a < 1:
        return {"status": "FAIL", "stage": "fusion-clicks",
                "message": f"窗 A 零点击 (clickCounts={{'a':{a},'b':{b}}}) —— 注入未命中或按窗路由未达"}
    if b != 0:
        return {"status": "FAIL", "stage": "fusion-clicks",
                "message": f"窗 B 收到点击 (b={b}) —— 按窗路由泄漏"}
    # 路线自报校验 (M4a 终验): expectedRoute 缺失/非 x11 = 冷启 env 被丢
    # (presented-route 判据不适用于 GDI 探针 —— 见 test.json checks 注释,
    # env 丢失检测由本条承担)。探针 "-" = env 没到 guest, 同 FAIL。
    expect = metrics.get("expectedRoute")
    if expect != "x11":
        return {"status": "FAIL", "stage": "fusion-clicks",
                "message": f"expectedRoute={expect!r} (期望 x11) —— smoke env "
                           f"未到 guest (冷启参数被丢或探针构建过旧)"}
    return {"status": "PASS", "stage": "fusion-clicks",
            "message": f"点击按窗命中 (a={a}, b=0), route 自报 x11"}


def fusion_popup(ctx: dict) -> dict:
    """M4c-T4: popup 链判定 —— result JSON 的 metrics.popupSeq。

    判据: opened（TrackPopupMenu 进入且驻留 ≥3s = OR 菜单窗 show 链在
    guest 侧成立；驻留时长是事实门 —— 立即返回 = 菜单没弹出）且 closed
    （程序化 WM_CANCELMODE 后返回 = close 链闭合；宿主侧 popup_hide →
    子窗销毁链 T3 已人工验证，本判定不重复覆盖）。expectedRoute 校验同
    fusion-clicks（env 丢失抓点）。ownerClicked 只记录不判（注入编排不是
    本链判据）。渲染呈现由 visual:fusion-popup-menu 另判。

    点选（WM_COMMAND）明确不判 —— T3 越界点选悬案未闭合（点击到达 wine
    但不产生 WM_COMMAND，四轮控制变量未收敛），移交 M4d；探针固定程序化
    开关菜单，不存在点选注入，无假 PASS 路径。"""
    result = ctx.get("result") or {}
    metrics = result.get("metrics") or {}
    seq = metrics.get("popupSeq")
    if not isinstance(seq, dict) or not seq:
        return {"status": "FAIL", "stage": "fusion-popup",
                "message": "popupSeq 缺失 —— 探针构建过旧（无 popup 指标）"}
    if metrics.get("windowsCreated") is False:
        return {"status": "FAIL", "stage": "fusion-popup",
                "message": "探针窗未创建 (windowsCreated=false)"}
    expect = metrics.get("expectedRoute")
    if expect != "x11":
        return {"status": "FAIL", "stage": "fusion-popup",
                "message": f"expectedRoute={expect!r} (期望 x11) —— smoke env "
                           f"未到 guest (冷启参数被丢或探针构建过旧)"}
    if not seq.get("opened"):
        return {"status": "FAIL", "stage": "fusion-popup",
                "message": f"菜单未弹出 (trackMs={seq.get('trackMs')}, "
                           f"result={seq.get('result')}) —— popup show 链断"}
    if not seq.get("closed"):
        return {"status": "FAIL", "stage": "fusion-popup",
                "message": "菜单未关闭 —— popup hide 链断（探针程序化关闭"
                           "未返回）"}
    return {"status": "PASS", "stage": "fusion-popup",
            "message": f"popup 开关链闭合 (trackMs={seq.get('trackMs')}, "
                       f"menuSeen={seq.get('menuSeen')}, "
                       f"rect={seq.get('menuRect')}), route 自报 "
                       f"{metrics.get('expectedRoute')}",
            "metrics": seq}


def fusion_popup_selection(ctx: dict) -> dict:
    """越界点选不做套件内自动判据 —— 恒 SKIP 并注明出处。

    悬案已闭合：M4d 破案真因 = multimode 注入口缺 motion 半边（修复
    8a1e975，见 docs/engineering/display-route-known-issues.md §2.14）。
    点选自动化另立门禁：专用 job smoke/jobs/popup-overflow-x11.json
    （2026-10-13 起该 job 走标准 result 协议 --automation/--result，探针
    checks[] 随归档由 result-json 判定；点击仍由人工 uitest 按物理坐标
    触发）。本 suite 用例不注入点选，保持 SKIP —— 套件无人点击时会假
    FAIL。SKIP 是合法答案，不硬造 PASS。"""
    return {"status": "SKIP", "stage": "fusion-popup-selection",
            "message": "越界点选不走套件内自动判定（悬案已闭合 8a1e975；"
                       "门禁 = 专用 job smoke/jobs/popup-overflow-x11.json，"
                       "人工 uitest 点击 + 归档 result-json 判定）——本用例不"
                       "注入点选，渲染/关闭链由 visual:fusion-popup-menu 与 "
                       "fusion-popup 另判"}


def visual(ctx: dict) -> dict:
    """对采集到的固定帧跑视觉校验。多帧任一通过即通过 —— 立方体/场景随
    动画相位波动（旋转角度不同颜色桶分布不同），单帧相位判不过，采集侧按序
    多截、判定取最好的一帧。

    历史注：X 路线曾按侧栏预览小框（SmokeDevPanel 的 XComponent）的归档
    矩形先裁剪再判定（D22 §2.4）。冷启自动起链落地后出图面已是桌面画布
    （与 wayland 同类全屏面），预览窗渲染前提不复存在，面板于 2026-10-20
    退役 —— x11 与 wayland 同走全屏四象限。fusion 判定器自带全屏定位。"""
    paths = ctx.get("frames") or []
    if not paths:
        # 固定帧只在 duration_ms >= 2500 且进入末 2 秒时渲染；结果里没有
        # fixedFrame 就说明程序根本没到那个阶段（时长不够），与"渲染失败"是
        # 两回事 —— 按现象给方向，别让人去查渲染 (review F12)。
        detail = "" if (ctx.get("result") or {}).get("metrics", {}).get("fixedFrame") else \
            "；结果里没有 fixedFrame —— 用例时长不足 2.5s 时固定帧不会渲染"
        return {"status": "FAIL", "stage": "missing-frame",
                "message": f"未采集到固定帧截图（截图时机错过或测试未渲染）{detail}"}
    reports = []
    for path in paths:
        report = frame.validate(ctx["validator"], path, region=None)
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
        # guest 事实自洽 (D49): 探针上报 selfPid 时, 只有它声明了与 key 同源
        # 的通道 (selfPidSource="unix-bridge", 待 unix 桥方案落地) 断言才启用
        # —— 三通道实验实证 (c394f259bdd): msvcrt _getpid=Wine ptid、/proc
        # 直读被 wine 文件层劫持 (两用例同值), 都与 key 不同源, 盲断言会让
        # 诚实值错杀 (05:21/05:42 两轮 x64/x86 全 FAIL)。通道未就位 → 跳过
        # 本段, 回落帧 key 同一性/spawn pid/attach 路径 (x64 快捷 PASS; x86
        # 同走帧 key 同一性, 见下段)。
        self_pid = metrics.get("selfPid")
        self_pid_source = metrics.get("selfPidSource")
        if self_pid and self_pid_source == "unix-bridge":
            if key_pid != int(self_pid):
                return {"status": "FAIL", "stage": "presented-route",
                        "message": f"呈现的不是本次运行进程的面: key pid={key_pid} "
                                   f"本进程 getpid={self_pid} (guest 事实自洽断言)",
                        "metrics": {"expected": expected, "presented": presented,
                                    "presentedKey": key, "selfPid": self_pid}}
            return {"status": "PASS", "stage": "presented-route",
                    "message": f"路线一致 ({presented}) 且归属本进程 "
                               f"(guest getpid={self_pid}, key={key})",
                    "metrics": {"expected": expected, "presented": presented,
                                "presentedKey": key, "selfPid": self_pid}}
        if key_pid != spawn_pid:
            # 归属不能靠「key pid == spawn pid」记账相等 —— 对 x86 结构性为假
            # (2026-10-11 实测推翻旧「explorer 抢 sticky」诊断): x86 (i386)
            # 程序经 wine loader 二次 NCP spawn 出实际承载进程, runner 记录的
            # spawn pid 只是它的启动器 wine_child (dlopen box64 后调
            # box64_hmos_main 跑 wine loader)。证据 (r20261011-204807):
            # WL_Broker StartNativeChildProcess childPid=34474 → WineChild[34474]
            # calling box64_hmos_main; 0.7s 后 WL_Broker 再 spawn
            # childPid=34486 + WL_NAPI ProcReg add name=winehua_graphics_smoke.exe;
            # 窗口 (gl 960x540) 归属 34486。五轮归档实测 pid 差 12-28 不固定
            # (差值 = 两次 spawn 间应用侧新分配的 pid 数); x64 原地 exec,
            # 两者恒等。帧面归属改用同一性判据: key = (unix pid<<32)|xwin,
            # 归档 [GUEST-FRAMES] 的 attach/detach 行记录的 key 与本轮结果
            # 自报的 presentedKey 全等 ⇒ 上屏那一帧就是 ledger 里登记过的本
            # 次窗口面 (key 含 pid+xwin, 异进程窗口不可能同 key)。几何佐证:
            # attach 行 src=WxH 应等于本轮渲染尺寸, 不等 = 证据矛盾按 FAIL
            # 交人工。残余盲区 (写明): 本窗口从未上屏时 presentedKey 停在
            # 上一窗粘性值, 若其 ledger 行恰在日志拉取窗口内可假过 —— 该
            # 形态五轮归档均未出现; 上屏停摆由 displayStallMs 段先行拦截,
            # 无 guest key 由 presentedKey=0 前段拦截。
            frame = _x11_guest_frame_of_key(ctx.get("run_dir"), key)
            if frame is not None:
                gw, gh = frame.get("src_w"), frame.get("src_h")
                w, h = metrics.get("width"), metrics.get("height")
                if gw and gh and w and h and (gw, gh) != (int(w), int(h)):
                    return {"status": "FAIL", "stage": "presented-route",
                            "message": f"帧 key 同源但几何矛盾: [GUEST-FRAMES] "
                                       f"attach src={gw}x{gh} ≠ 本轮渲染 {w}x{h}"
                                       f" (证据矛盾, 需人工核对)",
                            "metrics": {"presentedKey": key, "spawnPid": spawn_pid,
                                        "framePid": frame.get("pid"),
                                        "frameLine": frame.get("line")}}
                return {"status": "PASS", "stage": "presented-route",
                        "message": f"路线一致 ({presented}) 且归属成立 "
                                   f"(帧 key 与 [GUEST-FRAMES] {frame['kind']} 记录"
                                   f"全等, 窗口 pid={frame.get('pid')}; spawn "
                                   f"pid={spawn_pid} 为启动器)",
                        "metrics": {"expected": expected, "presented": presented,
                                    "presentedKey": key, "spawnPid": spawn_pid,
                                    "framePid": frame.get("pid"),
                                    "frameKind": frame["kind"]}}
            attach = _x11_guest_attach_of_pid(ctx.get("run_dir"), spawn_pid)
            if attach is None:
                return {"status": "FAIL", "stage": "presented-route",
                        "message": f"呈现的不是本次运行进程的面: key pid={key_pid} "
                                   f"spawn pid={spawn_pid}，且归档无 [GUEST-FRAMES] "
                                   f"帧面记录佐证 (同 key 无记录, spawn pid 无 "
                                   f"attach —— 日志被冲或运行器过旧)",
                        "metrics": {"expected": expected, "presented": presented,
                                    "presentedKey": key, "spawnPid": spawn_pid}}
            return {"status": "PASS", "stage": "presented-route",
                    "message": f"路线一致 ({presented}) 且归属本进程 "
                               f"(pid={spawn_pid} 面已 attach; 粘性 latest="
                               f"{key_pid} 在 ledger 无记录)",
                    "metrics": {"expected": expected, "presented": presented,
                                "presentedKey": key, "spawnPid": spawn_pid,
                                "attachKey": attach}}
        return {"status": "PASS", "stage": "presented-route",
                "message": f"路线一致 ({presented}) 且归属本进程 "
                           f"(pid={spawn_pid}, key={key})",
                "metrics": {"expected": expected, "presented": presented,
                            "presentedKey": key, "spawnPid": spawn_pid}}
    return {"status": "PASS", "stage": "presented-route",
            "message": f"路线一致 ({presented}, key={metrics.get('presentedKey')})",
            "metrics": {"expected": expected, "presented": presented,
                        "presentedKey": metrics.get("presentedKey")}}


def _x11_guest_attach_of_pid(run_dir, spawn_pid: int):
    """从归档 [GUEST-FRAMES] 日志找本 pid 帧面的 attach 事实。
    attach 行: "[GUEST-FRAMES] attach key=<十进制key> xwin=... pid=..."。
    返回 attach 的 key (int) 或 None (文件缺失/无本 pid 行)。只读归档。"""
    path = (Path(run_dir) / "device-evidence" / "guest-frames.log") if run_dir else None
    if not path or not path.is_file():
        return None
    for line in path.read_text(errors="replace").splitlines():
        m = re.search(r"\[GUEST-FRAMES\] attach key=(\d+)", line)
        if m and (int(m.group(1)) >> 32) == spawn_pid:
            return int(m.group(1))
    return None


def _x11_guest_frame_of_key(run_dir, frame_key: int):
    """从归档 [GUEST-FRAMES] 日志找 key 全等的帧面记录 (attach 优先, detach 兜底)。

    attach 行: "[GUEST-FRAMES] attach key=<k> xwin=... pid=... src=WxH ... kind=..."
    detach 行: "[GUEST-FRAMES] detach key=<k> xwin=... pid=... frames=N reason=..."
    两行同为该窗口的生命周期事实 (detach 必曾 attach, 日志窗口被 tail 截断时
    只剩 detach 也有同等的 key 同一性信息, 只是没有 src 几何)。
    返回 dict(key, kind, pid, src_w, src_h, line) 或 None (文件缺失/无同 key 行)。
    只读归档。"""
    path = (Path(run_dir) / "device-evidence" / "guest-frames.log") if run_dir else None
    if not path or not path.is_file():
        return None
    best = None
    for line in path.read_text(errors="replace").splitlines():
        m = re.search(r"\[GUEST-FRAMES\] (attach|detach) key=(\d+)", line)
        if not m or int(m.group(2)) != frame_key:
            continue
        kind = m.group(1)
        pid_m = re.search(r" pid=(\d+)", line)
        src_m = re.search(r" src=(\d+)x(\d+)", line)
        rec = {"key": frame_key, "kind": kind,
               "pid": int(pid_m.group(1)) if pid_m else None,
               "src_w": int(src_m.group(1)) if src_m else None,
               "src_h": int(src_m.group(2)) if src_m else None,
               "line": line.strip()[-200:]}
        if kind == "attach":
            return rec  # attach 带几何, 优先
        best = best or rec
    return best


REGISTRY = {
    "result-json": result_json,
    "visual": visual,
    "marker": marker,
    "presented-route": presented_route,
    "auto-retry": _auto_retry.auto_retry,
    "wine-trace": _wine_trace.wine_trace,
    "fusion-clicks": fusion_clicks,
    "fusion-state": fusion_state,
    "fusion-popup": fusion_popup,
    "fusion-popup-selection": fusion_popup_selection,
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
