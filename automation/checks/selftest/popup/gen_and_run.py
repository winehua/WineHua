#!/usr/bin/env python3
"""fusion-popup-menu 判定器合成自测 (M4c-T4)。本脚本入库为资产 (原在
build/ 下, M4c final review I2 迁入); 生成的形态图产物落在
build/popup-selftest/ (build/ gitignored, 每次运行重新生成)。
运行 (仓库根目录):
    python3 automation/checks/selftest/popup/gen_and_run.py
退出码: 全部形态判定符合期望 = 0 (GREEN), 任一 MISMATCH = 1 (RED)。
"""
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw

REPO = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(REPO / "automation"))

from checks.frame import validate  # noqa: E402

W, H = 2800, 1840  # 设备物理分辨率 (snapshot_display 同款)
BG = (24, 24, 24)      # OHOS 深色桌面
GREEN = (0, 255, 0)    # 探针锚点
MENU_BG = (255, 255, 255)
TEXT = (60, 60, 60)
BORDER = (160, 160, 160)

OUT = REPO / "build" / "popup-selftest"
OUT.mkdir(parents=True, exist_ok=True)


def draw_menu(img, x, y, w, h, rows, text_h=18, pitch=36):
    """wine 菜单形态 (真机 m4c-popup-1/m4c-oob-2 实测): 白底 + 细灰边 +
    暗色文字行 (行高 ~18px, 项距 ~36px)。rows=None = 空菜单。"""
    d = ImageDraw.Draw(img)
    d.rectangle([x, y, x + w, y + h], fill=MENU_BG, outline=BORDER, width=2)
    if rows:
        for i in range(rows):
            ty = y + 12 + i * pitch
            d.rectangle([x + 14, ty, x + 14 + 120, ty + text_h], fill=TEXT)
            if i == 0:  # 勾选列残留 (notepad 形态)
                d.rectangle([x + 3, ty + 4, x + 8, ty + 9], fill=TEXT)


def draw_button(img, x, y, w, h, label_rows=1):
    """屏上绿色 UI decoy (VKD3D 按钮形态): 绿底 + 白字。"""
    d = ImageDraw.Draw(img)
    d.rounded_rectangle([x, y, x + w, y + h], radius=12, fill=(76, 175, 80))
    for i in range(label_rows):
        ty = y + h // 2 - 10 + i * 24
        d.rectangle([x + 30, ty, x + w - 30, ty + 16], fill=(240, 240, 240))


def base(anchor=None):
    img = Image.new("RGB", (W, H), BG)
    if anchor:
        x, y, w, h = anchor
        ImageDraw.Draw(img).rectangle([x, y, x + w, y + h], fill=GREEN)
    return img


SHAPES = {}

# PASS 形态 ────────────────────────────────────────────────────────────────
img = base((200, 200, 1280, 960))
draw_menu(img, 280, 320, 208, 126, rows=3)  # 窗界内 (guest (140,160) ×2 邻域)
SHAPES["pass-inwindow"] = img

img = base((120, 100, 800, 240))
draw_menu(img, 128, 340, 208, 126, rows=3)  # 越界: 菜单悬挂窗底缘下
SHAPES["pass-oob"] = img

img = base((200, 200, 1280, 960))
draw_menu(img, 280, 320, 208, 126, rows=3)
draw_button(img, 300, 1250, 380, 70)        # 锚点下方绿色按钮白字 (不干扰)
SHAPES["pass-with-decoy"] = img

# FAIL 形态 ────────────────────────────────────────────────────────────────
SHAPES["fail-no-anchor"] = base()           # 探针窗缺失

SHAPES["fail-no-menu"] = base((200, 200, 1280, 960))  # 菜单未渲染 (纯绿窗)

img = base((200, 200, 1280, 960))
draw_menu(img, 280, 320, 208, 126, rows=0)  # 白块在场但零条目 (空菜单)
SHAPES["fail-empty-menu"] = img

img = base((200, 200, 1280, 960))
draw_menu(img, 280, 320, 208, 126, rows=1)  # 单行白块 = 按钮文字类误定位形态
SHAPES["fail-one-row"] = img

img = base((120, 100, 800, 240))            # oob 几何 + 菜单缺失
SHAPES["fail-oob-no-menu"] = img

img = base((200, 200, 1280, 960))           # 菜单底色缺失: 文字直接画在绿上
d = ImageDraw.Draw(img)
for i in range(3):
    ty = 330 + i * 36
    d.rectangle([294, ty, 414, ty + 18], fill=TEXT)
SHAPES["fail-menu-bg-missing"] = img

img = base((200, 200, 1280, 960))           # 只有 decoy (按钮白字), 无菜单
draw_button(img, 300, 1250, 380, 70)
SHAPES["fail-decoy-only"] = img

# PASS 形态 (真机像素): T3 notepad 菜单截图的真实菜单区域贴进绿锚点帧 ——
# 判据特征 (底色阈值/条目行距/勾选列) 对 wine 真实渲染成立。资产不在场时
# 优雅跳过 (其余 10 形态仍完整覆盖判据正反两面)。
REAL_SHOT = REPO / ".temp/m4c-popup-1.jpeg"
if REAL_SHOT.is_file():
    real = Image.open(REAL_SHOT).convert("RGB")
    img = base((200, 200, 1280, 960))
    # 真实菜单 bbox (实测: 文字 x 232-393, 条目带 y 111-169; 含边框取整块)
    img.paste(real.crop((219, 106, 437, 186)), (280, 320))
    SHAPES["pass-real-pixels"] = img
else:
    print(f"(skip pass-real-pixels: {REAL_SHOT} 不在场)")

EXPECTED = {
    "pass-inwindow": "PASS", "pass-oob": "PASS", "pass-with-decoy": "PASS",
    "fail-no-anchor": "FAIL", "fail-no-menu": "FAIL",
    "fail-empty-menu": "FAIL", "fail-one-row": "FAIL",
    "fail-oob-no-menu": "FAIL", "fail-menu-bg-missing": "FAIL",
    "fail-decoy-only": "FAIL",
}
if "pass-real-pixels" in SHAPES:
    EXPECTED["pass-real-pixels"] = "PASS"

results = []
ok = True
for name, img in SHAPES.items():
    path = OUT / f"{name}.jpg"
    img.save(path, quality=88)  # snapshot_display 同为 JPEG 有损
    report = validate("fusion-popup-menu", path)
    status = report["status"]
    match = status == EXPECTED[name]
    ok &= match
    results.append((name, EXPECTED[name], status, match,
                    report.get("message", "")))
    print(f"{name:24} expect={EXPECTED[name]:4} got={status:4} "
          f"{'OK' if match else 'MISMATCH'}  {report.get('message', '')}")

print()
print("SELFTEST", "GREEN" if ok else "RED",
      f"({sum(1 for r in results if r[3])}/{len(results)} shapes correct)")
sys.exit(0 if ok else 1)
