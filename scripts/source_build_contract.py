#!/usr/bin/env python3
"""Verify source-built candidates without claiming device acceptance of new bytes.

The build checks the committed root before compilation, then captures effective
vendor overlays after dependency producers and before assembly. This receipt
pairs rebuilt modules with those exact sources without device qualification.
"""
import argparse
import copy
import hashlib
import io
import json
import os
from pathlib import Path
import re
import subprocess
from zipfile import ZipFile

from dxvk_runtime_contract import ROOT, source_digest
from fex_simd_runtime import pe_kind
from normalize_runtime_identity import load_identity
from wined3d_runtime_contract import source_contract

RECORD = 'source-build-candidate.json'
POLICY = 'source-build-candidate'
# These modules are compiled by Hvigor after assemble.sh, not by build_native.sh.
# Their final identities must come from the first source-built HAP and be paired
# into the final HAP. No other missing dependency may be deferred.
CMAKE_MODULES = {'libs/arm64-v8a/' + name + '.so' for name in (
    'libvirgl_child', 'libentry', 'libwine_child',
    'libxwayland_child', 'libxclient_child', 'libxim_server_child', 'libdisplay_route_napi')}


def sha(data):
    return hashlib.sha256(data).hexdigest()


def elf_identity(data):
    # Keep section names, flags, addresses and all loadable bytes; tolerate only
    # the non-loadable metadata removed by Hvigor's release strip step.
    return sha(repr(load_identity(data)).encode())


def validate_binary(name, data):
    if name.startswith('libs/'):
        if data[:6] != b'\x7fELF\x02\x01' or int.from_bytes(data[18:20], 'little') != 183:
            raise ValueError('candidate native library is not AArch64 ELF: ' + name)
    else:
        kind = pe_kind(data)
        allowed = ('x86',) if '/i386-windows/' in name or '/x86/' in name else ('arm64', 'arm64ec', 'arm64x')
        # VKD3D deliberately stays x64 for FEX (see assemble.sh); Wine's
        # xtajit64 EC entry stub may carry AMD64 without an EC load-config.
        # Do not broaden this to DXVK's x64 aliases: those must remain ARM64X.
        if name == 'vkd3d/limited-500k/x64/d3d12.dll':
            allowed = ('x64',)
        elif name == 'bin/aarch64-windows/xtajit64.dll':
            allowed += ('x64',)
        if kind not in allowed:
            raise ValueError('candidate PE architecture mismatch: ' + name)


def validate_member(record, section, name, data):
    item = record[section][name]
    validate_binary(name, data)
    if section == 'libraries':
        if elf_identity(data) != item['loadableSha256']:
            raise ValueError('candidate loadable ELF changed: ' + name)
    elif sha(data) != item['sha256'] or len(data) != item['bytes']:
        raise ValueError('candidate payload changed: ' + name)


def required_members():
    core = json.loads((ROOT / 'docs/graphics/runtime-core.lock.json').read_text())
    texture = json.loads((ROOT / 'docs/graphics/texture-compat-runtime.lock.json').read_text())
    payload = set(core['payload'])
    payload.update('bin/' + arch + '/kernelbase.dll' for arch in ('i386-windows', 'aarch64-windows'))
    for profile, pin in texture['runtimes'].items():
        payload.update('dxvk/' + profile + '/' + name for name in pin['files'])
    # Venus is checked as ELF separately, with exact nested/top-level pairing.
    return set(core['libraries']) | CMAKE_MODULES, payload


def verify_native_targets(names):
    actual = set(names)
    if actual != CMAKE_MODULES:
        raise ValueError('Hvigor Native inventory differs from the source receipt; missing: ' +
                         ', '.join(sorted(CMAKE_MODULES - actual)) + '; unregistered: ' +
                         ', '.join(sorted(actual - CMAKE_MODULES)))


def source_identity(manifest):
    commit = subprocess.check_output(['git', '-C', str(ROOT), 'rev-parse', 'HEAD'], text=True).strip()
    if manifest['commit'] != commit:
        raise ValueError('candidate source manifest belongs to another checkout')
    changed = subprocess.check_output(['git', '-C', str(ROOT), 'diff', '--name-only',
                                       '-z', 'HEAD', '--ignore-submodules=all']).decode().split('\0')
    changed = [name for name in changed if name]
    if changed:
        raise ValueError('candidate tracked source changed during build: ' + ', '.join(changed))
    patches = {str(p.relative_to(ROOT)): sha(p.read_bytes()) for p in sorted((ROOT / 'patches').rglob('*.patch'))}
    if patches != manifest['patches']:
        raise ValueError('candidate patches differ from clean-checkout manifest')
    for name, pin in manifest['submodules'].items():
        if manifest.get('effectiveSources', {}).get(name, {}).get('sourceState') == 'uninitialized':
            continue  # verify_effective below checks that it stayed uninitialized.
        actual = subprocess.check_output(['git', '-C', str(ROOT / name), 'rev-parse', 'HEAD'], text=True).strip()
        if actual != pin:
            raise ValueError('candidate gitlink changed: ' + name)
    if manifest.get('sourceMode') == 'local-effective-source':
        from local_source_receipt import verify_effective
        verify_effective(ROOT, manifest)
    texture = json.loads((ROOT / 'docs/graphics/texture-compat-runtime.lock.json').read_text())
    dxvk = {}
    for profile, pin in texture['runtimes'].items():
        name = 'thirdparty/' + ('dxvk' if profile == 'legacy' else 'dxvk-modern')
        expected_source = pin['sourceSha256']
        if profile == 'legacy':
            from apply_dxvk_compat_patches import apply
            expected_source = apply(ROOT / name, verify_only=True)
        if manifest['submodules'][name] != pin['commit'] or source_digest(ROOT / name / 'src') != expected_source:
            raise ValueError('candidate DXVK source differs from reviewed source: ' + profile)
        dxvk[profile] = {'commit': pin['commit'], 'sourceSha256': expected_source}
    if manifest['submodules']['thirdparty/mesa'] != texture['venus']['commit']:
        raise ValueError('candidate Mesa source differs from reviewed source')
    for patch in texture['venus']['patches']:
        path = ROOT / patch['path']
        if sha(path.read_bytes().replace(b'\r\n', b'\n')) != patch['sha256']:
            raise ValueError('candidate Mesa overlay changed: ' + patch['path'])
    from apply_dxvk_compat_patches import stack_applied
    if not stack_applied(ROOT / 'thirdparty/mesa',
                         [ROOT / p['path'] for p in texture['venus']['patches']]):
        raise ValueError('candidate Mesa reviewed patch stack is missing or has drifted')
    from prepare_runtime_sources import load, verify
    selected = {}
    for component, variable in (('wine', 'WINE_SRC'), ('fex', 'FEX_SRC')):
        source = Path(os.environ.get(variable, ROOT / 'build/runtime-sources' / component))
        lock, _ = load(component)
        verify(source, lock)
        selected[component] = {'commit': lock['adaptedCommit'], 'tree': lock['adaptedTree']}
    return {'checkout': manifest, 'dxvk': dxvk, 'runtimeRecipes': selected,
            'wineD3D': source_contract(Path(os.environ.get('WINE_SRC', ROOT / 'build/runtime-sources/wine')))}


def record(directory, libs, manifest, compilers):
    sources = source_identity(manifest)
    result = {'schemaVersion': 1, 'policy': POLICY, 'deviceValidated': False,
              'sources': sources, 'libraries': {}, 'payload': {},
              'pendingNative': [],
              'compilers': [{'path': str(p), 'sha256': sha(p.read_bytes())} for p in compilers]}
    libraries, payload = required_members()
    for section, names in (('libraries', libraries), ('payload', payload)):
        for name in sorted(names):
            path = libs / Path(name).relative_to('libs') if section == 'libraries' else directory / name
            if section == 'libraries' and name in CMAKE_MODULES:
                result['pendingNative'].append(name)
                continue
            data = path.read_bytes()
            validate_binary(name, data)
            result[section][name] = {'sha256': sha(data), 'bytes': len(data)}
            if section == 'libraries':
                result[section][name]['loadableSha256'] = elf_identity(data)
    icd = directory / 'bin/guest_vulkan/lib/libvulkan_virtio.so'
    result['venusSha256'] = sha(icd.read_bytes())
    result['venusLoadableSha256'] = elf_identity(icd.read_bytes())
    (directory / RECORD).write_text(json.dumps(result, indent=2) + '\n')
    return result


def load(read, manifest, require_native=True):
    value = json.loads(read(RECORD))
    if (value.get('schemaVersion') != 1 or value.get('policy') != POLICY or
            value.get('deviceValidated') is not False or value['sources']['checkout'] != manifest):
        raise ValueError('candidate receipt does not match the clean-checkout manifest')
    libraries, payload = required_members()
    pending = set(value.get('pendingNative', []))
    if (not pending <= CMAKE_MODULES or (require_native and pending) or
            pending & set(value.get('libraries', {})) or
            set(value.get('libraries', {})) | pending != libraries or set(value.get('payload', {})) != payload):
        raise ValueError('candidate receipt omits required compatibility modules')
    if not value.get('compilers'):
        raise ValueError('candidate receipt lacks compiler identities')
    return value


def complete_native(value, read):
    for name in value.get('pendingNative', []):
        if name not in CMAKE_MODULES:
            raise ValueError('cannot defer non-CMake runtime: ' + name)
        data = read(name)
        validate_binary(name, data)
        value['libraries'][name] = {'sha256': sha(data), 'bytes': len(data),
                                    'loadableSha256': elf_identity(data)}
    value['pendingNative'] = []


def finalize(hap_path, rawfile, manifest):
    path = rawfile / 'wine-data.zip'
    outer_path = rawfile / 'wine-runtime-manifest.json'
    outer = json.loads(outer_path.read_text())
    before = path.read_bytes()
    if sha(before) != outer['payloadSha256'] or outer.get('validationPolicy') != POLICY:
        raise ValueError('cannot finalize a stale or non-candidate runtime')
    with ZipFile(hap_path) as hap, ZipFile(io.BytesIO(before)) as source:
        if sha(hap.read('resources/rawfile/wine-data.zip')) != sha(before):
            raise ValueError('native build HAP contains another runtime')
        value = load(source.read, manifest, require_native=False)
        complete_native(value, hap.read)
        buffer = io.BytesIO()
        with ZipFile(buffer, 'w', allowZip64=True) as output:
            for item in source.infolist():
                output.writestr(copy.copy(item), (json.dumps(value, indent=2) + '\n').encode()
                                if item.filename == RECORD else source.read(item.filename))
    data = buffer.getvalue()
    with ZipFile(io.BytesIO(data)) as output:
        load(output.read, manifest)
    outer['payloadSha256'] = sha(data)
    version_path = rawfile / 'wine_runtime.version'
    version, count = re.subn(r'content=[^;\s]+', 'content=' + sha(data)[:16], version_path.read_text())
    if count != 1:
        raise ValueError('candidate runtime must have one content marker')
    for target, contents in ((path, data), (outer_path, (json.dumps(outer, indent=2) + '\n').encode()),
                             (version_path, version.encode())):
        temporary = target.with_name(target.name + '.candidate.tmp')
        temporary.write_bytes(contents)
        temporary.replace(target)
    print('Hvigor native module identities paired; final HAP must pass the complete candidate gate')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--directory', type=Path)
    parser.add_argument('--libs', type=Path)
    parser.add_argument('--finalize-hap', type=Path)
    parser.add_argument('--rawfile', type=Path)
    parser.add_argument('--source-manifest', type=Path, required=True)
    parser.add_argument('--compiler', type=Path, action='append')
    args = parser.parse_args()
    manifest = json.loads(args.source_manifest.read_text())
    if args.finalize_hap:
        if not args.rawfile:
            parser.error('--finalize-hap requires --rawfile')
        finalize(args.finalize_hap, args.rawfile, manifest)
    else:
        if not args.directory or not args.libs or not args.compiler:
            parser.error('record requires --directory, --libs and --compiler')
        record(args.directory, args.libs, manifest, args.compiler)
        print('Source-build candidate recorded; deviceValidated=false; reviewed release locks unchanged')
