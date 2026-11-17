#!/usr/bin/env python3
"""Reject SDK stubs, stale Wine audio, and mismatched thread-exit repair packages."""
import argparse
import hashlib
import io
import json
from pathlib import Path
import struct
from zipfile import BadZipFile, ZipFile

from wine_audio_contract import shared_audio_format
from normalize_runtime_identity import load_identity


class RuntimeLibraryError(RuntimeError):
    pass


THREAD_EXIT_REPAIR_VERSION = 2
THREAD_EXIT_MARKERS = (
    b'failed to disable signal stack at thread exit:',
    b'invalid Unix stack at thread exit:',
    b'failed to join previous thread before freeing its TEB:',
    b'failed to query native pthread stack at startup:',
)


def validate_thread_exit_library(data, label, repair_version=THREAD_EXIT_REPAIR_VERSION):
    # Identity checks pair this diagnostic fingerprint with the compiled ELF.
    # The strings alone do not establish correct machine-code behavior.
    if len(data) < 64 or data[:6] != b'\x7fELF\x02\x01' or data[18:20] != b'\xb7\x00':
        raise RuntimeLibraryError(f'{label}: expected an ARM64 ELF ntdll')
    markers = THREAD_EXIT_MARKERS[:3] if repair_version == 1 else THREAD_EXIT_MARKERS
    if any(marker not in data for marker in markers):
        raise RuntimeLibraryError(f'{label}: missing thread-exit repair diagnostics')


def validate_thread_exit_metadata(archive, label):
    suffix = 'resources/rawfile/wine-runtime-manifest.json'
    manifests = [name for name in archive.namelist() if name.endswith(suffix)]
    if not manifests:
        return
    if len(manifests) != 1:
        raise RuntimeLibraryError(f'{label}: ambiguous runtime manifest')
    try:
        manifest = json.loads(archive.read(manifests[0]))
    except (ValueError, UnicodeError) as error:
        raise RuntimeLibraryError(f'{label}: invalid runtime manifest') from error
    if not isinstance(manifest, dict):
        raise RuntimeLibraryError(f'{label}: invalid runtime manifest')
    if 'threadExit' not in manifest:
        return  # Older, unmarked runtimes remain supported.
    record = manifest['threadExit']
    if not isinstance(record, dict) or record.get('repairVersion') not in (1, 2):
        raise RuntimeLibraryError(f'{label}: unsupported thread-exit repair record')
    if record.get('library') != 'libs/arm64-v8a/ntdll.so':
        raise RuntimeLibraryError(f'{label}: invalid thread-exit library member')
    prefix = manifests[0][:-len(suffix)]
    try:
        library = archive.read(prefix + record['library'])
        validate_thread_exit_library(library, label + '/' + record['library'], record['repairVersion'])
        digest = hashlib.sha256(library).hexdigest()
        if record.get('sha256') != digest:
            raise RuntimeLibraryError(f'{label}: thread-exit ntdll checksum mismatch')
        version = archive.read(prefix + 'resources/rawfile/wine_runtime.version').decode().strip()
        markers = [part for part in version.split(';') if part.startswith('thread-exit=')]
        if markers != ['thread-exit=' + digest[:16]]:
            raise RuntimeLibraryError(f'{label}: thread-exit runtime version mismatch')
        payload = archive.read(prefix + 'resources/rawfile/wine-data.zip')
        if manifest.get('payloadSha256') != hashlib.sha256(payload).hexdigest():
            raise RuntimeLibraryError(f'{label}: thread-exit runtime payload checksum mismatch')
        with ZipFile(io.BytesIO(payload)) as nested:
            members = []
            for name in nested.namelist():
                if Path(name).name != 'ntdll.so':
                    continue
                data = nested.read(name)
                if data[:6] != b'\x7fELF\x02\x01' or data[18:20] != b'\xb7\x00':
                    continue
                members.append(name)
                if hashlib.sha256(data).hexdigest() != digest:
                    raise RuntimeLibraryError(f'{label}/{name}: stale nested thread-exit ntdll')
            if record.get('nestedMembers') != members:
                raise RuntimeLibraryError(f'{label}: thread-exit nested member list mismatch')
    except (KeyError, ValueError, UnicodeError, BadZipFile) as error:
        raise RuntimeLibraryError(f'{label}: incomplete thread-exit runtime metadata') from error


def exported_functions(data):
    if data[:4] != b'\x7fELF':
        return {}
    if len(data) < 64 or data[4] not in (1, 2) or data[5] not in (1, 2):
        raise RuntimeLibraryError('invalid ELF header')
    endian = '<' if data[5] == 1 else '>'
    wide = data[4] == 2
    offset = struct.unpack_from(endian + ('Q' if wide else 'I'), data, 40 if wide else 32)[0]
    stride, count = struct.unpack_from(endian + 'HH', data, 58 if wide else 46)
    fmt = endian + ('IIQQQQIIQQ' if wide else 'IIIIIIIIII')
    sections = [struct.unpack_from(fmt, data, offset + i * stride) for i in range(count)]
    functions = {}
    for section in sections:
        if section[1] != 11:  # SHT_DYNSYM, also present in stripped SDK stubs
            continue
        strings = sections[section[6]]
        names = data[strings[4]:strings[4] + strings[5]]
        if section[9] < (24 if wide else 16):
            raise RuntimeLibraryError('invalid dynamic symbol stride')
        for pos in range(section[4], section[4] + section[5], section[9]):
            symbol = struct.unpack_from(endian + ('IBBHQQ' if wide else 'IIIBBH'), data, pos)
            name, info, other, index, value = ((symbol[0], symbol[1], symbol[2], symbol[3], symbol[4])
                if wide else (symbol[0], symbol[3], symbol[4], symbol[5], symbol[1]))
            if index and info & 15 == 2 and info >> 4 in (1, 2) and other & 3 in (0, 3):
                end = names.find(b'\0', name)
                functions[names[name:end].decode('ascii', 'replace')] = value
    return functions


def validate_library(data, label):
    try:
        if Path(label).name == 'wineohos.so':
            shared_audio_format(data)
        functions = exported_functions(data)
    except (IndexError, struct.error, ValueError) as error:
        raise RuntimeLibraryError(f'{label}: {error}') from error
    if Path(label).name == 'libentry.so':
        sdk_overrides = {'OH_Ability_StartNativeChildProcess', 'OH_Ability_CreateNativeChildProcess'}
        if sdk_overrides.intersection(functions):
            raise RuntimeLibraryError(f'{label}: SDK NCP symbol overrides can bypass phone dispatch; rebuild the host')
    # OHOS SDK link stubs alias their exported functions to one placeholder.
    if len(functions) >= 4 and len(set(functions.values())) == 1:
        raise RuntimeLibraryError(f'{label}: SDK-style link stub, not a runtime library')
    if Path(label).name in ('libz.so', 'libz.so.1'):
        required = ('inflate', 'inflateInit2_', 'inflateEnd', 'crc32', 'deflate')
        if any(name not in functions for name in required) or len({functions[n] for n in required}) < 4:
            raise RuntimeLibraryError(f'{label}: libz has no complete independent runtime implementation')


def validate_archive(data, label):
    with ZipFile(io.BytesIO(data)) as archive:
        if len(archive.namelist()) != len(set(archive.namelist())):
            raise RuntimeLibraryError(f'{label}: duplicate archive members')
        validate_mesa_aliases(archive, label)
        validate_mesa_metadata(archive, label)
        validate_thread_exit_metadata(archive, label)
        for name in archive.namelist():
            if '.so' in Path(name).name:
                validate_library(archive.read(name), f'{label}/{name}')
            elif name.endswith('wine-data.zip'):
                validate_archive(archive.read(name), f'{label}/{name}')


def validate_mesa_aliases(archive, label):
    """A Mesa megadriver and its DRI aliases must contain the same build.

    Phone forks load the bundle copy, while tablet children may load the data
    copy. An alias from an older build can carry a different pipe_screen ABI.
    Ignore only non-loadable release-strip metadata when comparing aliases.
    """
    names = set(archive.namelist())
    for name in sorted(names):
        path = Path(name)
        if not (path.name.startswith('libgallium-') and path.name.endswith('.so')):
            continue
        parent = path.parent
        expected = load_identity(archive.read(name))
        for alias in sorted(names):
            candidate = Path(alias)
            if not candidate.name.endswith('_dri.so'):
                continue
            if candidate.parent not in (parent, parent / 'dri'):
                continue
            if load_identity(archive.read(alias)) != expected:
                raise RuntimeLibraryError(f'{label}/{alias}: mismatched Mesa megadriver; rebuild and stage EGL/Gallium together')


def validate_mesa_metadata(archive, label):
    suffix = 'resources/rawfile/wine-runtime-manifest.json'
    for name in archive.namelist():
        if not name.endswith(suffix):
            continue
        record = json.loads(archive.read(name)).get('mesaRuntime')
        if record is None:
            continue
        if record.get('schemaVersion') != 1:
            raise RuntimeLibraryError(f'{label}: unsupported paired Mesa record')
        prefix = name[:-len(suffix)]
        groups = {}
        for member, item in record['libraries'].items():
            blob = archive.read(prefix + member)
            if len(blob) != item['bytes'] or hashlib.sha256(blob).hexdigest() != item['sha256']:
                raise RuntimeLibraryError(f'{label}/{member}: stale member of paired EGL/Gallium set')
            previous = groups.setdefault(item['library'], blob)
            if previous != blob:
                raise RuntimeLibraryError(f'{label}/{member}: inconsistent Mesa aliases')
        if set(groups) != {'egl', 'gles1', 'gles2', 'gallium'}:
            raise RuntimeLibraryError(f'{label}: incomplete paired EGL/Gallium set')
        with ZipFile(io.BytesIO(archive.read(prefix + 'resources/rawfile/wine-data.zip'))) as runtime:
            for member, item in record['nestedMembers'].items():
                blob = runtime.read(member)
                if blob != groups[item['library']]:
                    raise RuntimeLibraryError(f'{label}/{member}: bundle/data Mesa copies differ')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('paths', nargs='+', type=Path)
    args = parser.parse_args()
    try:
        for path in args.paths:
            if not path.exists():
                continue
            for item in (path.rglob('*') if path.is_dir() else [path]):
                if not item.is_file():
                    continue
                if '.so' in item.name:
                    validate_library(item.read_bytes(), str(item))
                elif item.suffix in ('.hap', '.zip'):
                    validate_archive(item.read_bytes(), str(item))
    except (RuntimeLibraryError, OSError) as error:
        parser.exit(1, f'runtime provenance: {error}\n')


if __name__ == '__main__':
    main()
