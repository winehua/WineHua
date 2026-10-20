#!/usr/bin/env python3
"""auto-retry 判定器合成自测（批次 1 F1，2026-10-11）。

不依赖设备：合成 auto-retry.log 形态（即归档），跑判定器，断言「期望判定 =
实际判定」。产物落在 build/auto-retry-selftest/（build/ gitignored，每次
运行重新生成 —— rm -rf build/ 后从零复跑即毁灭性重建演练）。
运行 (仓库根目录):
    python3 automation/checks/selftest/auto_retry/gen_and_run.py
退出码: 全部形态符合期望 = 0 (GREEN), 任一 MISMATCH = 1 (RED)。
"""
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(REPO / "automation"))

from checks.auto_retry import auto_retry  # noqa: E402

OUT = REPO / "build" / "auto-retry-selftest"

# hilog 形前缀仅为形态仿真（判定器只做子串匹配，不解析字段位置）
_TAG = "10-11 20:15:03.123  5231  5231 W 00000/wine: "
FAIL_LINE = _TAG + "[AUTO-RETRY] cold-start engine fail (wineboot), retrying once"
CANCEL_LINE = _TAG + "[AUTO-RETRY] cancelled (dismissed by user)"
SKIP_LINE = _TAG + "[AUTO-RETRY] skipped (state=starting)"

# 形态: (名称, 日志内容 (None = 不写文件), 期望判定)
SHAPES = [
    ("skip-no-file", None, "SKIP"),
    ("skip-empty", "", "SKIP"),
    ("skip-no-signal", _TAG + "unrelated W line\n", "SKIP"),
    ("pass-retry-once", FAIL_LINE + "\n", "PASS"),
    ("pass-fail-then-cancelled", FAIL_LINE + "\n" + CANCEL_LINE + "\n", "PASS"),
    ("pass-fail-then-skipped", FAIL_LINE + "\n" + SKIP_LINE + "\n", "PASS"),
    ("pass-cancelled-partial", CANCEL_LINE + "\n", "PASS"),  # fail 行被缓冲冲掉
    ("fail-budget-breach", FAIL_LINE + "\n" + FAIL_LINE + "\n", "FAIL"),
]


def main() -> int:
    red = False
    for name, content, expected in SHAPES:
        run_dir = OUT / name
        evidence = run_dir / "device-evidence"
        evidence.mkdir(parents=True, exist_ok=True)
        log = evidence / "auto-retry.log"
        if content is None:
            if log.is_file():
                log.unlink()
        else:
            log.write_text(content, encoding="utf-8")
        got = auto_retry({"run_dir": run_dir})
        ok = got["status"] == expected
        red = red or not ok
        print(f"{'ok  ' if ok else 'MISMATCH'} {name:<24} "
              f"expect={expected:<4} got={got['status']:<4} {got['message'][:64]}")
    print("GREEN" if not red else "RED")
    return 1 if red else 0


if __name__ == "__main__":
    sys.exit(main())
