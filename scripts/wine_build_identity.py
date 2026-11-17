#!/usr/bin/env python3
"""Fail closed on foreign/stale Wine caches; never clean, reset or reconfigure them."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys


def digest(data):
    return hashlib.sha256(data).hexdigest()


def file_hash(path):
    return digest(path.read_bytes())


def git(root, *args):
    result = subprocess.run(['git', '--no-optional-locks', '-c', 'diff.autoRefreshIndex=false', '-C', str(root), *args], capture_output=True,
                            env={**os.environ, 'GIT_OPTIONAL_LOCKS': '0'})
    if result.returncode:
        raise ValueError(f'cannot establish source Git identity at {root}; Git metadata must be readable')
    return result.stdout


def optional_git(root, *args):
    try:
        if Path(git(root, 'rev-parse', '--show-toplevel').decode().strip()).resolve() != root.resolve():
            return None
        return git(root, *args).decode().strip()
    except ValueError:
        return None


def full_source_digest(source):
    # Mounted worktrees can lack their external Git metadata. Hash every source
    # entry (except Git itself), never claim a commit was verified in this mode.
    entries = []
    for directory, dirs, files in os.walk(source, followlinks=False):
        dirs[:] = sorted(name for name in dirs if name != '.git')
        for name in sorted(files + [name for name in dirs if (Path(directory) / name).is_symlink()]):
            if name == '.git':
                continue
            path = Path(directory) / name
            relative = str(path.relative_to(source))
            if path.is_symlink():
                if not path.is_file():
                    raise ValueError(f'cannot establish content identity for source directory symlink: {relative}')
                entries.append([relative, 'link:' + os.readlink(path), file_hash(path)])
            else:
                entries.append([relative, file_hash(path)])
    return digest(json.dumps(entries, separators=(',', ':')).encode())


def source_identity(source):
    head = optional_git(source, 'rev-parse', 'HEAD')
    if head is None:
        return {'basis': 'full-source-tree-sha256', 'head': None, 'pin_verified': False,
                'tree_sha256': full_source_digest(source)}
    # Includes staged and unstaged tracked edits relative to the actual source
    # HEAD, plus untracked source files. Never substitute the legacy Wine pin.
    delta = git(source, 'diff', '--binary', 'HEAD', '--')
    untracked = git(source, 'ls-files', '--others', '--exclude-standard', '-z').split(b'\0')
    files = []
    for raw in untracked:
        if not raw:
            continue
        name = os.fsdecode(raw)
        path = source / name
        if path.is_symlink():
            files.append([name, 'link:' + os.readlink(path)])
        elif path.is_file():
            files.append([name, file_hash(path)])
    # These may be ignored generated inputs but are consumed by Wine's build.
    generated = {}
    for name in ('configure', 'include/config.h.in', 'dlls/ntdll/ntsyscalls.h',
                 'include/wine/vulkan.h', 'dlls/vulkan-1/vulkan-1.spec'):
        path = source / name
        if path.is_file():
            generated[name] = file_hash(path)
    return {'basis': 'git-head-and-effective-delta', 'head': head, 'pin_verified': True,
            'tree_sha256': full_source_digest(source), 'tracked_delta_sha256': digest(delta),
            'untracked_files': files, 'generated_inputs': generated}


def ordered_overlays(repo):
    repo = repo.resolve()
    script = repo / 'scripts/build_wine.sh'
    if 'prepare_runtime_sources.py' in script.read_text():
        from prepare_runtime_sources import load
        lock, _ = load('wine', repo)
        return [{'path': item['path'], 'sha256': item['sha256']} for item in lock['patches']]
    overlays = []
    for item in re.findall(r'^ensure_wine_patch\s+"([^"\n]+)"', script.read_text(), re.M):
        # Resolve only the two supported build variables. Never evaluate shell
        # text or consult the caller's environment when hashing build inputs.
        for token, value in (('${SCRIPT_DIR}', script.parent), ('$SCRIPT_DIR', script.parent),
                             ('${ROOT}', repo), ('$ROOT', repo)):
            item = item.replace(token, str(value))
        if '$' in item or '`' in item:
            raise ValueError('unresolved Wine overlay path: ' + item)
        path = Path(item)
        path = (path if path.is_absolute() else repo / path).resolve()
        if not path.is_relative_to(repo / 'patches/wine'):
            raise ValueError('Wine overlay escapes patches/wine: ' + str(path))
        overlays.append({'path': str(path.relative_to(repo)), 'sha256': file_hash(path)})
    if not overlays:
        raise ValueError('no ordered Wine overlays found in build_wine.sh')
    return overlays


def request_identity(args, source):
    repo = args.repo.resolve()
    script = repo / 'scripts/build_wine.sh'
    overlays = ordered_overlays(repo)
    pin = optional_git(repo, 'ls-tree', 'HEAD', 'thirdparty/wine-valve')
    pin_line = pin.split() if pin else []
    active_pin = pin_line[2] if len(pin_line) >= 3 and pin_line[1] == 'commit' else None
    tools = []
    for item in args.tool:
        path = Path(item).resolve(strict=True)
        version = subprocess.run([str(path), '--version'], capture_output=True, timeout=15)
        if version.returncode:
            raise ValueError(f'cannot identify tool {path}')
        tools.append({'path': str(path), 'sha256': file_hash(path),
                      'version_sha256': digest(version.stdout + version.stderr)})
    return {'source_path': str(source), 'build_path': str(args.build.resolve()), 'active_gitlink': active_pin, 'gitlink_verified': active_pin is not None,
            'build_script_sha256': file_hash(script),
            'identity_guard_sha256': file_hash(Path(__file__)),
            'environment_script_sha256': file_hash(repo / 'scripts/env.sh'),
            'diagnostics_inputs': {name: file_hash(repo / 'scripts' / name) for name in
                                  ('wine_runtime_diagnostics.py', 'wine_runtime_diagnostics.h')},
            'overlay_proof_sha256': file_hash(repo / 'scripts/verify_wine_overlay_stack.py')
                if (repo / 'scripts/verify_wine_overlay_stack.py').is_file() else None,
            'x11_config_check_sha256': file_hash(repo / 'scripts/verify_x11_wine_config.py')
                if (repo / 'scripts/verify_x11_wine_config.py').is_file() else None,
            'gstreamer_dependency_inventory': gstreamer_dependency_inventory(repo, args.config),
            'gstreamer_inputs': {name: file_hash(repo / 'scripts' / name) for name in
                                  ('gstreamer_plugins.json', 'gstreamer_inventory.py', 'wine_gstreamer_loader.h')},
            'ordered_overlays': overlays, 'configuration': sorted(args.config), 'tools': tools}


def gstreamer_dependency_inventory(repo, config):
    from gstreamer_inventory import describe
    values = dict(item.split('=', 1) for item in config)
    build = Path(os.environ.get('BUILD_DIR', repo / 'build'))
    plugins = build / 'sysroot-ext/usr/lib' / values.get('TARGET', '') / 'gstreamer-1.0'
    if not plugins.is_dir(): return None
    return describe(plugins, values['WINE_ARCH'], values['NATIVE_ARCH'])['inventorySha256']


def cached_srcdirs(path):
    text = path.read_text(errors='replace')
    values = re.findall(r'^\s*(?:srcdir|top_srcdir|abs_srcdir|abs_top_srcdir)\s*=\s*(.*?)\s*$', text, re.M)
    values += re.findall(r'^S\["(?:srcdir|top_srcdir|abs_srcdir|abs_top_srcdir)"\]="(.*)"$', text, re.M)
    # Autoconf records --srcdir in ac_cs_config. Parse words, never evaluate shell.
    for line in text.splitlines():
        if line.startswith('ac_cs_config='):
            outer = shlex.split(line.split('=', 1)[1])
            for word in shlex.split(' '.join(outer)):
                if word.startswith('--srcdir='):
                    values.append(word.split('=', 1)[1])
    return values


def check_paths(build, source):
    for directory in [build / 'wine-native', *sorted(build.glob('wine-ohos-*'))]:
        for filename in ('Makefile', 'config.status'):
            path = directory / filename
            if not path.exists():
                continue
            values = cached_srcdirs(path)
            if not values:
                raise ValueError(f'cannot establish cached srcdir in {path}')
            for value in values:
                value = value.strip("'\"")
                if '$' in value or '`' in value or not value:
                    raise ValueError(f'unresolved cached srcdir in {path}: {value!r}')
                actual = (directory / value).resolve()
                if actual != source:
                    raise ValueError(f'cached srcdir mismatch in {path}: {actual} != {source}')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('mode', choices=['check', 'record'])
    parser.add_argument('--source', required=True, type=Path)
    parser.add_argument('--build', required=True, type=Path)
    parser.add_argument('--repo', required=True, type=Path)
    parser.add_argument('--config', action='append', default=[])
    parser.add_argument('--tool', action='append', default=[])
    parser.add_argument('--require-manifest', action='store_true')
    args = parser.parse_args()
    source = args.source.resolve(strict=True)
    build = args.build.resolve()
    stamp = build / 'wine-build-identity.json'
    check_paths(build, source)
    request = request_identity(args, source)
    identity = {'schema': 1, 'request': request, 'source': source_identity(source)}
    if not identity['source']['pin_verified'] or not request['gitlink_verified']:
        print('Wine provenance: source basis=' + identity['source']['basis'] +
              '; source pin verified=' + str(identity['source']['pin_verified']) +
              '; superproject gitlink verified=' + str(request['gitlink_verified']), file=sys.stderr)
    if stamp.exists():
        previous = json.loads(stamp.read_text())
        if previous.get('schema') != identity['schema']:
            raise ValueError('unsupported cached Wine identity schema')
        if previous.get('request') != request:
            raise ValueError('Wine source path, pin, overlays, configuration or toolchain differs from cached identity')
        if previous.get('source') != identity['source']:
            raise ValueError('effective Wine source differs from cached identity')
        configure = build / 'configure'
        if configure.exists() and previous.get('configure_sha256') != file_hash(configure):
            raise ValueError('generated configure differs from recorded source identity')
        print('Wine build identity verified:', source, identity['source']['head'])
        return
    caches = [build / 'configure', build / 'wine-native/Makefile', build / 'wine-native/config.status']
    caches += list(build.glob('wine-ohos-*/Makefile')) + list(build.glob('wine-ohos-*/config.status'))
    if args.mode == 'check':
        if args.require_manifest or any(path.exists() for path in caches):
            raise ValueError('existing Wine cache has no provenance manifest (including generated configure)')
        print('Wine build identity: clean build directory; source', identity['source']['head'])
        return
    identity['configure_sha256'] = file_hash(build / 'configure')
    identity['main_commit'] = optional_git(args.repo, 'rev-parse', 'HEAD')
    build.mkdir(parents=True, exist_ok=True)
    # Create only, never overwrite a previous manifest or user cache.
    with stamp.open('x') as output:
        json.dump(identity, output, indent=2, sort_keys=True)
        output.write('\n')
    print('Wine effective-source manifest:', stamp)


if __name__ == '__main__':
    try:
        main()
    except (ValueError, OSError, subprocess.SubprocessError) as error:
        print(f'error: {error}\nChoose a new isolated BUILD_DIR. No source or cache was reset or overwritten.', file=sys.stderr)
        sys.exit(1)
