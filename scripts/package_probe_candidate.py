"""Build an isolated *diagnostic* candidate HAP: app libs + two wine modules.

Baseline = the deployed font-candidate HAP. Its `resources/rawfile/wine-data.zip`
is byte-identical to the runtime the device already runs, and the wine unix
builtins ship as HAP `libs/arm64-v8a/*.so` entries (stripped) — so a wine-side
diagnostic only needs to replace the corresponding libs entry; the payload (and
therefore the device's runtime unpack state) stays untouched.

Only these entries change:

  libs/arm64-v8a/libentry.so   (app-side fixes, see package_app_candidate.py)
  libs/arm64-v8a/ntdll.so      (2026-09-20 fault-attribution instrumentation)

Everything else must stay byte-identical; the script asserts that.
"""

import copy
import hashlib
import io
import json
from pathlib import Path
import zipfile

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "artifacts/probe-candidate"
BASELINE = ROOT / "artifacts/steam-font-contract/font-candidate-unsigned.hap"
BUILT_APP = ROOT / "entry/build/default/outputs/default/entry-default-signed.hap"
NEW_NTDLL = ROOT / "artifacts/probe-candidate/ntdll-probe.so"

APP_LIB_ENTRY = "libs/arm64-v8a/libentry.so"
APP_LIB_ENTRIES = ("libs/arm64-v8a/libentry.so", "libs/arm64-v8a/libwine_child.so")
# ArkTS 侧白名单等改动编译产物 (Want 参数里的 d3d_env 只认白名单键)
APP_ABC_ENTRIES = ("ets/modules.abc",)
NTDLL_ENTRY = "libs/arm64-v8a/ntdll.so"


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def main() -> int:
    OUT.mkdir(parents=True, exist_ok=True)
    new_ntdll = NEW_NTDLL.read_bytes()
    with zipfile.ZipFile(BASELINE) as base:
        old_ntdll = base.read(NTDLL_ENTRY)
        with zipfile.ZipFile(BUILT_APP) as built:
            app_libs = {name: built.read(name) for name in APP_LIB_ENTRIES}
            app_libs.update({name: built.read(name) for name in APP_ABC_ENTRIES})

        replacements = dict(app_libs)
        replacements[NTDLL_ENTRY] = new_ntdll
        result = OUT / "probe-candidate-unsigned.hap"
        with zipfile.ZipFile(result, "w", compression=zipfile.ZIP_DEFLATED) as target:
            for entry in base.infolist():
                target.writestr(copy.copy(entry),
                                replacements.get(entry.filename, base.read(entry)))

    with zipfile.ZipFile(BASELINE) as base, zipfile.ZipFile(result) as candidate:
        changed = [n for n in base.namelist() if base.read(n) != candidate.read(n)]
    assert set(changed) == set(replacements), changed
    payload = json.loads(zipfile.ZipFile(BASELINE).read(
        "resources/rawfile/wine-runtime-manifest.json"))
    print(json.dumps({
        "candidate": str(result),
        "changed_hap_entries": changed,
        "ntdll_old": digest(old_ntdll),
        "ntdll_new": digest(new_ntdll),
        "payload_sha256": payload["payloadSha256"],
        "app_lib_sha256": {name: digest(data) for name, data in app_libs.items()},
    }, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
