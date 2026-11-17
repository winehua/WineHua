#!/usr/bin/env python3
"""Apply pinned Box WoW64 compatibility overlays without resetting local sources."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[1]
LOCK = ROOT / 'docs/architecture/box64-wow64.lock.json'


def contract():
    return json.loads(LOCK.read_text())


def apply(source=None, verify_only=False):
    source = Path(source or ROOT / 'thirdparty/box64').resolve(strict=True)
    lock = contract()
    base = ['git', '-c', 'safe.directory=' + str(source), '-C', str(source)]
    revision = subprocess.run(base + ['rev-parse', 'HEAD'], text=True, capture_output=True)
    if revision.returncode == 0 and revision.stdout.strip() != lock['sourceCommit']:
        raise ValueError('Box source pin mismatch: ' + revision.stdout.strip())
    # Bind-mounted worktrees can lack the parent's .git directory. In that case
    # verify exact source identities instead of silently accepting arbitrary code.
    def matches(state):
        return all(hashlib.sha256((source / name).read_bytes().replace(b'\r\n', b'\n')).hexdigest() == row[state]
                   for name, row in lock['files'].items())
    for row in lock['patches']:
        patch = ROOT / row['path']
        if hashlib.sha256(patch.read_bytes()).hexdigest() != row['sha256']:
            raise ValueError('Box overlay digest mismatch: ' + row['path'])
    if matches('patched'):
        return lock
    if verify_only or not matches('original'):
        raise ValueError('Box source differs from both pinned and patched identities')
    for row in lock['patches']:
        command = ['patch', '--batch', '--forward', '-p1', '-i', str(ROOT / row['path'])]
        subprocess.run(command + ['--dry-run'], cwd=source, check=True, capture_output=True)
        subprocess.run(command, cwd=source, check=True, capture_output=True)
    if not matches('patched'):
        raise ValueError('Box overlay result identity mismatch')
    return lock


def validate_module(data):
    lock = contract()
    if len(data) < 64 or data[:2] != b'MZ':
        raise ValueError('Box module is not PE')
    pe = struct.unpack_from('<I', data, 60)[0]
    if pe + 6 > len(data) or data[pe:pe+4] != b'PE\0\0' or struct.unpack_from('<H', data, pe+4)[0] != 0xaa64:
        raise ValueError('Box module is not ARM64 PE')
    if lock['runtimeMarker'].encode() not in data:
        raise ValueError('Box module lacks the reviewed SEH/x87 runtime marker; rebuild wowbox64')
    return lock


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path)
    parser.add_argument('--verify-only', action='store_true')
    parser.add_argument('--module', type=Path)
    args = parser.parse_args()
    apply(args.source, args.verify_only)
    if args.module:
        validate_module(args.module.read_bytes())
    print('Box WoW64 managed overlays ready')
