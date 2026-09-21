"""Build an app-side candidate HAP from the deployed baseline HAP.

Why: the full `package.sh hap` path re-packs the HAP from the current build tree,
whose embedded wine payload is older than the runtime the device is actually
running (payload hash differs). Installing that HAP would make the device re-unpack
the stale runtime and silently drop the verified font fix.

So: keep every byte of the deployed baseline (payload, win32u.so, FEX, smoke
payload, metadata) and replace only the app native libraries that this change
actually rebuilt.
"""

import hashlib
import json
from pathlib import Path
import zipfile

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "artifacts/app-candidate"
BASELINE = ROOT / "artifacts/steam-font-contract/font-candidate-unsigned.hap"
BUILT = ROOT / "entry/build/default/outputs/default/entry-default-signed.hap"

# 设备当前 runtime 的 payload 指纹 (读自 files/wine/.winehua-runtime-manifest.json)
EXPECTED_PAYLOAD_SHA256 = "b651fac62b107e0235fd06c2da8ea977ecde862c766cad0111f109a8c4269d1e"

# 只搬运这些 entry (相对 HAP 根)
REPLACE_ENTRIES = ("libs/arm64-v8a/libentry.so",)


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def main() -> int:
    OUT.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(BASELINE) as base:
        manifest = json.loads(base.read("resources/rawfile/wine-runtime-manifest.json"))
        assert manifest["payloadSha256"] == EXPECTED_PAYLOAD_SHA256, manifest["payloadSha256"]
        with zipfile.ZipFile(BUILT) as built:
            replacements = {}
            hashes = {}
            for name in REPLACE_ENTRIES:
                replacements[name] = built.read(name)
                hashes[name] = digest(replacements[name])
            result = OUT / "app-candidate-unsigned.hap"
            with zipfile.ZipFile(result, "w", compression=zipfile.ZIP_DEFLATED) as target:
                for entry in base.infolist():
                    import copy
                    target.writestr(copy.copy(entry),
                                    replacements.get(entry.filename, base.read(entry)))
    with zipfile.ZipFile(BASELINE) as base, zipfile.ZipFile(result) as candidate:
        changed = [n for n in base.namelist() if base.read(n) != candidate.read(n)]
    assert set(changed) == set(REPLACE_ENTRIES), changed
    print(json.dumps({
        "candidate": str(result),
        "baseline": str(BASELINE),
        "replaced_entries": hashes,
        "changed_entries": changed,
        "payload_sha256": EXPECTED_PAYLOAD_SHA256,
    }, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
