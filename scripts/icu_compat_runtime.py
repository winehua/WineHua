"""Stage the standalone Wine ICU implementation and its matching Unicode data."""
import argparse
import copy
import hashlib
import io
import json
from pathlib import Path
import re
from zipfile import ZipFile
from fex_simd_runtime import pe_kind

PIN = 'dc5204ecb0c3c1bf6624542ecf42f931fcc1a3b0'
RECORD = 'icu-compat.json'
MODULES = ['icu.dll', 'icuin.dll', 'icuuc.dll']

def sha(data): return hashlib.sha256(data).hexdigest()

def inputs(output):
    if (output / 'memory-call-policy').read_text().strip() != 'typed-arm64ec-imports-v1':
        raise ValueError('ICU component memory-call build policy mismatch')
    if (output / 'source-pin').read_text().strip() != PIN:
        raise ValueError('ICU component source identity mismatch')
    data = {f'bin/{arch}/{name}': (output / arch / name).read_bytes()
            for arch in ('aarch64-windows', 'i386-windows') for name in MODULES}
    data['share/wine/nls/icudtl.dat'] = (output / 'nls/icudtl.dat').read_bytes()
    record = {'schemaVersion': 1, 'implementation': 'wine-embedded-icu', 'sourcePin': PIN, 'memoryCallPolicy': 'typed-arm64ec-imports-v1',
              'files': {name: sha(value) for name, value in data.items()}}
    validate(record, data.__getitem__)
    data[RECORD] = (json.dumps(record, indent=2) + '\n').encode()
    return data

def validate(record, read):
    expected = {f'bin/{arch}/{name}' for arch in ('aarch64-windows', 'i386-windows') for name in MODULES}
    expected.add('share/wine/nls/icudtl.dat')
    if record.get('schemaVersion') != 1 or record.get('implementation') != 'wine-embedded-icu' or record.get('sourcePin') != PIN:
        raise ValueError('unsupported ICU runtime record')
    if record.get('memoryCallPolicy') != 'typed-arm64ec-imports-v1':
        raise ValueError('ICU runtime lacks typed ARM64EC memory imports')
    if set(record.get('files', {})) != expected:
        raise ValueError('incomplete ICU runtime and Unicode data')
    for name, digest in record['files'].items():
        data = read(name)
        if sha(data) != digest or not data:
            raise ValueError('ICU runtime checksum mismatch: ' + name)
        if name.endswith('.dll') and (data[:2] != b'MZ' or data[64:80] != b'Wine builtin DLL' or
                pe_kind(data) not in (('arm64x',) if name == 'bin/aarch64-windows/icu.dll' else ('arm64', 'arm64x') if '/aarch64-windows/' in name else ('x86',))):
            raise ValueError('expected a Wine builtin ICU module: ' + name)

def stage(output, directory):
    for name, value in inputs(output).items():
        target = directory / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(value)

def update_rawfile(output, rawfile):
    updates = inputs(output)
    payload_path = rawfile / 'wine-data.zip'
    manifest_path = rawfile / 'wine-runtime-manifest.json'
    old = payload_path.read_bytes()
    manifest = json.loads(manifest_path.read_text())
    if manifest['payloadSha256'] != sha(old): raise ValueError('base payload checksum mismatch')
    buffer = io.BytesIO()
    with ZipFile(io.BytesIO(old)) as before, ZipFile(buffer, 'w', allowZip64=True) as after:
        names = before.namelist()
        if len(names) != len(set(names)): raise ValueError('duplicate base runtime members')
        for entry in before.infolist():
            after.writestr(copy.copy(entry), updates.get(entry.filename, before.read(entry)))
        for name in sorted(set(updates) - set(names)):
            after.writestr(name, updates[name])
    new = buffer.getvalue()
    with ZipFile(io.BytesIO(old)) as before, ZipFile(io.BytesIO(new)) as after:
        assert set(after.namelist()) == set(before.namelist()) | set(updates)
        changed = [name for name in before.namelist() if before.read(name) != after.read(name)]
        assert set(changed) <= set(updates)
        validate(json.loads(after.read(RECORD)), after.read)
    manifest['payloadSha256'] = sha(new)
    version_path = rawfile / 'wine_runtime.version'
    version = re.sub(r';icu=[^;\s]*', '', version_path.read_text().strip())
    version = re.sub(r'content=[^;\s]*', 'content=' + sha(new)[:16], version)
    version += ';icu=' + sha(updates[RECORD])[:16]
    temp = payload_path.with_suffix('.zip.icu-update')
    temp.write_bytes(new); temp.replace(payload_path)
    manifest_path.write_text(json.dumps(manifest, indent=2) + '\n')
    version_path.write_text(version + '\n')
    print(json.dumps({'changedPayloadMembers': changed,
                      'addedPayloadMembers': sorted(set(updates) - set(names)),
                      'payloadSha256': sha(new)}, indent=2))

if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('mode', choices=['stage', 'update-rawfile', 'check'])
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--directory', type=Path)
    args = p.parse_args()
    if args.mode == 'check': inputs(args.output)
    elif args.directory is None: p.error('--directory is required for staging')
    elif args.mode == 'stage': stage(args.output, args.directory)
    else: update_rawfile(args.output, args.directory)
