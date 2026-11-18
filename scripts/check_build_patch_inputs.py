#!/usr/bin/env python3
"""Reject literal build patch inputs absent from the Git index or checkout.

This check needs neither submodule initialization nor an OHOS SDK. Variable or
wildcard-generated patch lists remain validated by their respective recipes.
"""
from pathlib import Path
import json
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]
PATCH_PATH = re.compile(r'(?<![\w/.-])((?:scripts/)?patches/[\w./-]+\.patch)\b')


def tracked_files(root):
    raw = subprocess.check_output(['git', '-C', str(root), 'ls-files', '-z'])
    return set(raw.decode().rstrip('\0').split('\0'))


def active_lock_patches(value):
    # Candidate locks retain productionAdaptations as historical provenance;
    # only their ordered patches lists are inputs consumed by the build.
    if isinstance(value, dict):
        for key, child in value.items():
            if key == 'patches' and isinstance(child, list):
                for row in child:
                    if isinstance(row, dict) and isinstance(row.get('path'), str):
                        yield row['path']
            elif key != 'productionAdaptations':
                yield from active_lock_patches(child)
    elif isinstance(value, list):
        for child in value:
            yield from active_lock_patches(child)


def check_inputs(root, tracked):
    references = {}
    for name in sorted(tracked):
        path = Path(name)
        script = name.startswith('scripts/') and path.suffix in ('.sh', '.py')
        make = name == 'Makefile' or (name.startswith('make/') and path.suffix == '.mk')
        lock = name.startswith(('docs/', 'scripts/')) and name.endswith('.lock.json')
        if not (script or make or lock):
            continue
        content = (root / name).read_text()
        if lock:
            content = json.dumps(list(active_lock_patches(json.loads(content))))
        if path.suffix == '.py':
            # Path(__file__).resolve().parent is scripts/, while ROOT is the
            # checkout. Preserve that distinction for literal Python paths.
            content = content.replace(".parent / 'patches/", ".parent / 'scripts/patches/")
            content = content.replace('.parent / "patches/', '.parent / "scripts/patches/')
        for prefix in ('$SCRIPT_DIR/../', '${SCRIPT_DIR}/../', '$ROOT/', '${ROOT}/', '$(ROOT)/'):
            content = content.replace(prefix, '')
        for prefix in ('$SCRIPT_DIR/', '${SCRIPT_DIR}/', '$SCRIPTS/', '${SCRIPTS}/', '$(SCRIPTS)/'):
            content = content.replace(prefix, 'scripts/')
        for patch in PATCH_PATH.findall(content):
            references.setdefault(patch, set()).add(name)
    errors = []
    for patch, callers in sorted(references.items()):
        reason = None
        if patch not in tracked:
            reason = 'not tracked'
        elif not (root / patch).is_file() or not (root / patch).stat().st_size:
            reason = 'missing or empty'
        if reason:
            errors.append(patch + ': ' + reason + ' (referenced by ' + ', '.join(sorted(callers)) + ')')
    return references, errors


def main():
    references, errors = check_inputs(ROOT, tracked_files(ROOT))
    if errors:
        print('Build patch inputs: FAIL\n' + '\n'.join(errors))
        return 1
    print(f'Build patch inputs: PASS ({len(references)} literal inputs, tracked and present)')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
