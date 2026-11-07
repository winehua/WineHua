#!/usr/bin/env python3
"""Create a runtime candidate with a rebuilt, hash-checked legacy DXVK overlay.

Usage: python scripts/package_dxvk_candidate.py --base wine-data.zip
  --legacy build/dxvk-linux-rebuilt --out build/dxvk-runtime-candidate

Only the legacy DXVK DLLs and their manifest change. The source ZIP, current
rawfiles, Wine prefix and devices are never modified. This is an incremental
candidate, not a clean Wine/Mesa build.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import zipfile

from validate_desktop_rendering import digest, verify_dxvk_runtime

DLLS = ('d3d9.dll', 'd3d10.dll', 'd3d10_1.dll', 'd3d10core.dll', 'd3d11.dll', 'dxgi.dll')


def package_candidate(base, legacy, out):
    out.mkdir(parents=True, exist_ok=True)
    output = out / 'wine-data.zip'
    assert output.resolve() != base.resolve(), 'Output must not overwrite the source ZIP'
    provenance = json.loads((legacy / 'build-provenance.json').read_text(encoding='utf-8'))
    replacements = {}
    for arch in ('x64', 'x86'):
        for dll in DLLS:
            relative = f'{arch}/bin/{dll}'
            data = (legacy / relative).read_bytes()
            actual = hashlib.sha256(data).hexdigest()
            assert data and provenance['files'].get(relative) == actual, f'Unverified rebuilt DLL: {relative}'
            replacements[f'dxvk/legacy/{arch}/{dll}'] = data
    with base.open('rb') as stream:
        base_sha = digest(stream)
    try:
        with zipfile.ZipFile(base) as source, zipfile.ZipFile(output, 'w') as target:
            manifest = json.loads(source.read('dxvk/manifest.json'))
            for profile in ('legacy', 'modern-2.6'):
                for arch in ('x64', 'x86'):
                    prefix = f'dxvk/{profile}/{arch}/'
                    members = {n for n in source.namelist() if n.startswith(prefix) and n.endswith('.dll')}
                    members.update(n for n in replacements if n.startswith(prefix))
                    manifest['runtimes'][profile][arch] = {
                        Path(n).name: hashlib.sha256(replacements[n] if n in replacements else source.read(n)).hexdigest()
                        for n in sorted(members)}
            manifest['runtimes']['legacy']['commit'] = provenance['sourceCommit']
            manifest['runtimes']['legacy']['buildProvenance'] = provenance
            manifest_data = (json.dumps(manifest, indent=2) + '\n').encode('utf-8')
            replacements['dxvk/manifest.json'] = manifest_data
            written = set()
            for info in source.infolist():
                assert info.filename not in written, f'Duplicate source ZIP member: {info.filename}'
                written.add(info.filename)
                if info.filename in replacements:
                    target.writestr(info, replacements[info.filename])
                else:
                    with source.open(info) as reader, target.open(info, 'w') as writer:
                        shutil.copyfileobj(reader, writer, 1024 * 1024)
            for member, data in replacements.items():
                if member not in written:
                    target.writestr(member, data, compress_type=zipfile.ZIP_DEFLATED)
        with zipfile.ZipFile(output) as candidate:
            assert candidate.testzip() is None, 'Candidate ZIP CRC failure'
            verified = verify_dxvk_runtime(candidate)
        with output.open('rb') as stream:
            payload_sha = digest(stream)
        runtime_manifest = {'schemaVersion': 1, 'payload': 'wine-data.zip', 'payloadSha256': payload_sha}
        (out / 'wine-runtime-manifest.json').write_text(json.dumps(runtime_manifest, indent=2), encoding='utf-8')
        report = {'basePayloadSha256': base_sha, 'payloadSha256': payload_sha,
                  'changedMembers': sorted(replacements), 'verifiedDxvkDlls': verified,
                  'scope': 'incremental DXVK overlay; Wine and Mesa reused; no device deployment'}
        (out / 'candidate.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
        return report
    except Exception:
        output.unlink(missing_ok=True)
        raise


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--base', required=True, type=Path)
    parser.add_argument('--legacy', required=True, type=Path)
    parser.add_argument('--out', required=True, type=Path)
    args = parser.parse_args()
    report = package_candidate(args.base, args.legacy, args.out)
    print(json.dumps({'payloadSha256': report['payloadSha256'],
                      'changedMembers': len(report['changedMembers']), 'out': str(args.out)}))


if __name__ == '__main__':
    main()
