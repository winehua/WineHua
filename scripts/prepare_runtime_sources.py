#!/usr/bin/env python3
"""Materialize the locked Proton/FEX recipes without modifying submodules.

Only public upstream commits are submodule pins. Adapted commit objects are
reconstructed from the checked patch and history files, without private remotes
or a previously packaged VPP HAP. Existing foreign or dirty sources are refused.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def git(repo, *args, data=None):
    result = subprocess.run(['git', '-c', 'gc.auto=0', '-c', 'maintenance.auto=false',
                             '-C', str(repo), *args], input=data, capture_output=True, timeout=180)
    if result.returncode:
        raise ValueError('git ' + ' '.join(args[:2]) + ': ' + result.stderr.decode(errors='replace')[-1200:])
    return result.stdout


def load(component, root=ROOT):
    lock = json.loads((root / f'docs/upstream-sync/{component}-candidate.lock.json').read_text())
    history_path = root / f'docs/upstream-sync/{component}-adapted-history.json'
    if 'history' in lock and hashlib.sha256(history_path.read_bytes()).hexdigest() != lock['history']['sha256']:
        raise ValueError('Adapted history checksum differs from source lock')
    history = json.loads(history_path.read_text())
    for row in lock['patches']:
        path = root / row['path']
        if not path.resolve().is_relative_to(root.resolve()):
            raise ValueError('Patch path is outside source checkout')
        if hashlib.sha256(path.read_bytes()).hexdigest() != row['sha256']:
            raise ValueError('Locked patch checksum differs: ' + row['path'])
        raw = history[row['commit']].encode()
        oid = hashlib.sha1(b'commit ' + str(len(raw)).encode() + b'\0' + raw).hexdigest()
        if oid != row['commit']:
            raise ValueError('Adapted history object differs from lock')
    return lock, history


def verify(source, lock):
    if git(source, 'rev-parse', 'HEAD').decode().strip() != lock['adaptedCommit']:
        raise ValueError('Runtime source HEAD differs from the selected recipe')
    if git(source, 'rev-parse', 'HEAD^{tree}').decode().strip() != lock['adaptedTree']:
        raise ValueError('Runtime source tree differs from the selected recipe')
    if git(source, 'status', '--porcelain', '--untracked-files=all'):
        raise ValueError('Runtime source has unregistered modifications')


def prepare(component, source, lock, history, root=ROOT, verify_only=False):
    source = source.absolute()
    if source.is_symlink():
        raise ValueError('Runtime source must not be a symlink')
    if verify_only:
        verify(source, lock)
        return
    # Explicit, already adapted caches can be shared read-only, but must match
    # the entire tree and recipe. Never repair a caller-owned external checkout.
    managed = root / 'build/runtime-sources'
    if not source.resolve().is_relative_to(managed.resolve()):
        verify(source, lock)
        return
    source.parent.mkdir(parents=True, exist_ok=True)
    for ancestor in [source, *source.parents]:
        if ancestor.is_symlink():
            raise ValueError('Refusing runtime source symlink ancestor')
        if ancestor == root:
            break
    upstream = lock['upstream']['commit']
    if not source.exists():
        submodule = root / ('thirdparty/wine-valve' if component == 'wine' else 'thirdparty/fex')
        actual = git(submodule, 'rev-parse', 'HEAD').decode().strip()
        if actual != upstream:
            raise ValueError('Initialize the public upstream submodule at its pinned commit: ' + str(submodule))
        subprocess.run(['git', '-c', 'gc.auto=0', 'clone', '--shared', '--no-checkout',
                        str(submodule), str(source)], check=True, capture_output=True, timeout=180)
        git(source, 'checkout', '--detach', upstream)
    if git(source, 'status', '--porcelain', '--untracked-files=all'):
        raise ValueError('Existing runtime source is dirty; preserve it and inspect the interrupted build')
    head = git(source, 'rev-parse', 'HEAD').decode().strip()
    commits = [upstream] + [row['commit'] for row in lock['patches']]
    if head not in commits:
        raise ValueError('Existing runtime source belongs to another recipe')
    position = commits.index(head)
    for row in lock['patches'][position:]:
        raw = history[row['commit']].encode()
        headers = raw.split(b'\n\n', 1)[0].splitlines()
        parents = [line[7:].decode() for line in headers if line.startswith(b'parent ')]
        trees = [line[5:].decode() for line in headers if line.startswith(b'tree ')]
        if parents != [head]:
            raise ValueError('Locked patch history does not continue the previous commit')
        git(source, 'apply', '--index', '--whitespace=nowarn', str(root / row['path']))
        tree = git(source, 'write-tree').decode().strip()
        if trees != [tree]:
            raise ValueError('Replayed patch tree differs from adapted history')
        commit = git(source, 'hash-object', '-w', '-t', 'commit', '--stdin', data=raw).decode().strip()
        if commit != row['commit']:
            raise ValueError('Replayed commit differs from adapted history')
        git(source, 'update-ref', 'HEAD', commit, head)
        head = commit
    # FEX's public nested dependencies stay pinned by its own .gitmodules.
    # The dependency download is explicit in Make, not a surprise verification
    # side effect. This function remains usable with offline source caches.
    verify(source, lock)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--component', choices=('wine', 'fex'), required=True)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--verify-only', action='store_true')
    args = parser.parse_args()
    lock, history = load(args.component)
    prepare(args.component, args.source, lock, history, verify_only=args.verify_only)
    print(json.dumps(dict(component=args.component, commit=lock['adaptedCommit'],
                          tree=lock['adaptedTree'], patches=len(lock['patches']))))


if __name__ == '__main__':
    main()
