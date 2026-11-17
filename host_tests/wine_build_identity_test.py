"""Exercise the production identity guard against disposable configured source trees."""
from pathlib import Path
import json
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
GUARD = ROOT / 'scripts/wine_build_identity.py'
sys.path.insert(0, str(ROOT / 'scripts'))
from wine_build_identity import ordered_overlays

# Exercise the actual production inventory without requiring submodules or SDK.
production = ordered_overlays(ROOT)
recipe = json.loads((ROOT / 'docs/upstream-sync/wine-candidate.lock.json').read_text())
reviewed = [{key: row[key] for key in ('path', 'sha256')} for row in recipe['patches']]
assert production == reviewed, 'production overlay order/content differs from the reviewed graphics lock'


def git(path, *args):
    return subprocess.check_output(['git', '-C', str(path), '-c', 'user.name=Test',
        '-c', 'user.email=test@example.invalid', *args], stderr=subprocess.DEVNULL).decode().strip()


def snapshot(path):
    return {str(p.relative_to(path)): ('link', os.readlink(p)) if p.is_symlink() else ('file', p.read_bytes())
            for p in path.rglob('*') if p.is_file() or p.is_symlink()}


with tempfile.TemporaryDirectory(prefix='wine-identity-') as temporary:
    base = Path(temporary); source = base / 'source with spaces'; source.mkdir()
    git(source, 'init', '-q'); (source / 'configure.ac').write_text('AC_INIT([test], [1])\n')
    (source / 'unit.c').write_text('int version = 1;\n'); git(source, 'add', '.'); git(source, 'commit', '-qm', 'source')
    head = git(source, 'rev-parse', 'HEAD')
    repo = base / 'repo'; (repo / 'scripts').mkdir(parents=True); (repo / 'patches/wine').mkdir(parents=True)
    (repo / 'patches/wine/0001-test.patch').write_text('ordered overlay\n')
    (repo / 'scripts/build_wine.sh').write_text('ensure_wine_patch "$SCRIPT_DIR/../patches/wine/0001-test.patch" "test"\n')
    script = repo / 'scripts/build_wine.sh'
    registration = script.read_text()
    expected = ordered_overlays(repo)
    for expression in ('$ROOT/patches', '${ROOT}/patches', '${SCRIPT_DIR}/../patches'):
        script.write_text(registration.replace('$SCRIPT_DIR/../patches', expression))
        assert ordered_overlays(repo) == expected
    for expression in ('$UNKNOWN/patches', '$ROOT/../outside'):
        script.write_text(registration.replace('$SCRIPT_DIR/../patches', expression))
        try:
            ordered_overlays(repo)
        except ValueError:
            pass
        else:
            raise AssertionError('invalid overlay path accepted: ' + expression)
    script.write_text(registration)
    (repo / 'scripts/env.sh').write_text('configuration\n')
    for name in ('gstreamer_plugins.json', 'gstreamer_inventory.py', 'wine_gstreamer_loader.h',
                 'wine_runtime_diagnostics.py', 'wine_runtime_diagnostics.h'):
        (repo / 'scripts' / name).write_text('GStreamer build input\n')
    git(repo, 'init', '-q'); git(repo, 'add', '.');
    git(repo, 'update-index', '--add', '--cacheinfo', f'160000,{head},thirdparty/wine-valve')
    git(repo, 'commit', '-qm', 'repo')
    build = base / 'build'; build.mkdir()
    link = base / 'source-link'; link.symlink_to(source, target_is_directory=True)
    config = ['--config', 'WINE_ARCH=aarch64', '--config', 'HOST_TRIPLE=aarch64-unknown-linux-ohos']
    def run(mode='check', expected=None, use_source=source, extra=None):
        before = snapshot(base)
        command = ['python3', str(GUARD), mode, '--source', str(use_source), '--build', str(build), '--repo', str(repo)]
        result = subprocess.run(command + (config if extra is None else extra), capture_output=True, text=True)
        if expected is None:
            assert result.returncode == 0, result.stdout + result.stderr
        else:
            assert result.returncode != 0 and expected in result.stderr, result.stdout + result.stderr
        if mode == 'check' or expected is not None:
            after = snapshot(base)
            assert after == before, 'guard mutated: ' + repr([name for name in before.keys() | after.keys() if before.get(name) != after.get(name)])
        return result
    run(); (build / 'configure').write_text('generated configure\n')
    run(expected='no provenance manifest')
    run('record')
    manifest = json.loads((build / 'wine-build-identity.json').read_text())
    assert manifest['source']['head'] == head == manifest['request']['active_gitlink']
    assert manifest['request']['ordered_overlays'][0]['path'] == 'patches/wine/0001-test.patch'
    run(use_source=link)
    for name in ('wine-native', 'wine-ohos-aarch64'):
        directory = build / name; directory.mkdir()
        (directory / 'Makefile').write_text(f'srcdir = {source}\n')
        (directory / 'config.status').write_text(f'S["srcdir"]="{source}"\n')
    run()
    for path in (build / 'wine-native/Makefile', build / 'wine-ohos-aarch64/Makefile',
                 build / 'wine-native/config.status', build / 'wine-ohos-aarch64/config.status'):
        original = path.read_text(); path.write_text(f'srcdir = {base / "foreign source"}\n')
        run(expected='cached srcdir mismatch'); path.write_text(original)
    run(extra=['--config', 'WINE_ARCH=x86_64'], expected='configuration or toolchain differs')
    for name in ('gstreamer_plugins.json', 'gstreamer_inventory.py', 'wine_gstreamer_loader.h',
                 'wine_runtime_diagnostics.py', 'wine_runtime_diagnostics.h'):
        asset=repo/'scripts'/name; original=asset.read_text(); asset.write_text(original+'changed\n')
        run(expected='overlays, configuration'); asset.write_text(original)
    patch = repo / 'patches/wine/0001-test.patch'; original = patch.read_text(); patch.write_text(original + 'changed\n')
    run(expected='overlays, configuration'); patch.write_text(original)
    unit = source / 'unit.c'; original = unit.read_text(); unit.write_text('int version = 2;\n')
    run(expected='effective Wine source differs'); unit.write_text(original)
    generated = build / 'configure'; original = generated.read_text(); generated.write_text('foreign configure\n')
    run(expected='generated configure differs'); generated.write_text(original)
    previous_build=build; relocated=base/'relocated-build';shutil.copytree(build,relocated);build=relocated
    run(expected='configuration or toolchain differs');build=previous_build
    gitdir=source/'.git'; missing=base/'saved-git'; gitdir.rename(missing)
    run(expected='effective Wine source differs')
    previous_build=build; build=base/'no-git-build'; build.mkdir()
    run(); (build/'configure').write_text('generated configure without Git\n'); run('record'); run()
    fallback=json.loads((build/'wine-build-identity.json').read_text())
    assert fallback['source']['basis']=='full-source-tree-sha256' and fallback['source']['head'] is None
    unit.write_text('new unseen source edit\n');run(expected='effective Wine source differs');unit.write_text('int version = 1;\n')
    build=previous_build;missing.rename(gitdir)
    run()
print('Wine cache identity: source/configure/native/OHOS/arch/overlay/source edits/symlinks/spaces, all preservation checks passed')
