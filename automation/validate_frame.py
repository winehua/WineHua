#!/usr/bin/env python3
"""PIL/numpy frame validators for the WineHua regression suite.

Replaces the System.Drawing validators that previously ran inside the Windows
PowerShell orchestrator. Runs standalone so the same suite works from WSL pwsh.

Exit code 0 means PASS, 1 means FAIL. The machine-readable JSON is written to
--output when given, and printed to stdout otherwise.
"""

from __future__ import annotations

import argparse
import json
import math
import sys
from pathlib import Path

import numpy as np
from PIL import Image


def load_sampled_rgb(path: Path, step: int = 4) -> tuple[np.ndarray, np.ndarray, int, int]:
    """Return (sampled pixels, (x, y) coordinate grids, width, height).

    The grids carry original pixel coordinates (0, step, 2*step, ...) so
    centroids and bounds match the previous System.Drawing implementation.
    """
    with Image.open(path) as image:
        rgb = np.asarray(image.convert("RGB"), dtype=np.uint8)
        width, height = image.size
    xs = np.arange(0, width, step)
    ys = np.arange(0, height, step)
    xgrid, ygrid = np.meshgrid(xs, ys)
    return rgb[::step, ::step], xgrid, ygrid, width, height


def validate_rgba_quadrants(image_path: Path, step: int = 4) -> dict:
    """Four-colour quadrant topology, rotation-invariant (validator
    'rgba-quadrants-v1-rotations'). A reflection or duplicated/missing
    quadrant fails the visual gate."""
    pixels, xgrid, ygrid, width, height = load_sampled_rgb(image_path, step)
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

    # The OHOS presentation transform follows the display's native orientation.
    # A landscape snapshot can therefore contain a 90/180/270 degree rotation of
    # the canonical Vulkan framebuffer. Require the exact four-colour topology,
    # but accept rotations; a reflection or duplicated/missing quadrant still
    # fails the visual gate.
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
    """Coloured cube with depth/background variety (validator
    'd3d11-cube-color-depth-v1')."""
    pixels, xgrid, ygrid, width, height = load_sampled_rgb(image_path, step)
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


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--validator", required=True, choices=sorted(VALIDATORS))
    parser.add_argument("--image", required=True, type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()

    report = VALIDATORS[args.validator](args.image)
    encoded = json.dumps(report, sort_keys=True, indent=2) + "\n"
    if args.output:
        args.output.write_text(encoded, encoding="utf-8")
    print(encoded, end="")
    return 0 if report["status"] == "PASS" else 1


if __name__ == "__main__":
    sys.exit(main())
