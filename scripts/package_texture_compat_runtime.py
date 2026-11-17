#!/usr/bin/env python3
"""Stage the pinned BC texture and Venus fixes into an existing runtime.

The profile directory contains legacy/{arm64x,x64,x86} and
modern-2.6/{arm64x,x64,x86}. Signing remains a separate step.
"""
import argparse
import copy
import hashlib
import io
import json
from pathlib import Path
import re
import subprocess
from zipfile import ZipFile

from dxvk_runtime_contract import ROOT, source_digest, validate_modern_runtime

LOCK = ROOT / 'docs/graphics/texture-compat-runtime.lock.json'


def sha(data):
    return hashlib.sha256(data).hexdigest()


def encoded(value):
    return (json.dumps(value, indent=2) + '\n').encode()


def validate(runtime, icd, lock=None, candidate=None):
    lock = lock or json.loads(LOCK.read_text())
    if lock.get('schemaVersion') != 1:
        raise ValueError('unsupported texture runtime lock')
    manifest = json.loads(runtime.read('dxvk/manifest.json'))
    for profile, pin in lock['runtimes'].items():
        record = manifest['runtimes'][profile]
        if record['commit'] != pin['commit']:
            raise ValueError('unpaired DXVK commit: ' + profile)
        for member, digest in pin['files'].items():
            arch, name = member.split('/')
            if candidate is not None:
                from source_build_contract import validate_member
                path = 'dxvk/' + profile + '/' + member
                validate_member(candidate, 'payload', path, runtime.read(path))
                digest = candidate['payload'][path]['sha256']
            if sha(runtime.read('dxvk/' + profile + '/' + member)) != digest or record[arch][name] != digest:
                raise ValueError('stale DXVK texture runtime: ' + profile + '/' + member)
        if pin.get('patches') and (record.get('managedPatches') != pin['patches']
                                   or record.get('effectiveSourceSha256') != pin['patchedSourceSha256']):
            raise ValueError('stale DXVK managed overlay metadata: ' + profile)
        if pin.get('qualification') and record.get('textureCompatibility') != pin['qualification']:
            raise ValueError('unpaired DXVK validation status: ' + profile)
    expected = lock['venus']['sha256']
    guest = json.loads(runtime.read('bin/guest_vulkan/manifest.json'))
    top_matches = sha(icd) == expected
    if candidate is not None:
        from source_build_contract import elf_identity
        expected = candidate['venusSha256']
        top_matches = elf_identity(icd) == candidate['venusLoadableSha256']
    if not top_matches or sha(runtime.read('bin/guest_vulkan/lib/libvulkan_virtio.so')) != expected:
        raise ValueError('unpaired top-level and nested Venus ICD')
    if guest['files']['lib/libvulkan_virtio.so'] != expected or manifest['venusRuntime']['guestIcdSha256'] != expected:
        raise ValueError('stale Venus runtime metadata')
    if guest['guestMesaCommit'] != lock['venus']['commit']:
        raise ValueError('stale Venus source identity')
    validate_modern_runtime(runtime, candidate=candidate)


def package(rawfile, profiles, icd_path, host_path):
    lock = json.loads(LOCK.read_text())
    replacement = {}
    for profile, pin in lock['runtimes'].items():
        source = ROOT / 'thirdparty' / ('dxvk' if profile == 'legacy' else 'dxvk-modern')
        commit = subprocess.check_output(['git', '-c', 'safe.directory=' + str(source),
                                          '-C', str(source), 'rev-parse', 'HEAD'], text=True).strip()
        expected_source = pin['sourceSha256']
        if profile == 'legacy':
            from apply_dxvk_compat_patches import apply
            expected_source = apply(source, verify_only=True)
        if commit != pin['commit'] or source_digest(source / 'src') != expected_source:
            raise ValueError('DXVK source differs from texture lock: ' + profile)
        for member, digest in pin['files'].items():
            data = (profiles / profile / member).read_bytes()
            if sha(data) != digest:
                raise ValueError('unreviewed texture DLL: ' + profile + '/' + member)
            replacement['dxvk/' + profile + '/' + member] = data
    for patch in lock['venus']['patches']:
        if sha((ROOT / patch['path']).read_bytes().replace(b'\r\n', b'\n')) != patch['sha256']:
            raise ValueError('changed Mesa overlay: ' + patch['path'])
    icd = icd_path.read_bytes()
    if sha(icd) != lock['venus']['sha256'] or icd[:4] != b'\x7fELF' or int.from_bytes(icd[18:20], 'little') != 183:
        raise ValueError('unreviewed or non-AArch64 Venus ICD')
    replacement['bin/guest_vulkan/lib/libvulkan_virtio.so'] = icd
    output = io.BytesIO()
    with ZipFile(rawfile / 'wine-data.zip') as original:
        manifest = json.loads(original.read('dxvk/manifest.json'))
        for profile, pin in lock['runtimes'].items():
            record = manifest['runtimes'][profile]
            record['commit'] = pin['commit']
            record['textureCompatibility'] = pin.get('qualification',
                record.get('textureCompatibility', 'host-validated-device-pending'))
            if pin.get('patches'):
                record['managedPatches'] = pin['patches']
                record['effectiveSourceSha256'] = pin['patchedSourceSha256']
            for member, digest in pin['files'].items():
                arch, name = member.split('/')
                record.setdefault(arch, {})[name] = digest
        venus = manifest['venusRuntime']
        venus['guestMesaCommit'] = lock['venus']['commit']
        venus['guestIcdSha256'] = sha(icd)
        venus['hostVirglrendererSha256'] = sha(host_path.read_bytes())
        venus['id'] = 'venus-' + sha(icd)[:12] + '-' + venus['hostVirglrendererSha256'][:12]
        replacement['dxvk/manifest.json'] = encoded(manifest)
        guest = json.loads(original.read('bin/guest_vulkan/manifest.json'))
        guest['guestMesaCommit'] = lock['venus']['commit']
        guest['files']['lib/libvulkan_virtio.so'] = sha(icd)
        guest['managedPatches'] = lock['venus']['patches']
        replacement['bin/guest_vulkan/manifest.json'] = encoded(guest)
        missing = replacement.keys() - set(original.namelist())
        if missing:
            raise ValueError('base runtime lacks required members: ' + ', '.join(sorted(missing)))
        with ZipFile(output, 'w') as result:
            for member in original.infolist():
                result.writestr(copy.copy(member), replacement.get(member.filename, original.read(member)))
    payload = output.getvalue()
    with ZipFile(io.BytesIO(payload)) as result:
        validate(result, icd, lock)
    outer = json.loads((rawfile / 'wine-runtime-manifest.json').read_text())
    outer['payloadSha256'] = sha(payload)
    outer['textureCompatibility'] = {'lockSha256': sha(LOCK.read_bytes()), 'guestIcdSha256': sha(icd)}
    graphics = ROOT / 'docs/graphics/graphics-stack.lock.yaml'
    if 'graphicsMigration' in outer:
        outer['graphicsMigration']['lockSha256'] = sha(graphics.read_bytes())
    version = (rawfile / 'wine_runtime.version').read_text()
    version, count = re.subn(r'content=[^;\s]+', 'content=' + sha(payload)[:16], version)
    if count != 1:
        raise ValueError('runtime extraction version must contain one content marker')
    version = re.sub(r'graphics-migration=[^;\s]+', 'graphics-migration=' + sha(graphics.read_bytes())[:16], version)
    for name, data in [('wine-data.zip', payload), ('wine-runtime-manifest.json', encoded(outer)),
                       ('wine_runtime.version', version.encode())]:
        target = rawfile / name
        temporary = target.with_name(target.name + '.texture.tmp')
        temporary.write_bytes(data)
        temporary.replace(target)
    print('Texture runtime PASS: pinned DXVK DLLs, paired Venus ICD, nested manifests and extraction version')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--rawfile', type=Path, required=True)
    parser.add_argument('--profiles', type=Path, required=True)
    parser.add_argument('--guest-icd', type=Path, required=True)
    parser.add_argument('--host-virgl', type=Path, required=True)
    args = parser.parse_args()
    package(args.rawfile, args.profiles, args.guest_icd, args.host_virgl)
