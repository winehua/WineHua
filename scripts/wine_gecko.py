#!/usr/bin/env python3
"""Prepare the pinned x86/x64 Gecko shared directories; never run an installer."""
import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import struct
import tarfile
import urllib.request

LOCK_PATH = Path(__file__).with_name('wine_gecko.lock.json')


def load_lock():
    return json.loads(LOCK_PATH.read_text())


def digest(data):
    return hashlib.sha256(data).hexdigest()


def file_digest(path):
    h = hashlib.sha256()
    with path.open('rb') as f:
        for block in iter(lambda: f.read(1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()


def directory_name(lock, arch):
    return f'wine-gecko-{lock["version"]}-{arch}'


def component_manifest(lock):
    return {'status': 'bundled', 'version': lock['version'], 'format': 'shared-directory',
            'architectures': {arch: {
                'directory': 'share/wine/gecko/' + directory_name(lock, arch),
                'archiveSha256': item['sha256'],
                'fileCount': len(item['files']),
                'bytes': sum(f['bytes'] for f in item['files'].values()),
                'contentSha256': digest(json.dumps(item['files'], sort_keys=True,
                                                 separators=(',', ':')).encode()),
            } for arch, item in lock['archives'].items()}}


def check_wine_version(source, lock):
    # Keep both the mshtml loader and the optional installer paired on upgrades.
    for name in ('dlls/mshtml/nsiface.idl', 'dlls/appwiz.cpl/addons.c'):
        text = (Path(source) / name).read_text().replace('\\"', '"')
        match = re.search(r'#define\s+GECKO_VERSION\s+"([^"]+)"', text)
        if not match or match[1] != lock['version']:
            raise ValueError('Wine Gecko source version mismatch: ' + name)


def safe_name(name):
    path = PurePosixPath(name)
    if not name or path.is_absolute() or '..' in path.parts or '\\' in name or ':' in name:
        raise ValueError('Unsafe Gecko archive path: ' + name)
    if path.as_posix() != name.rstrip('/'):
        raise ValueError('Noncanonical Gecko archive path: ' + name)
    return path


def check_file(name, data, expected, arch, version):
    if len(data) != expected['bytes'] or digest(data) != expected['sha256']:
        raise ValueError('Gecko payload checksum mismatch: ' + name)
    if name == 'VERSION' and data != ('Wine Gecko ' + version).encode():
        raise ValueError('Gecko VERSION mismatch')
    if name.endswith(('.dll', '.exe')):
        offset = int.from_bytes(data[60:64], 'little')
        machine = 0x14c if arch == 'x86' else 0x8664
        if (data[:2] != b'MZ' or data[offset:offset + 4] != b'PE\0\0'
                or data[offset + 4:offset + 6] != struct.pack('<H', machine)):
            raise ValueError('Gecko PE architecture mismatch: ' + name)


def check_directory(directory, item, arch, version):
    if directory.is_symlink() or not directory.is_dir():
        raise ValueError('Missing or linked Gecko directory: ' + str(directory))
    files = {}
    for p in directory.rglob('*'):
        if p.is_symlink() or not (p.is_file() or p.is_dir()):
            raise ValueError('Linked or special Gecko file: ' + str(p))
        if p.is_file():
            files[p.relative_to(directory).as_posix()] = p
    if set(files) != set(item['files']):
        raise ValueError('Gecko file inventory mismatch: ' + arch)
    for name, p in files.items():
        check_file(name, p.read_bytes(), item['files'][name], arch, version)


def validate_payload(names, read, manifest, lock=None):
    lock = lock or load_lock()
    if manifest != component_manifest(lock):
        raise ValueError('Wine Gecko manifest differs from pinned dual-architecture runtime')
    expected = {}
    for arch, item in lock['archives'].items():
        base = 'share/wine/gecko/' + directory_name(lock, arch) + '/'
        expected.update({base + name: (arch, name, entry) for name, entry in item['files'].items()})
    actual = {n for n in names if n.startswith('share/wine/gecko/') and not n.endswith('/')}
    if actual != set(expected):
        raise ValueError('Wine Gecko packaged inventory mismatch')
    for path, (arch, name, entry) in expected.items():
        check_file(name, read(path), entry, arch, lock['version'])
    # Check the actual dual-view Wine DLL, not the host CPU or directory label.
    for arch, slot in (('x86', 'i386-windows'), ('x86_64',
                        'aarch64-windows' if 'bin/aarch64-windows/mshtml.dll' in names else 'x86_64-windows')):
        data = read('bin/' + slot + '/mshtml.dll')
        if directory_name(lock, arch).encode('utf-16le') not in data:
            raise ValueError('Wine mshtml does not load pinned Gecko: ' + arch)


def download(directory, item, lock):
    target = directory / item['file']
    pending = target.with_suffix(target.suffix + '.part')
    if target.is_symlink() or pending.is_symlink():
        raise ValueError('Linked Gecko archive rejected')
    if target.is_file() and target.stat().st_size == item['bytes'] and file_digest(target) == item['sha256']:
        return target
    try:
        with urllib.request.urlopen(lock['source'] + item['file'], timeout=45) as src, pending.open('wb') as dst:
            count = 0
            for block in iter(lambda: src.read(1024 * 1024), b''):
                count += len(block)
                if count > item['bytes']:
                    raise ValueError('Oversized Gecko archive')
                dst.write(block)
        if pending.stat().st_size != item['bytes'] or file_digest(pending) != item['sha256']:
            raise ValueError('Gecko archive checksum mismatch')
        os.replace(pending, target)
    finally:
        pending.unlink(missing_ok=True)
    return target


def extract(archive, directory, item, arch, lock):
    # Inspect every entry first. No tar.extract/extractall, links or special files.
    root = directory_name(lock, arch)
    with tarfile.open(archive) as tar:
        members = {}
        seen = set()
        for entry in tar:
            path = safe_name(entry.name)
            if path.parts[0] != root or entry.name in seen:
                raise ValueError('Unexpected or duplicate Gecko archive member: ' + entry.name)
            seen.add(entry.name)
            if entry.isdir():
                continue
            name = PurePosixPath(*path.parts[1:]).as_posix()
            if not entry.isfile() or name not in item['files'] or entry.size != item['files'][name]['bytes']:
                raise ValueError('Invalid Gecko archive member: ' + entry.name)
            members[name] = entry
        if set(members) != set(item['files']):
            raise ValueError('Incomplete Gecko archive: ' + arch)
        for name, entry in members.items():
            data = tar.extractfile(entry).read()
            check_file(name, data, item['files'][name], arch, lock['version'])
            target = directory / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
            target.chmod(0o644)


def remove_managed(directory):
    if directory.is_symlink() or any(p.is_symlink() for p in directory.rglob('*')):
        raise ValueError('Refusing to replace linked Gecko cache: ' + str(directory))
    if directory.exists():
        shutil.rmtree(directory)


def prepare(cache, source=None, check_only=False, package_to=None):
    lock = load_lock()
    if source:
        check_wine_version(source, lock)
    cache = Path(cache)
    if cache.is_symlink() or any(p.is_symlink() for p in cache.parents):
        raise ValueError('Linked Gecko cache rejected')
    cache.mkdir(parents=True, exist_ok=True)
    for arch, item in lock['archives'].items():
        directory = cache / directory_name(lock, arch)
        try:
            check_directory(directory, item, arch, lock['version'])
        except ValueError:
            if check_only:
                raise
            archive = download(cache, item, lock)
            pending = cache / (directory.name + '.prepare')
            remove_managed(pending)
            try:
                extract(archive, pending, item, arch, lock)
                check_directory(pending, item, arch, lock['version'])
                remove_managed(directory)
                os.replace(pending, directory)
            finally:
                remove_managed(pending)
        if package_to:
            target = Path(package_to) / directory.name
            remove_managed(target)
            shutil.copytree(directory, target)
    manifest = component_manifest(lock)
    target = cache / 'manifest.json'
    text = json.dumps(manifest, indent=2) + '\n'
    if check_only and (target.is_symlink() or not target.is_file() or target.read_text() != text):
        raise ValueError('Gecko cache manifest differs from pinned runtime')
    if not check_only and (not target.exists() or target.read_text() != text):
        if target.is_symlink():
            raise ValueError('Linked Gecko manifest rejected')
        target.write_text(text)
    return manifest


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--cache', required=True, type=Path)
    p.add_argument('--wine-source', type=Path)
    p.add_argument('--check-only', action='store_true')
    p.add_argument('--package-to', type=Path)
    a = p.parse_args()
    prepare(a.cache, a.wine_source, a.check_only, a.package_to)
    print('Wine Gecko x86 + x86_64 pinned shared runtime verified')


if __name__ == '__main__':
    main()
