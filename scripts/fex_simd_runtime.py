"""Record and stage paired FEX correctness repairs without changing other runtime members."""
import argparse
import copy
import hashlib
import io
import json
from pathlib import Path
import re
import struct
from zipfile import ZipFile

# Keep the established filename for existing package readers.
RECORD = 'fex-simd-zero.json'
REPAIR_VERSION = 2
SMC_COMMIT = 'b21c49352ea56f23ececbe3c87f0fc681be8024b'
SMC_PATCH = Path(__file__).resolve().parent / 'patches/fex-smc-block-state.patch'
MEMBERS = {'arm64ec': 'bin/aarch64-windows/libarm64ecfex.dll',
           'wow64': 'bin/aarch64-windows/libwow64fex.dll'}

def sha(data):
    return hashlib.sha256(data).hexdigest()

def pe_kind(data):
    def unpack(fmt, position):
        if position < 0 or position + struct.calcsize(fmt) > len(data):
            raise ValueError('truncated PE metadata')
        return struct.unpack_from(fmt, data, position)
    if len(data) < 64 or data[:2] != b'MZ':
        raise ValueError('invalid PE module')
    pe = unpack('<I', 60)[0]
    if data[pe:pe+4] != b'PE\0\0':
        raise ValueError('invalid PE signature')
    machine, count = unpack('<HH', pe + 4)
    optional_size = unpack('<H', pe + 20)[0]
    optional = pe + 24
    sections = optional + optional_size
    unpack('<40s', sections + 40*(count-1)) if count else None
    def offset(rva, size=1):
        for i in range(count):
            section = sections + 40*i
            virtual_size, address, raw_size, raw = unpack('<IIII', section + 8)
            if address <= rva and rva + size <= address + min(virtual_size, raw_size):
                position = raw + rva - address
                if position + size <= len(data): return position
        raise ValueError('PE RVA is not backed by file data')
    if machine in (0x8664, 0xaa64) and unpack('<H', optional)[0] == 0x20b:
        if optional_size < 112 + 11*8: raise ValueError('truncated PE64 directories')
        rva, size = unpack('<II', optional + 112 + 10*8)
        if rva and size >= 208:
            config = offset(rva, size)
            config_size = unpack('<I', config)[0]
            if config_size > size: raise ValueError('truncated PE load config')
            if machine == 0x8664 and config_size >= 208:
                # EC images use AMD64 on disk and identify the EC view via
                # CHPE metadata. Do not accept an ordinary x64 DLL as FEX EC.
                chpe = unpack('<Q', config + 200)[0]
                base = unpack('<Q', optional + 24)[0]
                if chpe and unpack('<I', offset(chpe - base, 4))[0] == 1:
                    return 'arm64ec'
            if machine == 0xaa64 and config_size >= 232:
                # ARM64X has ARM64 on disk; the ARM64X dynamic relocation
                # table supplies its second view (same recognition as LLVM).
                relative, section = unpack('<IH', config + 224)
                if 1 <= section <= count:
                    header = sections + 40*(section-1)
                    raw_size, raw = unpack('<II', header + 16)
                    table = raw + relative
                    if relative + 20 <= raw_size:
                        version, length = unpack('<II', table)
                        symbol, entry_size = unpack('<QI', table + 8)
                        if version == 1 and symbol == 6 and entry_size <= length - 12 and relative + 8 + length <= raw_size:
                            unpack('<1s', table + 7 + length)
                            return 'arm64x'
    return {0xa641: 'arm64ec', 0xaa64: 'arm64', 0xa64e: 'arm64x',
            0x8664: 'x64', 0x14c: 'x86'}.get(machine, 'unknown')

def validate(record, read):
    if record.get('schemaVersion') != 1 or record.get('repairVersion') not in (1, REPAIR_VERSION):
        raise ValueError('unsupported FEX SIMD repair record')
    if not re.fullmatch('[0-9a-f]{64}', record.get('patchSha256', '')):
        raise ValueError('missing FEX SIMD patch identity')
    if record['repairVersion'] == REPAIR_VERSION:
        fix = record.get('fixes', {}).get('smcBlockState', {})
        if fix.get('upstreamCommit') != SMC_COMMIT or not re.fullmatch('[0-9a-f]{64}', fix.get('patchSha256', '')):
            raise ValueError('missing FEX SMC block-state repair identity')
    if set(record.get('modules', {})) != set(MEMBERS):
        raise ValueError('incomplete FEX SIMD repair pair')
    for key, name in MEMBERS.items():
        item = record['modules'][key]
        if item.get('member') != name:
            raise ValueError('unexpected FEX module path')
        data = read(name)
        if item.get('sha256') != sha(data):
            raise ValueError('FEX SIMD repair module checksum mismatch: ' + key)
        if pe_kind(data) != {'arm64ec': 'arm64ec', 'wow64': 'arm64'}[key]:
            raise ValueError('FEX module architecture mismatch: ' + key)

def inputs(build):
    return {MEMBERS['arm64ec']: (build / 'fex-ec/Bin/libarm64ecfex.dll').read_bytes(),
            MEMBERS['wow64']: (build / 'fex-pe/Bin/libwow64fex.dll').read_bytes()}

def require_current(record):
    if record.get('repairVersion') != REPAIR_VERSION:
        raise ValueError('FEX build predates required SMC repair; rebuild FEX before packaging')
    if record.get('fixes', {}).get('smcBlockState', {}).get('patchSha256') != sha(SMC_PATCH.read_bytes()):
        raise ValueError('FEX SMC patch changed; rebuild FEX before packaging')


def write_record(build, patch, source_hash):
    data = inputs(build)
    record = {'schemaVersion': 1, 'repairVersion': REPAIR_VERSION, 'sourceHash': source_hash,
              'fixes': {'smcBlockState': {'upstreamCommit': SMC_COMMIT, 'patchSha256': sha(SMC_PATCH.read_bytes())}},
              'patchSha256': sha(patch.read_bytes()),
              'modules': {key: {'member': name, 'sha256': sha(data[name])}
                          for key, name in MEMBERS.items()}}
    validate(record, data.__getitem__)
    (build / RECORD).write_text(json.dumps(record, indent=2) + '\n')

def stage(build, directory):
    record = json.loads((build / RECORD).read_text())
    data = inputs(build)
    validate(record, data.__getitem__)
    require_current(record)
    for name, value in data.items():
        target = directory / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(value)
    (directory / RECORD).write_text(json.dumps(record, indent=2) + '\n')

def refresh_staged(directory):
    # Rebind module hashes after package transforms such as PE symbol stripping.
    record_path = directory / RECORD
    record = json.loads(record_path.read_text())
    if record.get('schemaVersion') != 1 or record.get('repairVersion') not in (1, REPAIR_VERSION):
        raise ValueError('unsupported FEX SIMD repair record')
    if set(record.get('modules', {})) != set(MEMBERS):
        raise ValueError('incomplete FEX SIMD repair pair')
    data = {}
    for key, name in MEMBERS.items():
        item = record['modules'][key]
        if item.get('member') != name:
            raise ValueError('unexpected FEX module path')
        data[name] = (directory / name).read_bytes()
        item['sha256'] = sha(data[name])
    validate(record, data.__getitem__)
    temporary = record_path.with_name(record_path.name + '.tmp')
    temporary.write_text(json.dumps(record, indent=2) + '\n')
    temporary.replace(record_path)

def update_rawfile(build, rawfile):
    record_bytes = (build / RECORD).read_bytes()
    record = json.loads(record_bytes)
    updates = inputs(build)
    validate(record, updates.__getitem__)
    require_current(record)
    updates[RECORD] = record_bytes
    payload_path = rawfile / 'wine-data.zip'
    manifest_path = rawfile / 'wine-runtime-manifest.json'
    manifest = json.loads(manifest_path.read_text())
    old = payload_path.read_bytes()
    if manifest['payloadSha256'] != sha(old):
        raise ValueError('base payload identity mismatch')
    buffer = io.BytesIO()
    with ZipFile(io.BytesIO(old)) as before, ZipFile(buffer, 'w', allowZip64=True) as after:
        names = before.namelist()
        if len(names) != len(set(names)) or not set(MEMBERS.values()) <= set(names):
            raise ValueError('ambiguous or incomplete base FEX payload')
        from stamp_wine_builtin_identity import stamp_archive
        updates['share/wine/wine.inf'] = stamp_archive(before, updates)
        for entry in before.infolist():
            after.writestr(copy.copy(entry), updates.get(entry.filename, before.read(entry)))
        if RECORD not in names:
            after.writestr(RECORD, record_bytes)
    new = buffer.getvalue()
    with ZipFile(io.BytesIO(old)) as before, ZipFile(io.BytesIO(new)) as after:
        assert set(after.namelist()) == set(before.namelist()) | {RECORD}
        changed = [name for name in before.namelist() if before.read(name) != after.read(name)]
        assert set(changed) <= set(updates)
        validate(json.loads(after.read(RECORD)), after.read)
    manifest['payloadSha256'] = sha(new)
    version_path = rawfile / 'wine_runtime.version'
    version = re.sub(r';fex-simd-zero=[^;\s]*', '', version_path.read_text().strip())
    version = re.sub(r'content=[^;\s]*', 'content=' + sha(new)[:16], version)
    # The marker makes normal runtime extraction and managed system32 refresh
    # notice this repair even when the application version stays unchanged.
    version += ';fex-simd-zero=' + sha(record_bytes)[:16]
    temp = payload_path.with_suffix('.zip.fex-update')
    temp.write_bytes(new)
    temp.replace(payload_path)
    manifest_path.write_text(json.dumps(manifest, indent=2) + '\n')
    version_path.write_text(version + '\n')
    print(json.dumps({'changedPayloadMembers': changed, 'addedRepairRecord': RECORD not in names,
                      'payloadSha256': sha(new), 'repair': record}, indent=2))

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mode', choices=['record', 'stage', 'refresh-staged', 'update-rawfile'])
    parser.add_argument('--build', type=Path)
    parser.add_argument('--patch', type=Path)
    parser.add_argument('--source-hash', default='detect')
    parser.add_argument('--directory', type=Path)
    args = parser.parse_args()
    if args.mode == 'record':
        if args.build is None:
            parser.error('--build is required for record')
        write_record(args.build, args.patch, args.source_hash)
    elif args.mode == 'stage':
        if args.build is None or args.directory is None:
            parser.error('--build and --directory are required for stage')
        stage(args.build, args.directory)
    elif args.mode == 'refresh-staged':
        if args.directory is None:
            parser.error('--directory is required for refresh-staged')
        refresh_staged(args.directory)
    else:
        if args.build is None or args.directory is None:
            parser.error('--build and --directory are required for update-rawfile')
        update_rawfile(args.build, args.directory)
