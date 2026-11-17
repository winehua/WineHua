#!/usr/bin/env python3
"""Apply a pinned overlay stack without resetting local source or Git indexes."""
import argparse
from pathlib import Path
import re
import shutil
import subprocess
import tempfile


def apply(source, overlays, scratch):
    source = source.resolve(strict=True)
    files = set()
    for overlay in overlays:
        for name in re.findall(r'^\+\+\+ b/(.+)$', overlay.read_text(), re.M):
            relative = Path(name)
            if relative.is_absolute() or '..' in relative.parts:
                raise ValueError('unsafe overlay path: ' + name)
            files.add(name)
        if '+++ /dev/null' in overlay.read_text():
            raise ValueError('display overlay deletions require explicit migration')
    if not files:
        raise ValueError('overlay stack contains no files')
    scratch.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='display-overlay-', dir=scratch) as work:
        root = Path(work)
        baseline = {}
        for name in sorted(files):
            item = subprocess.run(['git', '-C', str(source), 'show', 'HEAD:' + name], capture_output=True)
            baseline[name] = item.stdout if item.returncode == 0 else None
            if item.returncode == 0:
                target = root / name
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(item.stdout)
        for overlay in overlays:
            subprocess.run(['patch', '-d', str(root), '-p1', '--batch', '--force', '--dry-run'], input=overlay.read_bytes(), check=True, capture_output=True)
            subprocess.run(['patch', '-d', str(root), '-p1', '--batch', '--force'], input=overlay.read_bytes(), check=True, capture_output=True)
        changes = []
        # Validate the WHOLE stack before touching even one caller-owned file.
        for name in sorted(files):
            target = source / name
            if target.is_symlink() or not target.resolve().is_relative_to(source):
                raise ValueError('overlay target escapes source: ' + name)
            current = target.read_bytes() if target.is_file() else None
            wanted = (root / name).read_bytes()
            if current == wanted:
                continue
            if current != baseline[name]:
                raise ValueError('unrecognized local changes preserved: ' + str(target))
            changes.append(name)
        for name in changes:
            target = source / name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(root / name, target)
        print('Display overlays verified; updated', len(changes), 'files')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', required=True, type=Path)
    parser.add_argument('--scratch', required=True, type=Path)
    parser.add_argument('overlays', nargs='+', type=Path)
    args = parser.parse_args()
    apply(args.source, [p.resolve(strict=True) for p in args.overlays], args.scratch)


if __name__ == '__main__':
    main()
