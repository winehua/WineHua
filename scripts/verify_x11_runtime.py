#!/usr/bin/env python3
"""Check X11 ELF/PE/data closure in staged OHOS libraries or a final HAP.

GNU bundles are intentionally excluded: they use another loader and ABI.
"""
import argparse
import io
from pathlib import Path
from zipfile import ZipFile
from check_runtime_components import elf_needed, ValidationError
from gstreamer_inventory import check_elf
from runtime_library_guard import validate_library
from fex_simd_runtime import pe_kind

ROOTS = ('libxwayland_ohos.so', 'winex11.so', 'libXrender.so.1', 'libXfixes.so.3',
    'libXi.so.6', 'libXrandr.so.2', 'libXinerama.so.1', 'libXcursor.so.1', 'libXcomposite.so.1', 'libXxf86vm.so.1')
CMAKE_ROOTS = ('libentry.so', 'libxwayland_child.so', 'libxim_server_child.so', 'libdisplay_route_napi.so')
# SDK platform APIs. All project-built dependencies must resolve from HAP libs.
SYSTEM = frozenset(('libc.so', 'libm.so', 'libdl.so', 'libEGL.so', 'libGLESv2.so',
    'libGLESv3.so', 'libvulkan.so', 'libace_napi.z.so', 'libace_ndk.z.so',
    'libhilog_ndk.z.so', 'libchild_process.so', 'libipc_capi.so', 'libohaudio.so',
    'libnative_window.so', 'libnative_image.so', 'libnative_buffer.so',
    'libnative_vsync.so', 'libnative_window_manager.so', 'libohhidebug.so',
    'libdeviceinfo_ndk.z.so'))
DATA = ('xkb/rules/evdev', 'xkb/symbols/us', 'xkb/types/complete', 'xkm/server-0.xkm',
    'share/X11/locale/C/XLC_LOCALE', 'share/X11/locale/zh_CN.UTF-8/XLC_LOCALE')


def verify_libraries(read, names, arch, roots):
    pending = list(roots)
    visited = set()
    while pending:
        name = pending.pop()
        if name in visited:
            continue
        if name not in names:
            raise ValidationError('unresolved OHOS X11 dependency: ' + name)
        visited.add(name)
        value = read(name)
        check_elf(value, arch, name)
        validate_library(value, name)
        for dependency in elf_needed(name, value):
            if dependency in ('libc.so.6', 'libpthread.so.0', 'libdl.so.2') or dependency.startswith('ld-linux-'):
                raise ValidationError('GNU ABI in OHOS X11 closure: ' + name)
            if '/' in dependency:
                raise ValidationError('absolute X11 DT_NEEDED: ' + name + ': ' + dependency)
            if dependency not in SYSTEM:
                pending.append(dependency)
    return visited


def verify_payload(payload, arch):
    with ZipFile(payload) as archive:
        for name in DATA:
            if not archive.read(name):
                raise ValidationError('empty X11 data: ' + name)
        if archive.read('xkm/server-0.xkm')[:4] != b'\x0fmkx':
            raise ValidationError('invalid XKM cache')
        # ARM64X/ARM64 and Wow64 both need the PE driver entry, not just Unix .so.
        native = 'aarch64' if arch == 'aarch64' else 'x86_64'
        for target in (native, 'i386'):
            name = 'bin/' + target + '-windows/winex11.drv'
            allowed = ('x86',) if target == 'i386' else (('arm64', 'arm64ec', 'arm64x') if arch == 'aarch64' else ('x64',))
            if pe_kind(archive.read(name)) not in allowed:
                raise ValidationError('wrong PE X11 driver architecture: ' + name)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--hap', type=Path)
    parser.add_argument('--libs', type=Path)
    parser.add_argument('--payload', type=Path)
    parser.add_argument('--native-arch', choices=('arm64-v8a','x86_64'), default='arm64-v8a')
    parser.add_argument('--defer-cmake', action='store_true', help='assembly only: Hvigor has not built app modules yet')
    args = parser.parse_args()
    arch = 'aarch64' if args.native_arch == 'arm64-v8a' else 'x86_64'
    roots = ROOTS + (() if args.defer_cmake else CMAKE_ROOTS)
    if args.hap:
        if args.defer_cmake:
            parser.error('final HAP cannot defer native output checks')
        with ZipFile(args.hap) as hap:
            prefix = 'libs/' + args.native_arch + '/'
            names = {n[len(prefix):] for n in hap.namelist() if n.startswith(prefix)}
            visited = verify_libraries(lambda n: hap.read(prefix+n), names, arch, roots)
            verify_payload(io.BytesIO(hap.read('resources/rawfile/wine-data.zip')), arch)
    else:
        if not args.libs or not args.payload:
            parser.error('provide --hap or --libs and --payload')
        names = {p.name for p in args.libs.iterdir() if p.is_file()}
        visited = verify_libraries(lambda n: (args.libs/n).read_bytes(), names, arch, roots)
        verify_payload(args.payload, arch)
    print('OHOS X11 closure PASS:', len(visited), 'ELF libraries, PE drivers, XKB/XKM and locale data')


if __name__ == '__main__':
    main()
