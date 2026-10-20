#!/usr/bin/env python3
"""presented-route 判定器合成自测（D49 归属修正，2026-10-11）。

不依赖设备：合成归档形态（结果 metrics + device-evidence/guest-frames.log），
跑判定器，断言「期望判定 = 实际判定」。产物落在
build/presented-route-selftest/（build/ gitignored，每次运行重新生成 ——
rm -rf build/ 后从零复跑即毁灭性重建演练）。
运行 (仓库根目录):
    python3 automation/checks/selftest/presented_route/gen_and_run.py
退出码: 全部形态符合期望 = 0 (GREEN), 任一 MISMATCH = 1 (RED)。

形态口径（key = (unix pid<<32)|xwin）：
- KEY64: x64 直承进程（key pid == spawn pid，快捷 PASS 路径）
- KEY86: x86 二次 NCP spawn 的承载进程（key pid ≠ spawn pid，帧 key 同一性路径）
- KEY_OTHER: 与本次无关窗口（无匹配 ledger 行 ⇒ 证据缺口 FAIL）
"""
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(REPO / "automation"))

from checks import presented_route  # noqa: E402

OUT = REPO / "build" / "presented-route-selftest"

KEY64 = (34189 << 32) | 16777219   # x64: 窗口 pid == spawn pid
KEY86 = (34486 << 32) | 18874371   # x86: 窗口 pid = spawn pid + 12（启动器之后）
KEY_OTHER = (77777 << 32) | 12345  # 无关窗口（用于证据缺口形态）
KEY_NOWIN = (99999 << 32) | 54321  # presentedKey 有值但 ledger 无记录


def base_metrics(**over) -> dict:
    m = {"expectedRoute": "x11", "presentedRoute": "x11", "declaredRoute": "-",
         "displayStallMs": 300, "presentedKey": KEY86,
         "width": 960, "height": 540}
    m.update(over)
    return m


def attach(key: int, pid: int, src: str = "960x540") -> str:
    return ("10-11 20:48:19.880 31056 31842 I A00000/app.hackeris.winehua/"
            "guest-frames: [GUEST-FRAMES] attach key=%d "
            "xwin=18874371 pid=%d src=%s win=968x574@0,0 kind=gl flip=1"
            % (key, pid, src))


def detach(key: int, pid: int) -> str:
    # 真机 detach 行无 src（tail -50 截断后只剩它时走几何豁免路径）
    return ("10-11 20:48:27.753 31056 31842 I A00000/app.hackeris.winehua/"
            "guest-frames: [GUEST-FRAMES] detach key=%d "
            "xwin=18874371 pid=%d frames=597 reason=xwindow destroyed"
            % (key, pid))


# (名称, result metrics (None=无 result 文件), spawn pid, 日志行列表 (None=缺文件), 期望)
SHAPES = [
    ("pass-direct-pid", base_metrics(presentedKey=KEY64), 34189, None, "PASS"),
    ("pass-key-attach", base_metrics(), 34474, [attach(KEY86, 34486)], "PASS"),
    ("pass-key-detach-only", base_metrics(), 34474, [detach(KEY86, 34486)], "PASS"),
    ("fail-geom-mismatch", base_metrics(), 34474,
     [attach(KEY86, 34486, src="1280x720")], "FAIL"),
    ("fail-no-evidence", base_metrics(presentedKey=KEY_NOWIN), 34474,
     [attach(KEY_OTHER, 77777)], "FAIL"),
    ("pass-spawn-attach-fallback", base_metrics(presentedKey=KEY_NOWIN), 34189,
     [attach(KEY64, 34189)], "PASS"),
    ("pass-wayland", base_metrics(expectedRoute="wayland",
                                  presentedRoute="wayland", presentedKey=0),
     34189, None, "PASS"),
    ("fail-route-drift", base_metrics(presentedRoute="wayland"), 34474, None, "FAIL"),
    ("fail-key-zero", base_metrics(presentedKey=0), 34474,
     [attach(KEY86, 34486)], "FAIL"),
    ("fail-stall", base_metrics(displayStallMs=3000), 34474, None, "FAIL"),
    ("fail-missing-fields", None, 34474, None, "FAIL"),
    ("fail-declared", base_metrics(declaredRoute="wayland"), 34474, None, "FAIL"),
    ("pass-selfpid-bridge", base_metrics(selfPid=34486, selfPidSource="unix-bridge"),
     34474, None, "PASS"),
    ("fail-selfpid-bridge", base_metrics(selfPid=35555, selfPidSource="unix-bridge"),
     34474, None, "FAIL"),
    ("pass-selfpid-untrusted", base_metrics(selfPid=35555, selfPidSource="wine-ptid"),
     34474, [attach(KEY86, 34486)], "PASS"),
]


def main() -> int:
    red = False
    for name, metrics, spawn_pid, log_lines, expected in SHAPES:
        run_dir = OUT / name
        evidence = run_dir / "device-evidence"
        evidence.mkdir(parents=True, exist_ok=True)
        log = evidence / "guest-frames.log"
        if log_lines is None:
            if log.is_file():
                log.unlink()
        else:
            log.write_text("\n".join(log_lines) + "\n", encoding="utf-8")
        ctx = {"run_dir": run_dir, "device": {"pid": spawn_pid}}
        if metrics is not None:
            ctx["result"] = {"metrics": metrics}
        got = presented_route(ctx)
        ok = got["status"] == expected
        red = red or not ok
        print(f"{'ok  ' if ok else 'MISMATCH'} {name:<28} "
              f"expect={expected:<4} got={got['status']:<4} {got['message'][:64]}")
    print("GREEN" if not red else "RED")
    return 1 if red else 0


if __name__ == "__main__":
    sys.exit(main())
