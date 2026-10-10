"""固定帧视觉校验器（numpy + pillow，仅视觉判定需要）。"""

from __future__ import annotations

import math
from pathlib import Path


def _load_sampled_rgb(path: Path, step: int = 4, region: dict | None = None):
    """返回 (采样像素, 坐标网格, 宽, 高)；网格带原始像素坐标，使质心/边界
    与历史 System.Drawing 实现一致。

    region (D22 §2.4 判据债): 出图面不是全屏时按 {"x","y","width","height"}
    (物理像素, 与 snapshot_display 全屏截图同坐标系) 先裁剪再分析 —— 裁剪后
    的宽高即后续判定器的相对阈值基准。"""
    import numpy as np
    from PIL import Image

    with Image.open(path) as image:
        rgb = np.asarray(image.convert("RGB"), dtype=np.uint8)
        width, height = image.size
    if region is not None:
        x = max(0, int(region["x"]))
        y = max(0, int(region["y"]))
        w = int(region["width"])
        h = int(region["height"])
        if w <= 0 or h <= 0 or x + w > width or y + h > height:
            raise ValueError(
                f"region 超出截图边界: region=({x},{y},{w},{h}) image=({width}x{height})")
        rgb = rgb[y:y + h, x:x + w]
        width, height = w, h
    xs = np.arange(0, width, step)
    ys = np.arange(0, height, step)
    xgrid, ygrid = np.meshgrid(xs, ys)
    return rgb[::step, ::step], xgrid, ygrid, width, height


def validate_rgba_quadrants(image_path: Path, step: int = 4,
                            region: dict | None = None) -> dict:
    """四色象限拓扑，旋转不变（rgba-quadrants-v1-rotations）。镜像或象限
    重复/缺失即 FAIL。"""
    import numpy as np

    pixels, xgrid, ygrid, width, height = _load_sampled_rgb(image_path, step,
                                                            region)
    r = pixels[..., 0].astype(np.int16)
    g = pixels[..., 1].astype(np.int16)
    b = pixels[..., 2].astype(np.int16)

    masks = {
        "red": (r > 170) & (g < 100) & (b < 120),
        "green": (g > 150) & (r < 120) & (b < 130),
        "blue": (b > 160) & (r < 130) & (g < 140),
        "yellow": (r > 170) & (g > 140) & (b < 120),
    }
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
        "region": dict(region) if region else None,
        "minimumSamplesPerColor": minimum,
        "detectedTransform": detected_transform,
        "quadrants": quadrants,
        "centroids": centroids,
    }


def validate_d3d11_cube(image_path: Path, step: int = 4,
                        region: dict | None = None) -> dict:
    """带深度/背景色差的彩色立方体（d3d11-cube-color-depth-v1）。"""
    import numpy as np

    pixels, xgrid, ygrid, width, height = _load_sampled_rgb(image_path, step,
                                                            region)
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


def _classify_pixels(image_path: Path, step: int = 8):
    """fusion 双窗定位共用: 全屏采样并按主色分类。
    返回 (分类掩码 dict[name->(xs,ys)], width, height)。分类阈值宽松
    (JPEG 压缩/缩放容差), 命中率比精确色值重要。"""
    import numpy as np

    samples, xgrid, ygrid, width, height = _load_sampled_rgb(
        image_path, step=step)
    r = samples[:, :, 0].astype(int)
    g = samples[:, :, 1].astype(int)
    b = samples[:, :, 2].astype(int)
    red = (r > 170) & (g < 110) & (b < 110)
    green = (g > 170) & (r < 110) & (b < 110)
    blue = (b > 170) & (r < 110) & (g < 110)
    white = (r > 210) & (g > 210) & (b > 210)
    # I2 (review): 品红 = 窗 B 左上象限专用 (窗 A 纯红主体会把 B 的纯红
    # 象限并进同一 bbox, 拓扑判定结构性误判 —— 品红与红空间可分)
    magenta = (r > 170) & (b > 170) & (g < 110)
    # 终验预演 (2026-10-10): 白色同病 —— 窗 A 的白十字与 B 的白象限跨窗
    # 并 bbox, 拓扑同样结构误判; B 右下象限改青 (与探针 paint_window_b
    # 成对改, 原则 25), cyan 与 green/blue 按 r 通道可分。
    cyan = (g > 170) & (b > 170) & (r < 110)
    return {
        "red": (xgrid[red], ygrid[red]),
        "green": (xgrid[green], ygrid[green]),
        "blue": (xgrid[blue], ygrid[blue]),
        "white": (xgrid[white], ygrid[white]),
        "magenta": (xgrid[magenta], ygrid[magenta]),
        "cyan": (xgrid[cyan], ygrid[cyan]),
        "total": (xgrid, ygrid),
    }, width, height


def _bbox(xs, ys):
    if len(xs) == 0:
        return None
    return {"x": int(xs.min()), "y": int(ys.min()),
            "w": int(xs.max() - xs.min()), "h": int(ys.max() - ys.min())}


def validate_fusion_window_a(image_path: Path, region: dict | None = None) -> dict:
    """M4a 窗 A: 全屏定位纯红大块 (fusion_probe 窗 A 的 GDI 输出), 再验证
    中心白十字。不依赖 region —— 双窗是独立 OHOS 窗, 屏幕位置由窗管决定,
    host 侧无 preview-rect 可裁 (与 §2.4 单画布场景的本质区别)。"""
    import numpy as np

    classes, width, height = _classify_pixels(image_path)
    rx, ry = classes["red"]
    box = _bbox(rx, ry)
    if box is None or box["w"] * box["h"] < 0.02 * width * height:
        return {"status": "FAIL", "validator": "fusion-window-a",
                "message": f"未定位到纯红大块 (red px={len(rx)}, bbox={box})"}

    # 红块内部红色覆盖率 (排除整屏红误定位: 十字与噪声稀释后仍应过半)
    area = box["w"] * box["h"]
    inside = (rx >= box["x"]) & (rx <= box["x"] + box["w"]) & \
             (ry >= box["y"]) & (ry <= box["y"] + box["h"])
    coverage = float(inside.sum()) / max(1, area / 64)  # step=8 → 每样本代表 64px²
    if coverage < 0.5:
        return {"status": "FAIL", "validator": "fusion-window-a",
                "message": f"红块覆盖率不足: {coverage:.2f} (bbox={box})"}

    # 中心白十字: 窗几何中心附近应有白色 (横竖臂交点)
    cx, cy = box["x"] + box["w"] // 2, box["y"] + box["h"] // 2
    wx, wy = classes["white"]
    near = (np.abs(wx - cx) < box["w"] // 6) & (np.abs(wy - cy) < box["h"] // 6)
    if int(near.sum()) < 3:
        return {"status": "FAIL", "validator": "fusion-window-a",
                "message": f"红块中心无白十字 (center=({cx},{cy}), white near={int(near.sum())})"}
    return {"status": "PASS", "validator": "fusion-window-a",
            "message": f"红窗+白十字定位通过 (bbox={box})",
            "metrics": {"bbox": box, "coverage": round(coverage, 2)}}


def validate_fusion_window_b(image_path: Path, region: dict | None = None) -> dict:
    """M4a 窗 B: 四象限拓扑自动定位版 —— 独立找出 M(品红)/G/B/C(青) 四块
    的 bbox, 验证空间拓扑 (G 在 M 右侧同排 / B 在 M 下侧同列 / C 在 B 右侧
    同排)。左上用品红不纯红 (I2): 窗 A 的纯红主体会与 B 的红象限并成
    一个 bbox, 纯红拓扑结构性误判 —— 品红与红空间可分。右下用青不用白
    (终验预演 2026-10-10): 窗 A 白十字与白象限跨窗并 bbox 同病, 青与
    green/blue 按 r 通道可分。"""
    classes, width, height = _classify_pixels(image_path)
    boxes = {name: _bbox(*classes[name])
             for name in ("magenta", "green", "blue", "cyan")}
    for name, box in boxes.items():
        if box is None or box["w"] * box["h"] < 0.01 * width * height:
            return {"status": "FAIL", "validator": "fusion-window-b",
                    "message": f"象限色 {name} 未定位到大块 (bbox={box})"}

    magenta, green = boxes["magenta"], boxes["green"]
    blue, white = boxes["blue"], boxes["cyan"]
    tol = 0.12 * height
    # G 在 M 右侧 (水平排), 顶部对齐
    if green["x"] < magenta["x"] + magenta["w"] * 0.5:
        return {"status": "FAIL", "validator": "fusion-window-b",
                "message": f"G 不在 M 右侧: magenta={magenta} green={green}"}
    if abs(int(green["y"]) - int(magenta["y"])) > tol:
        return {"status": "FAIL", "validator": "fusion-window-b",
                "message": f"G 与 M 顶部未对齐: magenta.y={magenta['y']} green.y={green['y']} (tol={tol:.0f})"}
    # B 在 M 下侧 (垂直排), 左对齐
    if blue["y"] < magenta["y"] + magenta["h"] * 0.5:
        return {"status": "FAIL", "validator": "fusion-window-b",
                "message": f"B 不在 M 下侧: magenta={magenta} blue={blue}"}
    if abs(int(blue["x"]) - int(magenta["x"])) > tol:
        return {"status": "FAIL", "validator": "fusion-window-b",
                "message": f"B 与 M 左侧未对齐: magenta.x={magenta['x']} blue.x={blue['x']}"}
    # W 在 B 右侧同排 (右下象限)
    if white["x"] < blue["x"] + blue["w"] * 0.5:
        return {"status": "FAIL", "validator": "fusion-window-b",
                "message": f"W 不在 B 右侧: blue={blue} white={white}"}
    if abs(int(white["y"]) - int(blue["y"])) > tol:
        return {"status": "FAIL", "validator": "fusion-window-b",
                "message": f"W 与 B 顶部未对齐: blue.y={blue['y']} white.y={white['y']}"}
    return {"status": "PASS", "validator": "fusion-window-b",
            "message": "四象限拓扑定位通过",
            "metrics": {"boxes": boxes}}


VALIDATORS.update({
    "fusion-window-a": validate_fusion_window_a,
    "fusion-window-b": validate_fusion_window_b,
})


def validate(name: str, image_path: Path, region: dict | None = None) -> dict:
    runner = VALIDATORS.get(name)
    if runner is None:
        return {"status": "FAIL", "validator": name,
                "message": f"未知视觉校验器: {name}（可用: {', '.join(sorted(VALIDATORS))}）"}
    try:
        return runner(image_path, region=region)
    except Exception as error:  # noqa: BLE001 - 校验器依赖缺失/图片损坏都要有明确结论
        return {"status": "FAIL", "validator": name,
                "message": f"视觉校验异常: {error}"}
