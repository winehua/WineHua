"""固定帧视觉校验器（numpy + pillow，仅视觉判定需要）。"""

from __future__ import annotations

import math
from pathlib import Path


def _load_sampled_rgb(path: Path, step: int = 4):
    """返回 (采样像素, 坐标网格, 宽, 高)；网格带原始像素坐标，使质心/边界
    与历史 System.Drawing 实现一致。"""
    import numpy as np
    from PIL import Image

    with Image.open(path) as image:
        rgb = np.asarray(image.convert("RGB"), dtype=np.uint8)
        width, height = image.size
    xs = np.arange(0, width, step)
    ys = np.arange(0, height, step)
    xgrid, ygrid = np.meshgrid(xs, ys)
    return rgb[::step, ::step], xgrid, ygrid, width, height


def validate_rgba_quadrants(image_path: Path, step: int = 4) -> dict:
    """四色象限拓扑，旋转不变（rgba-quadrants-v1-rotations）。镜像或象限
    重复/缺失即 FAIL。"""
    import numpy as np

    pixels, xgrid, ygrid, width, height = _load_sampled_rgb(image_path, step)
    r = pixels[..., 0].astype(np.int16)
    g = pixels[..., 1].astype(np.int16)
    b = pixels[..., 2].astype(np.int16)

    masks = {
        "red": (r > 170) & (g < 100) & (b < 120),
        "green": (g > 150) & (r < 120) & (b < 130),
        "blue": (b > 160) & (r < 130) & (g < 140),
        "yellow": (r > 170) & (g > 140) & (b < 120),
    }
    # The Wine desktop itself is blue and can dwarf the blue test quadrant.
    # Red, green and yellow do not collide with the desktop; together they
    # span both axes of every accepted rotation and define the test frame ROI.
    anchor = masks["red"] | masks["green"] | masks["yellow"]
    if anchor.any():
        roi_min_x = int(xgrid[anchor].min())
        roi_max_x = int(xgrid[anchor].max())
        roi_min_y = int(ygrid[anchor].min())
        roi_max_y = int(ygrid[anchor].max())
        roi = ((xgrid >= roi_min_x) & (xgrid <= roi_max_x) &
               (ygrid >= roi_min_y) & (ygrid <= roi_max_y))
        masks = {name: mask & roi for name, mask in masks.items()}
    else:
        roi_min_x = roi_min_y = roi_max_x = roi_max_y = -1
    sample_count = math.ceil(width / step) * math.ceil(height / step)
    minimum = max(80, int(sample_count * 0.003))

    centroids = {}
    enough = True
    for name, mask in masks.items():
        count = int(mask.sum())
        if count < minimum:
            enough = False
        centroids[name] = {
            "count": count,
            "x": float(xgrid[mask].mean()) if count else -1.0,
            "y": float(ygrid[mask].mean()) if count else -1.0,
        }

    # OHOS 呈现变换跟随屏幕原生方向：横屏截图可能是规范帧的 90/180/270 度旋转。
    # 要求精确的四色拓扑但接受旋转；镜像/重复/缺失仍 FAIL。
    center_x = sum(centroids[name]["x"] for name in masks) / 4.0
    center_y = sum(centroids[name]["y"] for name in masks) / 4.0
    quadrants = {}
    for name in masks:
        column = "L" if centroids[name]["x"] < center_x else "R"
        row = "T" if centroids[name]["y"] < center_y else "B"
        quadrants[f"{row}{column}"] = name

    layouts = [
        {"name": "identity", "TL": "red", "TR": "green", "BL": "blue", "BR": "yellow"},
        {"name": "rotate90", "TL": "blue", "TR": "red", "BL": "yellow", "BR": "green"},
        {"name": "rotate180", "TL": "yellow", "TR": "blue", "BL": "green", "BR": "red"},
        {"name": "rotate270", "TL": "green", "TR": "yellow", "BL": "red", "BR": "blue"},
    ]
    detected_transform = None
    if len(quadrants) == 4:
        for layout in layouts:
            if all(quadrants.get(key) == layout[key] for key in ("TL", "TR", "BL", "BR")):
                detected_transform = layout["name"]
                break

    x_values = [centroids[name]["x"] for name in masks]
    y_values = [centroids[name]["y"] for name in masks]
    separated_columns = (max(x_values) - min(x_values)) > (width * 0.08)
    separated_rows = (max(y_values) - min(y_values)) > (height * 0.08)
    passed = bool(enough and separated_columns and separated_rows and detected_transform)

    return {
        "schemaVersion": 1,
        "status": "PASS" if passed else "FAIL",
        "validator": "rgba-quadrants-v1-rotations",
        "image": str(image_path),
        "width": width,
        "height": height,
        "minimumSamplesPerColor": minimum,
        "regionOfInterest": {
            "x": roi_min_x,
            "y": roi_min_y,
            "width": roi_max_x - roi_min_x + 1 if roi_min_x >= 0 else 0,
            "height": roi_max_y - roi_min_y + 1 if roi_min_y >= 0 else 0,
        },
        "detectedTransform": detected_transform,
        "quadrants": quadrants,
        "centroids": centroids,
    }


def validate_d3d11_cube(image_path: Path, step: int = 4) -> dict:
    """带深度/背景色差的彩色立方体（d3d11-cube-color-depth-v1）。"""
    import numpy as np

    pixels, xgrid, ygrid, width, height = _load_sampled_rgb(image_path, step)
    r = pixels[..., 0].astype(np.int16)
    g = pixels[..., 1].astype(np.int16)
    b = pixels[..., 2].astype(np.int16)
    maximum = np.maximum(np.maximum(r, g), b)
    minimum = np.minimum(np.minimum(r, g), b)

    dark = maximum < 55
    colored = (maximum > 100) & ((maximum - minimum) > 55)
    buckets = {
        "red": (colored & (r == maximum)).sum(),
        "green": (colored & (r != maximum) & (g == maximum)).sum(),
        "blue": (colored & (r != maximum) & (g != maximum)).sum(),
    }
    sample_count = math.ceil(width / step) * math.ceil(height / step)
    minimum_colored = max(500, int(sample_count * 0.005))
    active_buckets = sum(1 for value in buckets.values() if value > minimum_colored * 0.08)

    colored_count = int(colored.sum())
    if colored_count:
        min_x = int(xgrid[colored].min())
        max_x = int(xgrid[colored].max())
        min_y = int(ygrid[colored].min())
        max_y = int(ygrid[colored].max())
        box_width, box_height = max_x - min_x + 1, max_y - min_y + 1
    else:
        min_x = min_y = max_x = max_y = -1
        box_width = box_height = 0
    dark_count = int(dark.sum())

    passed = bool(
        colored_count >= minimum_colored
        and active_buckets >= 3
        and box_width > (width * 0.08)
        and box_height > (height * 0.08)
        and dark_count > (sample_count * 0.03)
    )

    return {
        "schemaVersion": 1,
        "status": "PASS" if passed else "FAIL",
        "validator": "d3d11-cube-color-depth-v1",
        "image": str(image_path),
        "width": width,
        "height": height,
        "coloredSamples": colored_count,
        "minimumColoredSamples": minimum_colored,
        "darkSamples": dark_count,
        "activeColorBuckets": active_buckets,
        "colorBuckets": {name: int(value) for name, value in buckets.items()},
        "coloredBounds": {"x": min_x, "y": min_y, "width": box_width, "height": box_height},
    }


VALIDATORS = {
    "rgba-quadrants": validate_rgba_quadrants,
    "d3d11-cube": validate_d3d11_cube,
}


def validate(name: str, image_path: Path) -> dict:
    runner = VALIDATORS.get(name)
    if runner is None:
        return {"status": "FAIL", "validator": name,
                "message": f"未知视觉校验器: {name}（可用: {', '.join(sorted(VALIDATORS))}）"}
    try:
        return runner(image_path)
    except Exception as error:  # noqa: BLE001 - 校验器依赖缺失/图片损坏都要有明确结论
        return {"status": "FAIL", "validator": name,
                "message": f"视觉校验异常: {error}"}
