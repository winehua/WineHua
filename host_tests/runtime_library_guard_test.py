"""Synthetic ELF SDK stub vs real implementation; safe generated-output reuse."""
import io
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from zipfile import ZipFile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
from runtime_library_guard import RuntimeLibraryError, validate_archive, validate_library
from copy_runtime_library import copy_runtime


class RuntimeLibraryGuardTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(); self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.ext, self.sdk, self.dest = [self.root / name for name in ('ext', 'sdk', 'generated')]
        for path in (self.ext, self.sdk, self.dest): path.mkdir()
        names = ('inflate', 'inflateInit2_', 'inflateEnd', 'crc32', 'deflate')
        stub = 'int stub(void) { return 0; }\n' + '\n'.join(
            f'int {name}(void) __attribute__((alias("stub")));' for name in names)
        real = '\n'.join(f'int {name}(void) {{ return {index}; }}' for index, name in enumerate(names))
        for name, source in [('stub', stub), ('real', real)]:
            (self.root / (name + '.c')).write_text(source)
            subprocess.run(['cc', '-shared', '-fPIC', str(self.root / (name + '.c')),
                            '-o', str(self.root / (name + '.so'))], check=True)
        self.stub, self.real = [(self.root / (name + '.so')).read_bytes() for name in ('stub', 'real')]
        (self.sdk / 'libz.so').write_bytes(self.stub)

    def copy(self, system=True):
        copy_runtime(self.ext, self.sdk, self.dest, 'libz.so', 'libz.so', system=system)

    def test_real_ext_runtime_preserved(self):
        (self.ext / 'libz.so').write_bytes(self.real)
        self.copy(); self.assertEqual((self.dest / 'libz.so').read_bytes(), self.real)

    def test_ext_sdk_copy_rejected(self):
        (self.ext / 'libz.so').write_bytes(self.stub)
        with self.assertRaises(RuntimeLibraryError): self.copy()
        self.assertTrue((self.ext / 'libz.so').exists())

    def test_ext_sdk_symlink_rejected(self):
        (self.ext / 'libz.so').symlink_to(self.sdk / 'libz.so')
        with self.assertRaises(RuntimeLibraryError): self.copy()

    def test_native_stale_sdk_copy_removed_only_from_generated(self):
        (self.dest / 'libz.so').write_bytes(self.stub)
        self.copy(); self.assertFalse((self.dest / 'libz.so').exists())
        self.assertEqual((self.sdk / 'libz.so').read_bytes(), self.stub)

    def test_unknown_stale_file_is_preserved_and_blocks(self):
        (self.dest / 'libz.so').write_bytes(b'user modified')
        with self.assertRaises(RuntimeLibraryError): self.copy()
        self.assertEqual((self.dest / 'libz.so').read_bytes(), b'user modified')

    def test_guest_requires_real_runtime(self):
        with self.assertRaises(RuntimeLibraryError): self.copy(system=False)

    def test_direct_and_nested_hap_stub_rejected(self):
        def archive(entries):
            data = io.BytesIO()
            with ZipFile(data, 'w') as out:
                for name, contents in entries.items(): out.writestr(name, contents)
            return data.getvalue()
        for entries in ({'libs/arm64-v8a/libz.so': self.stub},
                        {'resources/rawfile/wine-data.zip': archive({'guest_gfx/x86_64/lib/libz.so': self.stub})}):
            with self.assertRaises(RuntimeLibraryError): validate_archive(archive(entries), 'test.hap')
        validate_archive(archive({'libs/arm64-v8a/libz.so': self.real}), 'real.hap')

    def test_gates_cover_assemble_and_unsigned_hap(self):
        assemble = (ROOT / 'scripts/assemble.sh').read_text()
        package = (ROOT / 'scripts/package.sh').read_text()
        self.assertEqual(assemble.count('copy_runtime_library.py'), 3)
        routing = (ROOT / 'entry/src/main/cpp/wine/wine_env.cpp').read_text()
        self.assertEqual(routing.count('libxml2.so:libxml2.so.2:libz.so:libz.so.1'), 2)
        self.assertIn('runtime_library_guard.py" "$NATIVE_LIBS" "$wine_data"', assemble)
        self.assertLess(package.index('runtime_library_guard.py" "$unsigned_hap"'),
                        package.index('if [ "$mode" = unsigned ]; then', package.index('hvigorw assembleHap')))

    def test_packaged_host_cannot_override_system_ncp_symbols(self):
        for name in ('OH_Ability_StartNativeChildProcess', 'OH_Ability_CreateNativeChildProcess'):
            source = self.root / 'entry.c'
            library = self.root / 'libentry.so'
            source.write_text(f'int {name}(void) {{ return 0; }}\n')
            subprocess.run(['cc', '-shared', '-fPIC', str(source), '-o', str(library)], check=True)
            with self.assertRaisesRegex(RuntimeLibraryError, 'bypass phone dispatch'):
                validate_library(library.read_bytes(), str(library))
        validate_library(self.real, str(library))

    def test_mesa_driver_alias_cannot_come_from_another_build(self):
        for prefix in ('libs/arm64-v8a/', 'bin/guest_gfx/lib/'):
            alias = prefix + ('dri/' if prefix.startswith('bin/') else '') + 'virtio_gpu_dri.so'
            def package(driver):
                data = io.BytesIO()
                with ZipFile(data, 'w') as out:
                    out.writestr(prefix + 'libgallium-25.0.1.so', self.real)
                    out.writestr(alias, driver)
                return data.getvalue()
            validate_archive(package(self.real), 'paired.hap')
            with self.assertRaisesRegex(RuntimeLibraryError, 'mismatched Mesa megadriver'):
                validate_archive(package(self.stub), 'mixed.hap')

    def test_paired_mesa_record_rejects_stale_egl_and_data_copy(self):
        def package(stale_egl=False, stale_data=False):
            names = {'egl': 'libEGL.so', 'gles1': 'libGLESv1_CM.so',
                     'gles2': 'libGLESv2.so', 'gallium': 'libgallium-25.0.1.so'}
            libraries = {'libs/arm64-v8a/' + name: dict(library=kind,
                         bytes=len(self.real), sha256=hashlib.sha256(self.real).hexdigest())
                         for kind, name in names.items()}
            nested = {'bin/guest_gfx/lib/' + name: dict(library=kind)
                      for kind, name in names.items()}
            payload = io.BytesIO()
            with ZipFile(payload, 'w') as out:
                for member in nested:
                    out.writestr(member, self.stub if stale_data and member.endswith('libEGL.so') else self.real)
            data = io.BytesIO()
            with ZipFile(data, 'w') as out:
                for member in libraries:
                    out.writestr(member, self.stub if stale_egl and member.endswith('libEGL.so') else self.real)
                out.writestr('resources/rawfile/wine-data.zip', payload.getvalue())
                out.writestr('resources/rawfile/wine-runtime-manifest.json', json.dumps(dict(mesaRuntime=dict(
                    schemaVersion=1, libraries=libraries, nestedMembers=nested))))
            return data.getvalue()
        validate_archive(package(), 'paired.hap')
        with self.assertRaisesRegex(RuntimeLibraryError, 'stale member of paired'):
            validate_archive(package(stale_egl=True), 'mixed-egl.hap')
        with self.assertRaisesRegex(RuntimeLibraryError, 'bundle/data Mesa copies differ'):
            validate_archive(package(stale_data=True), 'mixed-data.hap')


if __name__ == '__main__': unittest.main()
