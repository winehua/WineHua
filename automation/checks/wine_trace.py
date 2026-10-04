"""wine-trace 判定器：X 路线载体用例 (judge=external) 的行为门禁。

背景 (2026-10-04): 载体用例 (notepad 这类真实应用) 设备端永远落 SKIP
(写不出 smoke 结果协议), 此前行为判定只能靠人工/截屏。本判定器把
"载体在 X 路线走通了窗口与输入链" 变成归档数据的自动断言 —— 判定与
执行分离, 改判据对历史归档重跑 (smoke.py check), 不必重跑设备。

判据链 (全部来自归档 device-evidence/wine_stderr_*.log, 由
WINEHUA_WINEDEBUG=+x11drv,+event 产生, 用例 env 声明):

    1. 载体 marker   === PID=<n> entryParams=...<exe>|   进程确实 spawn
    2. 驱动加载      load_display_driver OHOS: display route=x11,
                     loading winex11 (per-process override)  路线送达 winex11
    3. X 连接        trace:x11drv:xinerama_init   显示枚举成功 = X 服务可用
    4. 窗口创建      trace:x11drv:X11DRV_create_win_data win   应用窗口拿到 X 窗口
    5. 事件流        FocusIn   XWM 焦点到达 = 窗口被管理、事件在流

归因边界 (实测教训, 2026-10-04 设备 .206): stderr 是全部子进程共享的
O_APPEND 文件, 按 entryParams 位置归因子进程会错 (相邻秒级 spawn 的
trace 整体错位到下一段)。本判定器因此做**进程无关的链路断言**:
签名在文件任一位置出现即算 —— 依赖前提是用例 env 独占声明了
WINEHUA_WINEDEBUG (runner 只把它注入该用例的进程), 这些签名只可能
来自载体自身。多载体同时开 trace 的编排不适用本判定器 (当前没有)。

判定语义:
    env 未声明 +x11drv   → SKIP (不误报: 没有 trace 就不下结论)
    env 已声明, 断言全过 → PASS
    env 已声明, 任一缺失 → FAIL (stage 指明断在哪一跳)
"""

from __future__ import annotations

import os
from pathlib import Path

# 断言链: (stage, 归档行内签名)。顺序 = 数据流顺序, 断在第一跳。
_SIGNATURES = (
    ("trace-x11-driver", "loading winex11 (per-process override)"),
    ("trace-x-server", "x11drv:xinerama_init"),
    ("trace-window", "x11drv:X11DRV_create_win_data win "),
    ("trace-focus", "FocusIn"),
)


def _vehicle_marker_found(text: str, exe: str) -> bool:
    """载体 marker: entryParams 里出现该 exe (C:\\smoke\\x64\\notepad.exe| 形式)。"""
    if not exe:
        return False
    basename = exe.replace("\\", "/").rsplit("/", 1)[-1]
    for line in text.splitlines():
        if line.startswith("=== PID=") and "entryParams=" in line:
            if f"{basename}|" in line:
                return True
    return False


def wine_trace(ctx: dict) -> dict:
    test = ctx.get("test") or {}
    env = test.get("env") or {}
    winedebug = env.get("WINEHUA_WINEDEBUG", "")
    if "+x11drv" not in winedebug:
        return {
            "status": "SKIP",
            "stage": "trace-not-enabled",
            "message": "wine-trace 判据需要用例 env 声明 "
                       "WINEHUA_WINEDEBUG=+x11drv,+event (当前缺 trace, 不下结论)",
        }

    run_dir = Path(ctx["run_dir"])
    evidence = run_dir / "device-evidence"
    files = sorted(evidence.glob("wine_stderr_*.log")) if evidence.is_dir() else []
    if not files:
        return {"status": "FAIL", "stage": "trace-no-evidence",
                "message": f"归档无 stderr 证据: {evidence}"}
    text = "".join(f.read_text(errors="replace") for f in files)

    if not _vehicle_marker_found(text, test.get("exe", "")):
        return {"status": "FAIL", "stage": "trace-no-vehicle",
                "message": f"载体未 spawn (marker 缺失): {test.get('exe', '')}"}

    for stage, signature in _SIGNATURES:
        if signature not in text:
            return {
                "status": "FAIL",
                "stage": stage,
                "message": f"X 路线载体链路断在 {stage}: 归档 trace 缺 '{signature}'",
            }

    return {
        "status": "PASS",
        "stage": "",
        "message": "X 路线载体链路完整: spawn → winex11(route=x11) → X 连接 → "
                   "窗口创建 → 焦点事件",
        "metrics": {"trace_signatures": len(_SIGNATURES) + 1},
    }
