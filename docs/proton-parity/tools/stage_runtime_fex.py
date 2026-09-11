#!/usr/bin/env python3
"""实验工具: 就地更新 rawfile 运行时包里的 FEX 内容 (不重跑 assemble.sh)。

用途 (见 docs/proton-parity/p2-device-validation.md):
  在已有 `entry/src/main/resources/rawfile/wine-data.zip` 上做最小替换,
  供随后用 hvigor 正式出包:
    1. 替换 bin/aarch64-windows/{libarm64ecfex,libwow64fex}.dll
    2. 新增 share/fex-emu/Config.json
    3. 同步更新 wine-runtime-manifest.json 的 payloadSha256

正式路径仍然是 `make NATIVE_ARCH=arm64-v8a hap` (assemble.sh 自己会做这些)。
本工具只是让"改了 FEX 但没复制完整 build/ 目录"时也能出包。

用法:
  stage_runtime_fex.py RAWFILE_DIR FEX_EC.dll FEX_WOW.dll CONFIG.json
"""

import hashlib
import io
import json
import os
import sys
import zipfile

INNER = "wine-data.zip"
MANIFEST = "wine-runtime-manifest.json"
PE_DIR = "bin/aarch64-windows"
CONFIG_PATH = "share/fex-emu/Config.json"


def main():
    if len(sys.argv) != 5:
        sys.exit(__doc__)
    rawfile, dll_ec, dll_wow, config = sys.argv[1:]
    inner_path = os.path.join(rawfile, INNER)

    for path in (inner_path, dll_ec, dll_wow, config):
        if not os.path.isfile(path):
            sys.exit(f"missing input: {path}")

    src = zipfile.ZipFile(inner_path)
    replacements = {
        f"{PE_DIR}/libarm64ecfex.dll": dll_ec,
        f"{PE_DIR}/libwow64fex.dll": dll_wow,
        CONFIG_PATH: config,
    }
    entries = len(src.infolist())

    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w", zipfile.ZIP_DEFLATED) as dst:
        for info in src.infolist():
            path = replacements.pop(info.filename, None)
            data = open(path, "rb").read() if path else src.read(info.filename)
            zi = zipfile.ZipInfo(info.filename, date_time=info.date_time)
            zi.compress_type = info.compress_type
            zi.external_attr = info.external_attr
            dst.writestr(zi, data)
        for name, path in replacements.items():
            dst.writestr(name, open(path, "rb").read())
            print(f"  + {name}")
    src.close()

    new_bytes = buf.getvalue()
    with open(inner_path, "wb") as f:
        f.write(new_bytes)
    payload_sha = hashlib.sha256(new_bytes).hexdigest()

    manifest_path = os.path.join(rawfile, MANIFEST)
    with open(manifest_path, encoding="utf-8") as f:
        manifest = json.load(f)
    print(f"payloadSha256 {manifest.get('payloadSha256')} -> {payload_sha}")
    manifest["payloadSha256"] = payload_sha
    with open(manifest_path, "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=2, ensure_ascii=False)

    print(f"{INNER}: {len(new_bytes)} bytes, {entries} original entries")


if __name__ == "__main__":
    main()
