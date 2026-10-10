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


def _classify_masks(image_path: Path, step: int = 8):
    """全屏采样并按主色分类的 2D 掩码形态（阈值单源在这里；
    _classify_pixels 与 fusion-popup-menu 的连通分量定位都引用它）。
    分类阈值宽松 (JPEG 压缩/缩放容差), 命中率比精确色值重要。"""
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
        "red": red, "green": green, "blue": blue, "white": white,
        "magenta": magenta, "cyan": cyan,
    }, xgrid, ygrid, width, height


def _classify_pixels(image_path: Path, step: int = 8):
    """fusion 双窗定位共用: 全屏采样并按主色分类。
    返回 (分类掩码 dict[name->(xs,ys)], width, height)。"""
    masks, xgrid, ygrid, width, height = _classify_masks(image_path, step)
    coords = {name: (xgrid[m], ygrid[m]) for name, m in masks.items()}
    coords["total"] = (xgrid, ygrid)
    return coords, width, height


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
    m_xs, m_ys = classes["magenta"]
    m_box = _bbox(m_xs, m_ys)
    if m_box is None or m_box["w"] * m_box["h"] < 0.01 * width * height:
        return {"status": "FAIL", "validator": "fusion-window-b",
                "message": f"象限色 magenta 未定位到大块 (bbox={m_box})"}
    # 锚定过滤 (真机 171703 实测): 全屏帧里蓝/青类会被屏幕上其它蓝色 UI
    # (键盘按钮等) 拉爆 bbox —— 以品红象限为锚, G/B/C 只取 M 邻域
    # (右/下/右下三个方向, 容差 tol) 内的样本再取 bbox。拓扑检查本身
    # 验证的就是这些方向关系, 过滤与判据同源, 不引入新假设。
    tol = 0.12 * height
    mx0, mx1 = m_box["x"], m_box["x"] + m_box["w"]
    my0, my1 = m_box["y"], m_box["y"] + m_box["h"]

    def _bbox_near(xs, ys, x0, x1, y0, y1):
        keep = (xs >= x0) & (xs <= x1) & (ys >= y0) & (ys <= y1)
        return _bbox(xs[keep], ys[keep])

    green = _bbox_near(classes["green"][0], classes["green"][1],
                       mx1 - tol, mx1 + 2 * m_box["w"] + tol,
                       my0 - tol, my1 + tol)
    blue = _bbox_near(classes["blue"][0], classes["blue"][1],
                      mx0 - tol, mx1 + tol,
                      my1 - tol, my1 + 2 * m_box["h"] + tol)
    boxes = {"magenta": m_box, "green": green, "blue": blue}
    for name, box in boxes.items():
        if box is None or box["w"] * box["h"] < 0.01 * width * height:
            return {"status": "FAIL", "validator": "fusion-window-b",
                    "message": f"象限色 {name} 未定位到大块 (bbox={box})"}
    # C(青) 在 B 右侧: 以过滤后的 B 为锚
    bx0, bx1 = blue["x"], blue["x"] + blue["w"]
    by0, by1 = blue["y"], blue["y"] + blue["h"]
    white = _bbox_near(classes["cyan"][0], classes["cyan"][1],
                       bx1 - tol, bx1 + 2 * blue["w"] + tol,
                       by0 - tol, by1 + tol)
    boxes["cyan"] = white
    if white is None or white["w"] * white["h"] < 0.01 * width * height:
        return {"status": "FAIL", "validator": "fusion-window-b",
                "message": f"象限色 cyan 未定位到大块 (bbox={white})"}

    magenta = m_box
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


def _dense_band(values, gap: int = 16):
    """一维坐标的最宽「密集连续段」(lo, hi)。密集 = 该坐标上的样本计数
    ≥ max(3, 峰值×0.25)。散点 UI (按钮/图标/文字) 计数远低于窗体行/列,
    进不了带 —— fusion-popup 的绿锚点定位用它, 不被屏上零散绿色元素
    (VKD3D 按钮/文件夹图标) 拉爆 bbox (fusion-window-b 锚定过滤同思路,
    但按钮可能与锚点同排, 单靠邻域过滤不够, 按密度取主矩形)。gap = 相邻
    密集坐标的最大空隙 (采样格单位), 容 JPEG 噪声断点。"""
    import numpy as np

    if len(values) == 0:
        return None
    uniq, counts = np.unique(values, return_counts=True)
    threshold = max(3, int(counts.max() * 0.25))
    dense = uniq[counts >= threshold]
    if len(dense) == 0:
        return None
    best_lo = lo = int(dense[0])
    best_hi = hi = int(dense[0])
    for value in dense[1:]:
        if int(value) - hi <= gap:
            hi = int(value)
        else:
            if hi - lo > best_hi - best_lo:
                best_lo, best_hi = lo, hi
            lo = hi = int(value)
    if hi - lo > best_hi - best_lo:
        best_lo, best_hi = lo, hi
    return best_lo, best_hi


def _largest_component_bbox(mask, cell: int = 1):
    """粗网格布尔掩码的最大 4-连通分量 bbox (原始像素坐标, cell = 网格
    步长)。菜单定位用它而不是全体白像素的 bbox: 搜索区里其它白色元素
    (按钮白字/状态文字) 会把 bbox 并成跨矩形大框, 底色占比判定整段失真
    (合成自测 pass-with-decoy 形态实锤)。网格 ~80k 格, BFS 开销可忽略。"""
    from collections import deque

    import numpy as np

    active = np.argwhere(mask)
    if len(active) == 0:
        return None, 0
    seen = np.zeros(mask.shape, dtype=bool)
    best_box = None
    best_size = 0
    for sy, sx in active:
        if seen[sy, sx]:
            continue
        seen[sy, sx] = True
        queue = deque([(sy, sx)])
        ys0 = ys1 = sy
        xs0 = xs1 = sx
        size = 0
        while queue:
            y, x = queue.popleft()
            size += 1
            ys0, ys1 = min(ys0, y), max(ys1, y)
            xs0, xs1 = min(xs0, x), max(xs1, x)
            for ny, nx in ((y - 1, x), (y + 1, x), (y, x - 1), (y, x + 1)):
                if (0 <= ny < mask.shape[0] and 0 <= nx < mask.shape[1]
                        and mask[ny, nx] and not seen[ny, nx]):
                    seen[ny, nx] = True
                    queue.append((ny, nx))
        if size > best_size:
            best_size = size
            best_box = {"x": int(xs0) * cell, "y": int(ys0) * cell,
                        "w": int(xs1 - xs0) * cell, "h": int(ys1 - ys0) * cell}
    return best_box, best_size


def validate_fusion_popup_menu(image_path: Path, region: dict | None = None) -> dict:
    """M4c popup 菜单帧内容判定（fusion-popup / fusion-popup-oob 探针）。

    场景: fusion_popup 探针单窗纯绿锚点（WS_POPUP 无非客户区, 客户区全绿,
    不画任何白色 —— 白块唯一来源是菜单），TrackPopupMenu 程序化弹菜单，
    OR 菜单窗以独立 OHOS 子窗呈现在锚点窗内（fusion-popup）或底缘下
    （fusion-popup-oob）。判据三段：
      1. 绿锚点窗定位（主色分类器 green）+ 密度带取主矩形；
      2. 锚点邻域白块 = 菜单底色（真机 m4c-popup-1 / m4c-oob-2 实测
         menu bg RGB≥243；白块在锚点 bbox 内或正下方 ≤1.5 倍锚点高 ——
         oob 形态菜单整条越出窗底缘）；
      3. 白块内暗色文字条带 ≥2（条目结构）。细采样 step=2 —— 菜单文字
         笔画在 2800px 整屏帧里约 2px 宽，粗采样 (step=8) 会整行漏掉
         （真机实测条目行高 17-19px、项距 ~36px）。

    只判渲染呈现（已验证域）。点选（WM_COMMAND）不在本判定范围 —— T3
    越界点选悬案移交 M4d（checks 侧由 fusion-popup-selection 出 SKIP
    标记，不做成会假 PASS 的判据）。"""
    import numpy as np

    masks, xgrid, ygrid, width, height = _classify_masks(image_path)
    gx, gy = xgrid[masks["green"]], ygrid[masks["green"]]
    band_x = _dense_band(gx)
    band_y = _dense_band(gy)
    if band_x is None or band_y is None:
        return {"status": "FAIL", "validator": "fusion-popup-menu",
                "message": "绿锚点未定位 (探针窗缺失或被遮挡)"}
    ax0, ax1 = band_x
    ay0, ay1 = band_y
    if (ax1 - ax0) * (ay1 - ay0) < 0.01 * width * height:
        return {"status": "FAIL", "validator": "fusion-popup-menu",
                "message": f"绿锚点过小 ({ax1 - ax0}x{ay1 - ay0}, "
                           f"screen={width}x{height}) —— 探针窗未呈现?"}

    # 菜单搜索区: 锚点 x 邻域 ±1/4 宽; y 从锚点顶到其下 1.6 倍高 (oob 悬挂)。
    # 白块 = 区内最大白色连通分量 (不是全体白像素 bbox: 按钮白字等散点白
    # 会把 bbox 并成跨矩形大框, 底色占比判定失真 —— 合成自测
    # pass-with-decoy 形态实锤)。
    rx0 = max(0, ax0 - (ax1 - ax0) // 4)
    rx1 = min(width - 1, ax1 + (ax1 - ax0) // 4)
    ry0 = max(0, ay0 - (ay1 - ay0) // 10)
    ry1 = min(height - 1, ay1 + (ay1 - ay0) * 3 // 2)
    region_white = masks["white"] & (xgrid >= rx0) & (xgrid <= rx1) & \
                   (ygrid >= ry0) & (ygrid <= ry1)
    menu, comp = _largest_component_bbox(region_white, cell=8)
    if menu is None or comp < 40:
        return {"status": "FAIL", "validator": "fusion-popup-menu",
                "message": f"锚点邻域无菜单白块 (largest={comp} samples, "
                           f"region=({rx0},{ry0})-({rx1},{ry1})) —— 菜单未渲染"}
    aw, ah = ax1 - ax0, ay1 - ay0
    if menu["w"] > aw * 1.2 or menu["h"] > ah * 1.2:
        return {"status": "FAIL", "validator": "fusion-popup-menu",
                "message": f"白块尺寸超出菜单形态 ({menu} vs 锚点 {aw}x{ah})"}
    if menu["w"] < 24 or menu["h"] < 16:
        return {"status": "FAIL", "validator": "fusion-popup-menu",
                "message": f"白块过小不成菜单 ({menu})"}

    # 细采样: 菜单 bbox (pad 4px) 内找暗色文字条带
    fx0 = max(0, menu["x"] - 4)
    fy0 = max(0, menu["y"] - 4)
    fine_region = {"x": fx0, "y": fy0,
                   "width": min(width - fx0, menu["w"] + 8),
                   "height": min(height - fy0, menu["h"] + 8)}
    samples, fxg, fyg, _, _ = _load_sampled_rgb(image_path, step=2,
                                                region=fine_region)
    r = samples[:, :, 0].astype(int)
    g = samples[:, :, 1].astype(int)
    b = samples[:, :, 2].astype(int)
    dark = (r < 120) & (g < 120) & (b < 120)
    white = (r > 210) & (g > 210) & (b > 210)  # 同 _classify_masks 白阈值
    white_ratio = float(white.sum()) / max(1, white.size)
    if white_ratio < 0.35:
        return {"status": "FAIL", "validator": "fusion-popup-menu",
                "message": f"菜单底色占比不足 ({white_ratio:.2f} < 0.35) —— "
                           f"白块不是菜单 (按钮文字类小面积白)"}
    rows = dark.sum(axis=1)
    text_rows = np.nonzero(rows >= 2)[0]
    if len(text_rows) == 0:
        return {"status": "FAIL", "validator": "fusion-popup-menu",
                "message": "菜单白块内无文字 —— 条目结构缺失 (空菜单?)"}
    # 行聚类成条带 (间隙 ≤3 个采样格 = 6px; 真机项距 ~36px 远大于此)
    bands = []
    start = prev = int(text_rows[0])
    for y in text_rows[1:]:
        y = int(y)
        if y - prev <= 3:
            prev = y
            continue
        bands.append((start, prev))
        start = prev = y
    bands.append((start, prev))
    bands = [band for band in bands if band[1] - band[0] >= 2]  # ≥4px 行高
    if len(bands) < 2:
        return {"status": "FAIL", "validator": "fusion-popup-menu",
                "message": f"文字条带 {len(bands)} < 2 —— 条目结构缺失 "
                           f"(探针菜单固定 3 项; 单行白 = 按钮文字类误定位)"}
    return {"status": "PASS", "validator": "fusion-popup-menu",
            "message": f"菜单底色+条目结构定位通过 ({len(bands)} 条目行, "
                       f"menu={menu})",
            "metrics": {"anchor": {"x": ax0, "y": ay0, "w": aw, "h": ah},
                        "menu": menu,
                        "textBands": len(bands),
                        "whiteRatio": round(white_ratio, 2)}}


VALIDATORS.update({
    "fusion-window-a": validate_fusion_window_a,
    "fusion-window-b": validate_fusion_window_b,
    "fusion-popup-menu": validate_fusion_popup_menu,
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
