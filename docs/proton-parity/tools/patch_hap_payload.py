#!/usr/bin/env python3
"""实验工具: 直接替换已有 HAP 内 wine-data.zip 的条目并同步 payloadSha256。

用途: 在没有 Docker / hvigor 的环境里做最小出包 (例如只改 smoke/suites.json)。
正式路径仍是 `make NATIVE_ARCH=arm64-v8a hap`。

产物是**未签名** HAP, 之后用 sign.py (或 Windows 侧
DevEco 的 node + java + hap-sign-tool.jar) 签名。

用法:
  patch_hap_payload.py IN.hap OUT.hap INNER_NAME=LOCAL_FILE [INNER_NAME=LOCAL_FILE ...]

例:
  patch_hap_payload.py base.hap out.hap smoke/suites.json ./suites.json
"""

import hashlib
import io
import json
import os
import sys
import zipfile

INNER = "resources/rawfile/wine-data.zip"
MANIFEST = "resources/rawfile/wine-runtime-manifest.json"


def main():
    if len(sys.argv) < 4:
        sys.exit(__doc__)
    in_hap, out_hap = sys.argv[1], sys.argv[2]
    replacements = {}
    for spec in sys.argv[3:]:
        name, _, path = spec.partition("=")
        if not name or not os.path.isfile(path):
            sys.exit(f"bad replacement spec: {spec}")
        replacements[name] = path

    src = zipfile.ZipFile(in_hap)
    old_inner = src.read(INNER)
    inner = zipfile.ZipFile(io.BytesIO(old_inner))

    buf = io.BytesIO()
    seen = set()
    with zipfile.ZipFile(buf, "w", zipfile.ZIP_DEFLATED) as dst:
        for info in inner.infolist():
            local = replacements.get(info.filename)
            if local:
                seen.add(info.filename)
                data = open(local, "rb").read()
            else:
                data = inner.read(info.filename)
            zi = zipfile.ZipInfo(info.filename, date_time=info.date_time)
            zi.compress_type = info.compress_type
            zi.external_attr = info.external_attr
            dst.writestr(zi, data)
        for name, path in replacements.items():
            if name in seen:
                continue
            dst.writestr(name, open(path, "rb").read())
    inner.close()

    new_inner = buf.getvalue()
    payload_sha = hashlib.sha256(new_inner).hexdigest()

    manifest = json.loads(src.read(MANIFEST).decode("utf-8"))
    print(f"payloadSha256 {manifest.get('payloadSha256')} -> {payload_sha}")
    manifest["payloadSha256"] = payload_sha
    new_manifest = json.dumps(manifest, indent=2, ensure_ascii=False).encode("utf-8")

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

    print(f"wrote {out_hap} ({os.path.getsize(out_hap)} bytes); replaced: {sorted(replacements)}")


if __name__ == "__main__":
    main()
