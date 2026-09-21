"""Replace only ARM64EC FEX in the v12 payload; sign the resulting HAP separately."""
import argparse
import copy
import hashlib
import io
import json
from pathlib import Path
import zipfile

ROOT = Path(__file__).resolve().parent.parent
PAYLOAD = "resources/rawfile/wine-data.zip"
MANIFEST = "resources/rawfile/wine-runtime-manifest.json"
FEX = "bin/aarch64-windows/libarm64ecfex.dll"
BASE_PAYLOAD_SHA = "b651fac62b107e0235fd06c2da8ea977ecde862c766cad0111f109a8c4269d1e"
BASE_FEX_SHA = "8aba586e7988b01cd1d24685bef778ed71f64da9dc3bf525bd720ef6596f0cfc"


def digest(data):
    return hashlib.sha256(data).hexdigest()


def rewrite(source, destination, updates):
    assert len(source.namelist()) == len(set(source.namelist())), "duplicate ZIP entries"
    assert set(updates) <= set(source.namelist()), "replacement entry absent"
    with zipfile.ZipFile(destination, "w") as target:
        for entry in source.infolist():
            target.writestr(copy.copy(entry), updates.get(entry.filename, source.read(entry)))


def check_changes(before, after, expected):
    assert before.namelist() == after.namelist(), "entry inventory changed"
    changed = [name for name in before.namelist() if before.read(name) != after.read(name)]
    assert set(changed) == set(expected), changed
    return changed


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", type=Path, default=ROOT / "artifacts/probe-candidate/probe-candidate-unsigned.hap")
    parser.add_argument("--fex", type=Path, default=ROOT / "artifacts/fex-rwx-probe/libarm64ecfex-rwx-probe.dll")
    parser.add_argument("--ntdll", type=Path, help="Optional stripped Unix ntdll candidate (HAP entry only)")
    parser.add_argument("--output", type=Path, default=ROOT / "artifacts/fex-rwx-probe/fex-rwx-probe-unsigned.hap")
    args = parser.parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    assert args.output.resolve() != args.baseline.resolve()
    new_fex = args.fex.read_bytes()
    with zipfile.ZipFile(args.baseline) as base:
        old_payload = base.read(PAYLOAD)
        assert digest(old_payload) == BASE_PAYLOAD_SHA
        runtime_manifest = json.loads(base.read(MANIFEST))
        assert runtime_manifest["payloadSha256"] == BASE_PAYLOAD_SHA
        payload_buffer = io.BytesIO()
        with zipfile.ZipFile(io.BytesIO(old_payload)) as payload:
            assert digest(payload.read(FEX)) == BASE_FEX_SHA
            rewrite(payload, payload_buffer, {FEX: new_fex})
            with zipfile.ZipFile(payload_buffer) as candidate_payload:
                payload_changes = check_changes(payload, candidate_payload, [FEX])
        new_payload = payload_buffer.getvalue()
        runtime_manifest["payloadSha256"] = digest(new_payload)
        updates = {
            PAYLOAD: new_payload,
            MANIFEST: json.dumps(runtime_manifest, indent=2).encode(),
        }
        if args.ntdll:
            updates["libs/arm64-v8a/ntdll.so"] = args.ntdll.read_bytes()
        rewrite(base, args.output, updates)
        with zipfile.ZipFile(args.output) as candidate:
            hap_changes = check_changes(base, candidate, updates)
    report = {
        "baseline": str(args.baseline), "candidate": str(args.output),
        "changed_hap_entries": hap_changes, "changed_payload_entries": payload_changes,
        "fex_old_sha256": BASE_FEX_SHA, "fex_new_sha256": digest(new_fex),
        "payload_old_sha256": BASE_PAYLOAD_SHA, "payload_new_sha256": digest(new_payload),
        "unsigned_hap_sha256": digest(args.output.read_bytes()),
        "ntdll_sha256": digest(args.ntdll.read_bytes()) if args.ntdll else None,
    }
    args.output.with_suffix(".json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
