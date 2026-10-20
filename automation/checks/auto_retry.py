"""auto-retry 判定器：D47 首启失败自动重试（90c26c7 止血 + 批次 1 F1 补漏）的
归档行为认证。

背景（2026-10-11）: 全新安装首启 wineboot 偶发失败 → state:failed。止血 =
首启窗口首次失败自动重试一次（WineEnvService.scheduleColdStartAutoRetry，
「每进程一次」预算）；批次 1 复核 F1 补漏 = 关闭卡片/显式重试作废待定重试
（cancelColdStartAutoRetry，日志指纹 `[AUTO-RETRY] cancelled`）。

判据数据 = 归档 device-evidence/auto-retry.log —— smoke.py archive_evidence
在 suite 结束时从设备 hilog 过滤 `AUTO-RETRY` 拉回（hilog 只留几分钟，晚了
会被冲）。D47 触发本身是偶发的：无信号 = SKIP —— 合法答案：本轮没验，
不是验过了（不误报绿），也不是红。

判定语义:
    证据文件缺失            → SKIP（未触发 D47 场景，或老运行器无此项）
    无 [AUTO-RETRY] 行      → SKIP（同上）
    失败行 >= 2             → FAIL（「每进程一次」预算破裂；若归档确跨多
                              会话也应人工复核 —— 预算是 per-进程的）
    失败行 <= 1 且有行      → PASS（机制按预算触发；cancelled/skipped 出现
                              一并记入 message —— cancelled 即 F1 修复的
                              实测指纹「关闭被尊重」）

已知边界: F1 指纹（cancelled）只在真的走「关闭」路径时出现，retry-only 的
归档不能证明 F1 代码在位（pre-fix 构建同样只产单行 fail）；版本在位性靠
构建对账，本判定器不越权装作能判版本。
"""

from __future__ import annotations

from pathlib import Path

_FAIL_MARK = "cold-start engine fail"


def auto_retry(ctx: dict) -> dict:
    run_dir = ctx.get("run_dir")
    path = (Path(run_dir) / "device-evidence" / "auto-retry.log") if run_dir else None
    if not path or not path.is_file():
        return {"status": "SKIP", "stage": "auto-retry-no-evidence",
                "message": "归档无 auto-retry.log —— D47 场景未触发，或运行器"
                           "无此项（老构建），本轮不下结论"}
    text = path.read_text(errors="replace")
    lines = [line for line in text.splitlines() if "[AUTO-RETRY]" in line]
    if not lines:
        return {"status": "SKIP", "stage": "auto-retry-no-signal",
                "message": "本轮无 [AUTO-RETRY] 输出（首启未失败 = D47 未触发），"
                           "无信号是合法答案"}
    fails = sum(1 for line in lines if _FAIL_MARK in line)
    cancelled = sum(1 for line in lines if "cancelled" in line)
    skipped = sum(1 for line in lines if "skipped" in line)
    metrics = {"lines": len(lines), "fail": fails,
               "cancelled": cancelled, "skipped": skipped}
    detail = f"[AUTO-RETRY] 行 {len(lines)}: fail={fails}, cancelled={cancelled}, skipped={skipped}"
    if fails >= 2:
        return {"status": "FAIL", "stage": "auto-retry-budget",
                "message": f"失败行 {fails} 条 —— 「每进程一次」预算破裂"
                           f"（或归档跨多会话，需复核）。{detail}",
                "metrics": metrics}
    extra = []
    if cancelled:
        extra.append("关闭被尊重（F1 指纹）")
    if skipped:
        extra.append("重试被接管让位（state 已推进）")
    return {"status": "PASS", "stage": "auto-retry",
            "message": f"自动重试机制按预算触发（fail={fails}）"
                       f"{'；' + '；'.join(extra) if extra else ''}",
            "metrics": metrics}
