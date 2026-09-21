"""Build an isolated font candidate from a known baseline HAP (sign separately).

Only win32u.so, the two font probes and their manifests change. All other
runtime and application entries must remain byte-identical to the baseline.
"""
import hashlib
import copy
import io
import json
from pathlib import Path
import zipfile

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "artifacts/steam-font-contract"

def digest(data):
    return hashlib.sha256(data).hexdigest()

def rewrite(source, destination, replacements):
    with zipfile.ZipFile(destination, "w", compression=zipfile.ZIP_DEFLATED) as target:
        for entry in source.infolist():
            target.writestr(copy.copy(entry), replacements.get(entry.filename, source.read(entry)))
        for name, data in replacements.items():
            if name not in source.namelist():
                target.writestr(name, data)

with zipfile.ZipFile(OUT / "baseline.hap") as baseline:
    payload_name = "resources/rawfile/wine-data.zip"
    with zipfile.ZipFile(io.BytesIO(baseline.read(payload_name))) as payload:
        assert digest(payload.read("bin/aarch64-windows/libarm64ecfex.dll")) == \
            "8aba586e7988b01cd1d24685bef778ed71f64da9dc3bf525bd720ef6596f0cfc"
        updates = {}
        manifest = json.loads(payload.read("smoke/manifest.json"))
        for arch, folder in (("amd64", "x64-fex"), ("i386", "x86")):
            name = f"{folder}/font-contract-{arch}.exe"
            data = (OUT / f"font-contract-{arch}.exe").read_bytes()
            updates[f"smoke/{name}"] = data
            manifest["files"][name] = digest(data)
        updates["smoke/manifest.json"] = json.dumps(manifest, indent=2).encode()
        suites = json.loads(payload.read("smoke/suites.json"))
        suites["suites"]["steam-font"] = {"tests": [
            {"testId": f"font-{arch}", "exe": f"{folder}/font-contract-{arch}.exe",
             "env": {}, "d3dBackend": "wined3d", "argvMode": "raw",
             "argv": ["--output", "C:/smoke/results/<run-id>/<test-id>.json"],
             "timeoutMs": 60000}
            for arch, folder in (("i386", "x86"), ("amd64", "x64-fex"))]}
        updates["smoke/suites.json"] = json.dumps(suites, indent=2).encode()
        manifest["enabledSuites"].append("steam-font")
        updates["smoke/manifest.json"] = json.dumps(manifest, indent=2).encode()
        rewrite(payload, OUT / "wine-data.zip", updates)
    manifest_name = "resources/rawfile/wine-runtime-manifest.json"
    runtime_manifest = json.loads(baseline.read(manifest_name))
    payload_data = (OUT / "wine-data.zip").read_bytes()
    runtime_manifest["payloadSha256"] = digest(payload_data)
    updates = {
        payload_name: payload_data,
        manifest_name: json.dumps(runtime_manifest, indent=2).encode(),
        "libs/arm64-v8a/win32u.so": (ROOT / "build/wine-ohos-aarch64/dlls/win32u/win32u.so").read_bytes(),
    }
    result = OUT / "font-candidate-unsigned.hap"
    rewrite(baseline, result, updates)
    with zipfile.ZipFile(result) as candidate:
        changed = [n for n in baseline.namelist() if baseline.read(n) != candidate.read(n)]
        assert set(changed) == set(updates), changed
        print(json.dumps({"changed_hap_entries": changed,
                          "win32u_sha256": digest(updates["libs/arm64-v8a/win32u.so"])}, indent=2))
