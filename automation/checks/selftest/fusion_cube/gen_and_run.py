#!/usr/bin/env python3
"""fusion-cube 判定器合成自测 (MW-GPU 门禁)。按 popup/ 模板入库; 生成的
形态图产物落在 build/fusion-cube-selftest/ (build/ gitignored, 每次运行
重新生成)。
运行 (仓库根目录):
    python3 automation/checks/selftest/fusion_cube/gen_and_run.py
退出码: 全部形态判定符合期望 = 0 (GREEN), 任一 MISMATCH = 1 (RED)。
真机帧形态 (.temp/fusion-cube-1.jpeg) 不在场时优雅跳过。
"""
import math
import sys
from pathlib import Path

import numpy as np
from PIL import Image

REPO = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(REPO / "automation"))

from checks.frame import validate  # noqa: E402

W, H = 2800, 1840  # 设备物理分辨率 (snapshot_display 同款)
OUT = REPO / "build" / "fusion-cube-selftest"
OUT.mkdir(parents=True, exist_ok=True)


def draw_gradient_cube(arr, cx, cy, size, angle_deg):
    """旋转渐变立方体形态 (真机 shot-cube 同构): 方形四边形内 u/v 双轴
    渐变, 三通道错相保证三色桶活跃; 旋转后 bbox 四角留暗底。"""
    yy, xx = np.mgrid[0:arr.shape[0], 0:arr.shape[1]]
    dx = xx - cx
    dy = yy - cy
    a = math.radians(angle_deg)
    u = dx * math.cos(a) + dy * math.sin(a)
    v = -dx * math.sin(a) + dy * math.cos(a)
    half = size / 2.0
    inside = (np.abs(u) <= half) & (np.abs(v) <= half)
    tu = (u + half) / size
    tv = (v + half) / size
    r = np.clip(255 * tu, 0, 255)
    g = np.clip(255 * (1 - tu), 0, 255)
    b = np.clip(255 * tv, 0, 255)
    arr[inside] = np.stack([r[inside], g[inside], b[inside]], axis=-1).astype(np.uint8)


def base_screen():
    arr = np.full((H, W, 3), 24, dtype=np.uint8)  # OHOS 深色底
    # 左侧 UI 面板 decoy (环境页形态: 深底 + 彩色按钮列)
    arr[200:1500, 40:250] = (40, 40, 40)
    for i, color in enumerate([(76, 175, 80), (80, 80, 80), (33, 150, 243),
                               (198, 60, 60), (76, 175, 80), (130, 80, 180)]):
        y = 250 + i * 170
        arr[y:y + 90, 60:230] = color
    return arr


def save(arr, name):
    path = OUT / f"{name}.jpeg"
    Image.fromarray(arr).save(path, quality=85)
    return path


def run(name, path, expect, forms):
    report = validate("fusion-cube", path)
    status = report["status"]
    forms.append((name, expect, status, report.get("message", "")))


def main():
    forms = []

    # 1) 合成正例: 窗 (暗底) + 渐变立方体 (960 窗内 ~640 立方, 真机同量级)
    arr = base_screen()
    arr[60:820, 860:1860] = (24, 26, 30)  # 立方体宿主窗深底
    draw_gradient_cube(arr, cx=1360, cy=440, size=620, angle_deg=30)
    run("synthetic-cube", save(arr, "synthetic-cube"), "PASS", forms)

    # 2) 反例: 小彩块 (marker 量级) —— 绝对尺寸门应拦下
    arr = base_screen()
    arr[100:200, 900:1060] = (40, 80, 200)
    run("small-cube", save(arr, "small-cube"), "FAIL", forms)

    # 3) 反例: 诱饵屏 —— 只有 UI 按钮/彩色块, 无立方体
    arr = base_screen()
    for i, color in enumerate([(220, 60, 60), (60, 200, 60), (60, 60, 220),
                               (200, 200, 60), (200, 60, 200)]):
        x = 700 + i * 380
        arr[900:1300, x:x + 260] = color
    run("decoy-blocks", save(arr, "decoy-blocks"), "FAIL", forms)

    # 4) 反例: 窗在但空白 (全黑内容) —— 无彩色样本
    arr = base_screen()
    arr[60:820, 860:1860] = (0, 0, 0)
    run("blank-window", save(arr, "blank-window"), "FAIL", forms)

    # 5) 真机帧正例 (可选资产, 不在场优雅跳过)
    real = REPO / ".temp" / "fusion-cube-1.jpeg"
    if real.exists():
        run("real-device-frame", real, "PASS", forms)
    else:
        print("[skip] 真机帧 .temp/fusion-cube-1.jpeg 不在场 (优雅跳过)")

    failed = 0
    for name, expect, status, message in forms:
        ok = status == expect
        failed += 0 if ok else 1
        mark = "ok  " if ok else "MISMATCH"
        print(f"[{mark}] {name}: expect={expect} got={status} {message[:80]}")
    print(f"\n{len(forms)} forms, {failed} mismatch")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
