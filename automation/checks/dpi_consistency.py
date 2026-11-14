"""DPI 一致性判定（suite 级，只读归档）。

背景（2026-11-14 war3 实测）：unaware 窗口的窗口消息按 192→96 虚化，而全屏
游戏按自己的显示模式（如 800x600）渲染并由合成器拉伸，点击坐标被二次变换，
落点只有一半并钳在左上 1/4。修复 = 无 manifest 进程默认 system-aware +
监视器 DPI 与 system_dpi 同源；本判定用 wine stderr 的 DPIDIAG/MSGDIAG 打点
验证不变量，不依赖设备现场。

判据（任一不满足 FAIL）：
  1. 每个进程 DPIDIAG sysparams: override=192 且 system_dpi=192
  2. 本路线监视器注册 DPIDIAG wayland_source/x11_source 若出现则 dpi=192
     （暖启动读注册表缓存，驱动回调不执行，本会话可能没有这两行）
  3. 每条 MSGDIAG: win_dpi=192 且 raw==win（出现 96 或坐标不一致即 FAIL）；
     选中 input-* 注入用例时至少要求一条消息样本，融合模式（只选基本
     用例）没有注入通道，允许消息样本为空
  4. 至少一条 DPIDIAG wincreate 的 thread_ctx 为 aware（awareness != 0）

范围：wine stderr 跨会话追加，只取最后一次 wineserver 启动之后的部分，
否则会把历史会话的旧口径行算进来（2026-11-14 实测首个归档误报 179 条）。
进程 spawn 头按 entryParams 的 `|` 分隔字段识别 wineserver（`|wineserver|`）；
不能用 `.*wineserver` —— 每个进程头的 env 都带 APPSPAWN_FD_wineserver_sock，
会匹配到任意进程，把范围切到最后一次 spawn 之后（实测只统计到 1 个进程）。
"""

from __future__ import annotations

import json
import re
from pathlib import Path

SYS_PARAMS_RE = re.compile(
    r"DPIDIAG sysparams pid=(\d+) env=(\S+) override=(\d+) system_dpi=(\d+)")
SOURCE_RE = re.compile(
    r"DPIDIAG (wayland_source|x11_source) pid=(\d+) name=(\S+) dpi=(\d+)")
WINCREATE_RE = re.compile(
    r"DPIDIAG wincreate pid=(\d+) hwnd=0x[0-9a-fA-F]+ thread_ctx=0x([0-9a-fA-F]+)")
MSG_RE = re.compile(
    r"MSGDIAG raw=(-?\d+),(-?\d+) win=(-?\d+),(-?\d+) hwnd=\S+ msg=\S+ "
    r"dctx=\S+ thread_dpi=\d+ mon=\d+ raw_mon=\d+ win_dpi=(\d+)")
SESSION_MARK_RE = re.compile(r"^=== PID=\d+ .*\|wineserver\|", re.M)


def _fail(stage: str, message: str, metrics: dict | None = None) -> dict:
    return {"status": "FAIL", "stage": f"dpi-consistency:{stage}",
            "message": message, "metrics": metrics or {}}


def dpi_consistency(ctx: dict) -> dict:
    run_dir = Path(ctx["run_dir"])
    evidence_files = sorted((run_dir / "device-evidence").glob("wine_stderr_*.log"))
    if not evidence_files:
        return _fail("evidence", "device-evidence 缺少 wine_stderr_*.log")

    job = {}
    job_path = run_dir / "job.json"
    if job_path.is_file():
        try:
            job = json.loads(job_path.read_text())
        except json.JSONDecodeError:
            job = {}
    route = ((job.get("params") or {}).get("env") or {}).get(
        "WINEHUA_DISPLAY_ROUTE", "wayland")
    source_prefix = "x11_source" if route == "x11" else "wayland_source"
    # 融合模式没有 guest 坐标注入通道（按窗注入走 byTitle），矩阵只选基本
    # 用例；此时允许没有消息样本，但仍校验出现的每条消息。
    selected = [str(t) for t in (job.get("tests") or [])]
    injected = not selected or any(t.startswith("input-") for t in selected)

    text = "\n".join(
        path.read_text(errors="replace") for path in evidence_files)

    # 只取最后一次 wineserver 启动之后（本次引擎会话）
    markers = list(SESSION_MARK_RE.finditer(text))
    if markers:
        text = text[markers[-1].start():]

    sys_rows = SYS_PARAMS_RE.findall(text)
    source_rows = SOURCE_RE.findall(text)
    wincreate_rows = WINCREATE_RE.findall(text)
    msg_rows = MSG_RE.findall(text)

    metrics = {
        "route": route,
        "processes": len(sys_rows),
        "sources": len(source_rows),
        "wincreate": len(wincreate_rows),
        "messages": len(msg_rows),
    }

    if not sys_rows:
        return _fail("sysparams", "缺少 DPIDIAG sysparams（打点未编入或进程未启动）",
                     metrics)
    bad_sys = [row for row in sys_rows
               if int(row[2]) != 192 or int(row[3]) != 192]
    if bad_sys:
        sample = ", ".join(f"pid={row[0]} env={row[1]} override={row[2]} "
                           f"system_dpi={row[3]}" for row in bad_sys[:5])
        return _fail("sysparams",
                     f"{len(bad_sys)}/{len(sys_rows)} 个进程 DPI 口径不一致: {sample}",
                     metrics)

    route_sources = [row for row in source_rows if row[0] == source_prefix]
    bad_sources = [row for row in route_sources if int(row[3]) != 192]
    if bad_sources:
        sample = ", ".join(f"name={row[2]} dpi={row[3]}" for row in bad_sources[:5])
        return _fail("source", f"{len(bad_sources)} 个监视器 DPI != 192: {sample}",
                     metrics)

    if not wincreate_rows:
        return _fail("wincreate", "缺少 DPIDIAG wincreate（窗口创建打点未生效）",
                     metrics)
    aware_rows = [row for row in wincreate_rows if int(row[1], 16) & 0x0F]
    if not aware_rows:
        return _fail("wincreate",
                     f"{len(wincreate_rows)} 次窗口创建全部 unaware —— "
                     "默认 system-aware 未生效", metrics)

    if not msg_rows:
        if injected:
            return _fail("message", "缺少 MSGDIAG（消息打点未生效或无窗口消息）",
                         metrics)
        # 未选注入用例：融合模式无 guest 坐标注入，无消息样本属预期
        return {
            "status": "PASS",
            "stage": "dpi-consistency",
            "message": (f"DPI 口径一致（无注入用例，无消息样本）: "
                        f"{len(sys_rows)} 进程 override=system_dpi=192, "
                        f"{len(route_sources)} 监视器 dpi=192, "
                        f"{len(wincreate_rows)} 次建窗 aware"),
            "metrics": metrics,
        }
    bad_msgs = []
    for row in msg_rows:
        raw_x, raw_y, win_x, win_y = map(int, row[:4])
        win_dpi = int(row[4])
        if win_dpi != 192 or (raw_x, raw_y) != (win_x, win_y):
            bad_msgs.append(row)
    if bad_msgs:
        sample = "; ".join(
            f"raw={row[0]},{row[1]} win={row[2]},{row[3]} win_dpi={row[4]}"
            for row in bad_msgs[:5])
        return _fail("message",
                     f"{len(bad_msgs)}/{len(msg_rows)} 条窗口消息坐标域不一致: "
                     f"{sample}", metrics)

    return {
        "status": "PASS",
        "stage": "dpi-consistency",
        "message": (f"DPI 口径一致: {len(sys_rows)} 进程 override=system_dpi=192, "
                    f"{len(route_sources)} 监视器 dpi=192, "
                    f"{len(wincreate_rows)} 次建窗 aware, "
                    f"{len(msg_rows)} 条消息 raw==win (win_dpi=192)"),
        "metrics": metrics,
    }
