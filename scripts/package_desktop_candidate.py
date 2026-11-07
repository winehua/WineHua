#!/usr/bin/env python3
"""Build a Windows HAP candidate using an explicitly selected existing runtime.

python scripts/package_desktop_candidate.py --payload path/wine-data.zip \
  --manifest path/wine-runtime-manifest.json --out build/desktop-candidate

Uses package_windows.ps1 and local.properties for the SDK. The selected payload
is hashed before building. Current rawfile payload/manifest are snapshotted and
restored in finally, then rehashed; nothing is installed or reset on a device.
Output: desktop-candidate-signed.hap, build.log and candidate.json under --out.
"""
import argparse
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
from validate_desktop_rendering import ROOT, digest, verify_hap


def sha(path):
    with path.open('rb') as stream:
        return digest(stream)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--payload', type=Path, required=True)
    parser.add_argument('--manifest', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    selected = json.loads(args.manifest.read_text(encoding='utf-8'))
    assert sha(args.payload) == selected['payloadSha256'], 'Selected payload/manifest mismatch'
    args.out.mkdir(parents=True, exist_ok=True)
    candidate = args.out / 'desktop-candidate-signed.hap'
    if candidate.exists():
        raise RuntimeError('Candidate already exists; use a new output directory.')
    raw = ROOT / 'entry/src/main/resources/rawfile'
    payload = raw / 'wine-data.zip'
    manifest = raw / 'wine-runtime-manifest.json'
    original_sha = sha(payload)
    original_manifest = manifest.read_bytes()
    # Hvigor may clean the project's build directory during configuration.
    # Input backups and the live build log must survive that cleanup.
    with tempfile.TemporaryDirectory(prefix='winehua-rawfile-snapshot-') as temporary:
        saved = Path(temporary) / 'wine-data.zip'
        build_log = Path(temporary) / 'build.log'
        shutil.copy2(payload, saved)
        try:
            shutil.copyfile(args.payload, payload)
            shutil.copyfile(args.manifest, manifest)
            with build_log.open('wb') as log:
                subprocess.run(['powershell.exe', '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File',
                    str(ROOT / 'scripts/package_windows.ps1')], cwd=ROOT,
                    stdout=log, stderr=subprocess.STDOUT, check=True)
            built = ROOT / 'entry/build/default/outputs/default/entry-default-signed.hap'
            args.out.mkdir(parents=True, exist_ok=True)
            shutil.copy2(built, candidate)
            report = verify_hap(candidate)
        finally:
            shutil.copyfile(saved, payload)
            manifest.write_bytes(original_manifest)
            assert sha(payload) == original_sha, 'Working rawfile was not restored'
            args.out.mkdir(parents=True, exist_ok=True)
            if build_log.exists():
                shutil.copy2(build_log, args.out / 'build.log')
    report['working_runtime_restored'] = True
    report['selected_runtime_manifest'] = selected
    (args.out / 'candidate.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
