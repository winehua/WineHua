#!/usr/bin/env python3
"""Apply the reviewed OHOS Legacy DXVK overlays; fail on drift, never reset sources."""
import argparse
import hashlib
import json
import re
import shutil
import tempfile
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]

def stack_applied(source, patches):
    # Later reviewed overlays may modify earlier hunks. Prove the entire stack
    # in reverse order on only the affected files; never reverse caller sources.
    names = {name for patch in patches for name in
             re.findall(r"^\+\+\+ b/(\S+)", patch.read_text(), re.M)}
    if not all((source / name).is_file() for name in names):
        return False
    with tempfile.TemporaryDirectory(prefix='dxvk-overlay-proof-') as folder:
        proof = Path(folder)
        subprocess.run(['git', 'init', '-q', str(proof)], check=True)
        for name in names:
            target = proof / name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source / name, target)
        for patch in reversed(patches):
            if subprocess.run(['git', '-C', str(proof), 'apply', '--reverse', str(patch)],
                              capture_output=True).returncode:
                return False
    return True


def apply(source=None, verify_only=False):
    source = Path(source or ROOT / 'thirdparty/dxvk').resolve(strict=True)
    pin = json.loads((ROOT / 'docs/graphics/texture-compat-runtime.lock.json').read_text())['runtimes']['legacy']
    patches = [ROOT / row['path'] for row in pin.get('patches', [])]
    # Validate all identities before performing any source mutation.
    for row, patch in zip(pin.get('patches', []), patches):
        if hashlib.sha256(patch.read_bytes().replace(b'\r\n', b'\n')).hexdigest() != row['sha256']:
            raise ValueError('DXVK overlay digest mismatch: ' + row['path'])
    if stack_applied(source, patches):
        return pin.get('patchedSourceSha256', pin['sourceSha256'])
    for row in pin.get('patches', []):
        patch = ROOT / row['path']
        if hashlib.sha256(patch.read_bytes().replace(b'\r\n', b'\n')).hexdigest() != row['sha256']:
            raise ValueError('DXVK overlay digest mismatch: ' + row['path'])
        command = ['git', '-c', 'safe.directory=' + str(source), '-C', str(source), 'apply']
        if subprocess.run(command + ['--reverse', '--check', str(patch)], capture_output=True).returncode == 0:
            continue
        if verify_only:
            raise ValueError('DXVK overlay missing: ' + row['path'])
        subprocess.run(command + ['--check', str(patch)], check=True, capture_output=True)
        subprocess.run(command + [str(patch)], check=True, capture_output=True)
    return pin.get('patchedSourceSha256', pin['sourceSha256'])

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path)
    parser.add_argument('--verify-only', action='store_true')
    args = parser.parse_args()
    apply(args.source, args.verify_only)
    print('Legacy DXVK managed overlays ready')
