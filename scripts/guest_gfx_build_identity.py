#!/usr/bin/env python3
"""Record Mesa's actual build options and verify opt-in Zink bundle contents."""
import argparse
import hashlib
import json
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]


def mesa_identity(source=None):
    lock = json.loads((ROOT / 'docs/graphics/graphics-stack.lock.yaml').read_text())
    recipe = ROOT / 'scripts/apply_mesa_ohos_patches.sh'
    overlays = re.findall(r'\$SCRIPT_DIR/\.\./(patches/[^"\s]+\.patch)', recipe.read_text())
    if overlays != lock['mesa_production_overlays']:
        raise ValueError('Mesa production overlay order differs from graphics lock')
    expected = {row['path']: row['sha256'] for row in lock['managed_patches']['mesa']}
    rows = [{'path': name, 'sha256': digest(ROOT / name)} for name in overlays]
    if any(row['sha256'] != expected[row['path']] for row in rows):
        raise ValueError('Mesa production overlay content differs from graphics lock')
    files = lock['effective_files']['mesa']
    if source is not None and any(not (source / name).is_file() or digest(source / name) != value
                                  for name, value in files.items()):
        raise ValueError('Mesa source differs from the effective shadow transfer sources')
    return {'pin': lock['submodules']['mesa'], 'overlays': rows, 'effectiveFiles': files}


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("record", "verify-zink"))
    parser.add_argument("--install-root", type=Path, required=True)
    parser.add_argument("--build-root", type=Path)
    parser.add_argument("--source-root", type=Path)
    parser.add_argument("--arch", required=True)
    args = parser.parse_args()
    root = args.install_root
    manifest = root / "share/winehua/guest-gfx-build.json"
    if args.action == "record":
        if args.build_root is None or args.source_root is None:
            parser.error("record requires --build-root and --source-root")
        options = json.loads((args.build_root / "meson-info/intro-buildoptions.json").read_text())
        options = {entry["name"]: entry["value"] for entry in options}
        libraries = sorted({path for pattern in ("libgallium*.so", "libEGL.so*", "libGLES*.so*")
                            for path in (root / "lib").glob(pattern) if path.is_file()})
        if not any(path.name.startswith('libgallium') for path in libraries):
            parser.error("no installed Gallium driver library")
        data = {
            "schemaVersion": 3,
            "arch": args.arch,
            "galliumDrivers": options["gallium-drivers"],
            "platforms": options["platforms"],
            "opengl": options["opengl"],
            "libraries": {str(path.relative_to(root)): digest(path) for path in libraries},
            "mesaSource": mesa_identity(args.source_root),
        }
        manifest.parent.mkdir(parents=True, exist_ok=True)
        manifest.write_text(json.dumps(data, indent=2) + "\n")
    else:
        if not manifest.is_file():
            parser.error("Zink requires guest-gfx-build.json from build_ohos_guest_gfx.sh; rebuild Mesa")
        data = json.loads(manifest.read_text())
        if data.get("schemaVersion") != 3 or data.get("arch") != args.arch:
            parser.error("Mesa build identity has a different schema or architecture")
        if data.get('mesaSource') != mesa_identity():
            parser.error('Mesa build identity has stale sources or overlays; rebuild the graphics bundle')
        if "zink" not in data.get("galliumDrivers", []) or not data.get("opengl"):
            parser.error("Mesa was not built with the Zink OpenGL driver")
        libraries = data.get("libraries", {})
        if not libraries:
            parser.error("Mesa build identity has no Gallium library hashes")
        for family in ('libgallium', 'libEGL.so', 'libGLESv2.so'):
            if not any(Path(name).name.startswith(family) for name in libraries):
                parser.error("Incomplete Mesa EGL/Gallium build identity; rebuild the complete graphics bundle")
        for name, expected in libraries.items():
            path = (root / name).resolve()
            if not path.is_relative_to(root.resolve()) or not path.is_file() or digest(path) != expected:
                parser.error("Mesa library differs from the recorded Zink build: " + name)
        print("Verified Zink build identity and Gallium library hashes")


if __name__ == "__main__":
    main()
