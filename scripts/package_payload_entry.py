"""Replace a single entry inside the HAP's wine-data.zip payload and repackage.

Diagnostic helper for testing one rebuilt runtime DLL without re-running the
whole payload assembly. The HAP is rewritten only for the payload and its
runtime manifest hash; every other entry keeps its bytes.
"""
import argparse
import copy
import hashlib
import io
import json
from pathlib import Path
import zipfile

PAYLOAD = "resources/rawfile/wine-data.zip"
MANIFEST = "resources/rawfile/wine-runtime-manifest.json"


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def rewrite(source: zipfile.ZipFile, destination, updates, expect):
    assert len(source.namelist()) == len(set(source.namelist())), "duplicate ZIP entries"
    assert set(updates) <= set(source.namelist()), "replacement entry absent"
    if expect:
        assert expect in updates, "expect entry must be replaced"
    with zipfile.ZipFile(destination, "w") as target:
        for entry in source.infolist():
            target.writestr(copy.copy(entry), updates.get(entry.filename, source.read(entry)))


def check_changes(before, after, expected):
    assert before.namelist() == after.namelist(), "entry inventory changed"
    changed = [name for name in before.namelist() if before.read(name) != after.read(name)]
    assert set(changed) == set(expected), changed
    return changed


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base", type=Path, required=True, help="source (unsigned) HAP")
    parser.add_argument("--payload-entry", help="path inside wine-data.zip")
    parser.add_argument("--file", type=Path, help="replacement file for --payload-entry")
    parser.add_argument("--payload-from", type=Path,
                        help="take wine-data.zip + runtime manifest from this HAP instead "
                             "(used when the app libraries change but the runtime must stay)")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)

    if args.payload_from:
        # Whole-payload swap: keep the freshly built app HAP (native libs from this
        # tree) but carry the runtime that is actually installed on the device.
        assert not (args.payload_entry or args.file), "use either --payload-from or --payload-entry"
        with zipfile.ZipFile(args.payload_from) as source:
            new_payload = source.read(PAYLOAD)
            manifest = json.loads(source.read(MANIFEST))
            assert manifest["payloadSha256"] == digest(new_payload), "source manifest mismatch"
        with zipfile.ZipFile(args.base) as base:
            old_payload = base.read(PAYLOAD)
            assert digest(old_payload) != digest(new_payload), "payloads are identical"
            updates = {PAYLOAD: new_payload, MANIFEST: json.dumps(manifest, indent=2).encode()}
            rewrite(base, args.output, updates, PAYLOAD)
            with zipfile.ZipFile(args.output) as candidate:
                hap_changes = check_changes(base, candidate, updates)
        report = {
            "base": str(args.base), "payload_from": str(args.payload_from),
            "candidate": str(args.output),
            "payload_old_sha256": digest(old_payload),
            "payload_new_sha256": digest(new_payload),
            "changed_hap_entries": hap_changes,
            "unsigned_hap_sha256": digest(args.output.read_bytes()),
        }
        args.output.with_suffix(".json").write_text(json.dumps(report, indent=2) + "\n")
        print(json.dumps(report, indent=2))
        return

    assert args.payload_entry and args.file, "need --payload-entry and --file"
    new_bytes = args.file.read_bytes()
    with zipfile.ZipFile(args.base) as base:
        old_payload = base.read(PAYLOAD)
        manifest = json.loads(base.read(MANIFEST))
        assert manifest["payloadSha256"] == digest(old_payload), "baseline manifest mismatch"

        buffer = io.BytesIO()
        with zipfile.ZipFile(io.BytesIO(old_payload)) as payload:
            old_entry = payload.read(args.payload_entry)
            assert old_entry != new_bytes, "replacement is identical to the current entry"
            rewrite(payload, buffer, {args.payload_entry: new_bytes}, args.payload_entry)
            with zipfile.ZipFile(buffer) as candidate:
                payload_changes = check_changes(payload, candidate, [args.payload_entry])

        new_payload = buffer.getvalue()
        manifest["payloadSha256"] = digest(new_payload)
        updates = {PAYLOAD: new_payload, MANIFEST: json.dumps(manifest, indent=2).encode()}
        rewrite(base, args.output, updates, PAYLOAD)
        with zipfile.ZipFile(args.output) as candidate:
            hap_changes = check_changes(base, candidate, updates)

    report = {
        "base": str(args.base),
        "candidate": str(args.output),
        "payload_entry": args.payload_entry,
        "entry_old_sha256": digest(old_entry),
        "entry_new_sha256": digest(new_bytes),
        "changed_payload_entries": payload_changes,
        "changed_hap_entries": hap_changes,
        "payload_new_sha256": digest(new_payload),
        "unsigned_hap_sha256": digest(args.output.read_bytes()),
    }
    args.output.with_suffix(".json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
