#!/usr/bin/env python3
"""Pair packaged Modern DXVK bytes with the reviewed source and graphics lock."""
import argparse
import hashlib
import io
import json
from pathlib import Path
from zipfile import ZipFile

ROOT = Path(__file__).resolve().parents[1]
LOCK_PATH = ROOT / 'docs/graphics/dxvk-runtime.lock.json'


def load_modern_lock():
    lock = json.loads(LOCK_PATH.read_text())
    expected = {a + '/' + f for a in ('arm64x', 'x64', 'x86') for f in ('d3d11.dll', 'dxgi.dll')}
    if lock.get('schemaVersion') != 1 or set(lock.get('files', {})) != expected:
        raise ValueError('Modern DXVK lock must pin all six reviewed DLLs')
    return lock


def source_digest(source):
    files = {path.relative_to(source).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest()
             for path in sorted(source.rglob('*')) if path.is_file()}
    return hashlib.sha256(json.dumps(files, sort_keys=True, separators=(',', ':')).encode()).hexdigest()


def validate_modern_runtime(archive, lock=None, candidate=None):
    pinned = load_modern_lock()
    if lock is not None and lock['submodules']['dxvk-modern'] != pinned['commit']:
        raise ValueError('Modern DXVK source pin differs from its runtime lock')
    runtime = json.loads(archive.read('dxvk/manifest.json'))['runtimes']['modern-2.6']
    if runtime.get('commit') != pinned['commit']:
        raise ValueError('Modern DXVK packaged commit differs from the graphics lock')
    for member, expected in pinned['files'].items():
        architecture, filename = Path(member).parts
        data = archive.read('dxvk/modern-2.6/' + member)
        actual = hashlib.sha256(data).hexdigest()
        if candidate is not None:
            from source_build_contract import validate_member
            name = 'dxvk/modern-2.6/' + member
            validate_member(candidate, 'payload', name, data)
            expected = candidate['payload'][name]['sha256']
        if actual != expected or runtime.get(architecture, {}).get(filename) != actual:
            raise ValueError('stale or unpaired Modern DXVK payload: ' + member)
        if filename == 'd3d11.dll' and b'FP32 DenormFlushToZero disabled' not in data:
            raise ValueError('Modern DXVK lacks the reviewed Maleoon shader repair: ' + member)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--runtime-zip', type=Path)
    parser.add_argument('--hap', type=Path)
    args = parser.parse_args()
    if source_digest(ROOT / 'thirdparty/dxvk-modern/src') != load_modern_lock()['sourceSha256']:
        raise SystemExit('Modern DXVK source differs from the reviewed runtime')
    if args.hap:
        with ZipFile(args.hap) as hap:
            members = [n for n in hap.namelist() if n.endswith('/rawfile/wine-data.zip')]
            if len(members) != 1:
                raise ValueError('HAP must contain exactly one Wine runtime')
            with ZipFile(io.BytesIO(hap.read(members[0]))) as runtime:
                validate_modern_runtime(runtime)
    else:
        with ZipFile(args.runtime_zip or ROOT / 'entry/src/main/resources/rawfile/wine-data.zip') as runtime:
            validate_modern_runtime(runtime)
    print('Modern DXVK source, manifest and six reviewed DLLs PASS')
