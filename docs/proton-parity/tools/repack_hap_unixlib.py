#!/usr/bin/env python3
"""实验工具: 把新版 FEX 产物注入已有 HAP (不跑完整构建链)。

用途 (见 docs/proton-parity/p2-device-validation.md):
  在已有 HAP 基础上做最小替换, 用于真机验证 UnixLib 是否被 wine 加载:
    1. 替换 resources/rawfile/wine-data.zip 内的
       bin/aarch64-windows/{libarm64ecfex,libwow64fex}.dll
    2. 同步更新 resources/rawfile/wine-runtime-manifest.json 的 payloadSha256
    3. 追加 libs/arm64-v8a/{libarm64ecfex,libwow64fex}.so

这不是产品构建入口。正式出包仍然走 `make NATIVE_ARCH=arm64-v8a hap`。
产物为**未签名** HAP, 之后需用 scripts/package.sh 同款 sign.py 签名。

用法:
  repack_hap_unixlib.py BASE.hap FEX_EC.dll FEX_WOW.dll UNIXLIB_EC.so UNIXLIB_WOW.so OUT.hap
"""

import hashlib
import io
import json
import os
import sys
import zipfile

INNER = "resources/rawfile/wine-data.zip"
MANIFEST = "resources/rawfile/wine-runtime-manifest.json"
PE_DIR = "bin/aarch64-windows"
LIBS_DIR = "libs/arm64-v8a"


def rebuild_inner_zip(old_bytes, replacements):
    src = zipfile.ZipFile(io.BytesIO(old_bytes))
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w", zipfile.ZIP_DEFLATED) as dst:
        for info in src.infolist():
            data = None
            if info.filename in replacements:
                with open(replacements[info.filename], "rb") as f:
                    data = f.read()
            if data is None:
                data = src.read(info.filename)
            zi = zipfile.ZipInfo(info.filename, date_time=info.date_time)
            zi.compress_type = info.compress_type
            zi.external_attr = info.external_attr
            dst.writestr(zi, data)
    return buf.getvalue()


def main():
    if len(sys.argv) != 7:
        sys.exit(__doc__)
    base_hap, dll_ec, dll_wow, so_ec, so_wow, out_hap = sys.argv[1:]

    for path in (base_hap, dll_ec, dll_wow, so_ec, so_wow):
        if not os.path.isfile(path):
            sys.exit(f"missing input: {path}")

    src = zipfile.ZipFile(base_hap)
    old_inner = src.read(INNER)

    new_inner = rebuild_inner_zip(old_inner, {
        f"{PE_DIR}/libarm64ecfex.dll": dll_ec,
        f"{PE_DIR}/libwow64fex.dll": dll_wow,
    })
    payload_sha = hashlib.sha256(new_inner).hexdigest()

    manifest = json.loads(src.read(MANIFEST).decode("utf-8"))
    old_sha = manifest.get("payloadSha256")
    manifest["payloadSha256"] = payload_sha
    new_manifest = json.dumps(manifest, indent=2, ensure_ascii=False).encode("utf-8")
    print(f"payloadSha256 {old_sha} -> {payload_sha}")

    extra = {
        f"{LIBS_DIR}/libarm64ecfex.so": so_ec,
        f"{LIBS_DIR}/libwow64fex.so": so_wow,
    }

    os.makedirs(os.path.dirname(os.path.abspath(out_hap)), exist_ok=True)
    with zipfile.ZipFile(out_hap, "w", zipfile.ZIP_DEFLATED) as dst:
        for info in src.infolist():
            if info.filename == INNER:
                data = new_inner
            elif info.filename == MANIFEST:
                data = new_manifest
            else:
                data = src.read(info.filename)
            zi = zipfile.ZipInfo(info.filename, date_time=info.date_time)
            zi.compress_type = info.compress_type
            zi.external_attr = info.external_attr
            dst.writestr(zi, data)
        for name, path in extra.items():
            if name in src.namelist():
                sys.exit(f"refusing to overwrite existing entry: {name}")
            with open(path, "rb") as f:
                dst.writestr(name, f.read())

    print(f"wrote {out_hap} ({os.path.getsize(out_hap)} bytes)")
    for name, path in extra.items():
        print(f"  + {name}  ({os.path.getsize(path)} bytes)")


if __name__ == "__main__":
    main()
