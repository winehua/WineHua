"""Read-only identity check for a signed fex-rwx candidate HAP."""
import argparse
import hashlib
import json
import zipfile
from pathlib import Path

PAYLOAD = "resources/rawfile/wine-data.zip"
MANIFEST = "resources/rawfile/wine-runtime-manifest.json"
FEX = "bin/aarch64-windows/libarm64ecfex.dll"
NTDLL = "libs/arm64-v8a/ntdll.so"


def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("hap", type=Path)
    parser.add_argument("--expect-ntdll", help="expected sha256 of libs/arm64-v8a/ntdll.so")
    parser.add_argument("--expect-fex", help="expected sha256 of the payload FEX dll")
    parser.add_argument("--expect-signature", action="store_true")
    args = parser.parse_args()

    report = {"hap": str(args.hap), "hap_sha256": sha(args.hap), "size": args.hap.stat().st_size}
    with zipfile.ZipFile(args.hap) as hap:
        names = hap.namelist()
        report["entries"] = len(names)
        report["signature_entries"] = sorted(n for n in names if n.startswith("META-INF/"))
        ntdll = hashlib.sha256(hap.read(NTDLL)).hexdigest()
        manifest = json.loads(hap.read(MANIFEST))
        payload_bytes = hap.read(PAYLOAD)
        payload_sha = hashlib.sha256(payload_bytes).hexdigest()
        with zipfile.ZipFile(__import__("io").BytesIO(payload_bytes)) as payload:
            fex = hashlib.sha256(payload.read(FEX)).hexdigest()

    report["ntdll_sha256"] = ntdll
    report["fex_sha256"] = fex
    report["payload_sha256"] = payload_sha
    report["manifest_payload_sha256"] = manifest["payloadSha256"]
    checks = {
        "manifest_matches_payload": manifest["payloadSha256"] == payload_sha,
        "signature_present": bool(report["signature_entries"]),
    }
    if args.expect_ntdll:
        checks["ntdll_expected"] = ntdll == args.expect_ntdll
    if args.expect_fex:
        checks["fex_expected"] = fex == args.expect_fex
    report["checks"] = checks
    print(json.dumps(report, indent=2))
    assert all(checks.values()), checks


if __name__ == "__main__":
    main()
