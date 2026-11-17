"""Candidate provenance must not turn source rebuilds into accepted releases."""
import copy
import io
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
from zipfile import ZipFile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
import source_build_contract as candidate
import runtime_core_contract as core


class CandidateTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        root = Path(cls.temp.name)
        source = root / 'test.c'
        source.write_text('int value(void) { return 42; }\n')
        binary = root / 'test.so'
        subprocess.run(['cc', '-shared', '-fPIC', '-g', str(source), '-o', str(binary)], check=True)
        # Synthetic fixture for format checks; never executed as ARM code.
        data = bytearray(binary.read_bytes())
        struct.pack_into('<H', data, 18, 183)
        cls.elf = bytes(data)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def test_real_checkout_stays_strict_and_reports_staged_and_unstaged_paths(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            def git(*args):
                return subprocess.check_output(['git', '-C', str(root), *args], text=True).strip()
            git('init', '-q')
            (root / 'tracked.c').write_text('original source\n')
            (root / '.gitignore').write_text('build/\n')
            (root / 'patches').mkdir()
            (root / 'docs/graphics').mkdir(parents=True)
            (root / 'thirdparty/mesa').mkdir(parents=True)
            # This small fixture exercises real Git state; graphics overlay
            # behavior is covered by the separate production contract tests.
            lock = root / 'docs/graphics/texture-compat-runtime.lock.json'
            lock.write_text(json.dumps({'runtimes': {}, 'venus': {'commit': 'pin', 'patches': []}}))
            git('add', '.')
            git('-c', 'user.name=Test', '-c', 'user.email=test@example.com', 'commit', '-qm', 'fixture')
            commit = git('rev-parse', 'HEAD')
            manifest = {'commit': commit, 'patches': {}, 'submodules': {'thirdparty/mesa': 'pin'}}
            # Keep a real git call for the provenance check while removing the
            # unrelated graphics checks after it succeeds.
            with patch.object(candidate, 'ROOT', root):
                for stage in (False, True):
                    (root / 'tracked.c').write_text('modified source\n')
                    if stage:
                        git('add', 'tracked.c')
                    with self.assertRaisesRegex(ValueError, 'changed during build: tracked.c'):
                        candidate.source_identity(manifest)
                    git('reset', '-q', 'HEAD', '--', 'tracked.c')
                    git('checkout', '--', 'tracked.c')
                (root / 'build').mkdir()
                (root / 'build/generated.h').write_text('generated output\n')
                # A clean checkout gets past the dirty-source check and reaches
                # the independent submodule-pin guard.
                with self.assertRaisesRegex(ValueError, 'gitlink changed'):
                    candidate.source_identity(manifest)
                with self.assertRaisesRegex(ValueError, 'another checkout'):
                    candidate.source_identity(dict(manifest, commit='wrong'))

    def receipt(self):
        return {'schemaVersion': 1, 'policy': candidate.POLICY, 'deviceValidated': False,
                'sources': {'checkout': {'commit': 'a' * 40}},
                'libraries': {'libs/arm64-v8a/test.so': {
                    'sha256': candidate.sha(self.elf), 'bytes': len(self.elf),
                    'loadableSha256': candidate.elf_identity(self.elf)}},
                'payload': {}, 'compilers': [{'sha256': 'b' * 64}]}

    def load(self, value, manifest=None):
        with patch.object(candidate, 'required_members', return_value=({'libs/arm64-v8a/test.so'}, set())):
            return candidate.load(lambda _: json.dumps(value).encode(), manifest or {'commit': 'a' * 40})

    def test_manifest_and_device_acceptance_are_mandatory(self):
        self.load(self.receipt())
        for key, value in (('deviceValidated', True), ('policy', 'accepted-release'), ('compilers', [])):
            record = self.receipt(); record[key] = value
            with self.assertRaises(ValueError): self.load(record)
        with self.assertRaises(ValueError): self.load(self.receipt(), {'commit': 'c' * 40})

    def test_missing_or_extra_inventory_is_rejected(self):
        record = self.receipt(); record['libraries'] = {}
        with self.assertRaises(ValueError): self.load(record)

    def test_elf_strip_preserves_identity_but_code_changes_do_not(self):
        record = self.receipt(); name = 'libs/arm64-v8a/test.so'
        candidate.validate_member(record, 'libraries', name, self.elf)
        # llvm-strip is unnecessary: alter only the non-loadable .comment bytes.
        changed = self.elf.replace(b'GCC:', b'XYZ:', 1)
        self.assertNotEqual(changed, self.elf)
        candidate.validate_member(record, 'libraries', name, changed)
        code = bytearray(self.elf)
        # e_flags belongs to the loadable identity, unlike compiler comments.
        code[48] ^= 1
        with self.assertRaises(ValueError): candidate.validate_member(record, 'libraries', name, bytes(code))

    def test_wrong_elf_architecture_is_rejected(self):
        data = bytearray(self.elf); struct.pack_into('<H', data, 18, 62)
        with self.assertRaises(ValueError): candidate.validate_binary('libs/arm64-v8a/test.so', bytes(data))

    def test_default_release_lock_remains_strict(self):
        record = self.receipt(); name = 'libs/arm64-v8a/test.so'
        with tempfile.TemporaryDirectory() as directory:
            lock = Path(directory) / 'lock.json'
            lock.write_text(json.dumps({'schemaVersion': 1, 'libraries': {name: '0' * 64}}))
            with patch.object(core, 'LOCK', lock):
                with self.assertRaisesRegex(ValueError, 'Unreviewed'):
                    core.validate_core(lambda _: self.elf)
                core.validate_core(lambda _: self.elf, candidate=record)

    def test_payload_checksum_and_pe_abi_both_required(self):
        data = bytearray(512); data[:2] = b'MZ'
        struct.pack_into('<I', data, 60, 128); data[128:132] = b'PE\0\0'
        struct.pack_into('<HH', data, 132, 0x14c, 0)
        name = 'bin/i386-windows/test.dll'
        record = {'payload': {name: {'sha256': candidate.sha(data), 'bytes': len(data)}}}
        candidate.validate_member(record, 'payload', name, bytes(data))
        data[-1] = 1
        with self.assertRaises(ValueError): candidate.validate_member(record, 'payload', name, bytes(data))
        with self.assertRaises(ValueError): candidate.validate_binary('dxvk/modern-2.6/arm64x/test.dll', bytes(data))

    def pending_receipt(self):
        value = self.receipt()
        value['pendingNative'] = [sorted(candidate.CMAKE_MODULES)[0]]
        return value

    def test_intentional_x64_modules_do_not_allow_x64_dxvk_or_wine_replacements(self):
        data = bytearray(512); data[:2] = b'MZ'
        struct.pack_into('<I', data, 60, 128); data[128:132] = b'PE\0\0'
        struct.pack_into('<HH', data, 132, 0x8664, 0)
        for name in ('bin/aarch64-windows/xtajit64.dll', 'vkd3d/limited-500k/x64/d3d12.dll'):
            candidate.validate_binary(name, bytes(data))
        for name in ('bin/aarch64-windows/ntdll.dll', 'dxvk/modern-2.6/x64/d3d11.dll',
                     'dxvk/legacy/arm64x/dxgi.dll'):
            with self.assertRaisesRegex(ValueError, 'architecture mismatch'):
                candidate.validate_binary(name, bytes(data))

    def test_only_cmake_modules_may_be_deferred_and_final_hap_requires_all(self):
        value = self.pending_receipt()
        inventory = set(value['libraries']) | set(value['pendingNative'])
        with patch.object(candidate, 'required_members', return_value=(inventory, set())):
            read = lambda _: json.dumps(value).encode()
            candidate.load(read, value['sources']['checkout'], require_native=False)
            with self.assertRaisesRegex(ValueError, 'omits required'):
                candidate.load(read, value['sources']['checkout'])
            changed = copy.deepcopy(value)
            changed['pendingNative'] = ['libs/arm64-v8a/unbuilt-dependency.so']
            with self.assertRaises(ValueError):
                candidate.load(lambda _: json.dumps(changed).encode(),
                               value['sources']['checkout'], require_native=False)
            candidate.complete_native(value, lambda _: self.elf)
            candidate.load(read, value['sources']['checkout'])

    def test_deferred_module_must_be_aarch64_and_cannot_expand_inventory(self):
        wrong = bytearray(self.elf); struct.pack_into('<H', wrong, 18, 62)
        with self.assertRaisesRegex(ValueError, 'not AArch64'):
            candidate.complete_native(self.pending_receipt(), lambda _: bytes(wrong))
        value = self.receipt(); value['pendingNative'] = ['libs/arm64-v8a/other.so']
        with self.assertRaisesRegex(ValueError, 'cannot defer'):
            candidate.complete_native(value, lambda _: self.elf)

    def test_cold_assembly_defers_the_complete_real_hvigor_inventory(self):
        # First CI assembly has none of the application CMake outputs. External
        # dependencies remain mandatory, and the final gate accepts no pending DLL.
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            compiler = root / 'clang'; compiler.write_bytes(b'compiler fixture')
            venus = root / 'bin/guest_vulkan/lib/libvulkan_virtio.so'
            venus.parent.mkdir(parents=True); venus.write_bytes(self.elf)
            dependency = root / 'libs/arm64-v8a/libexternal.so'
            dependency.parent.mkdir(parents=True); dependency.write_bytes(self.elf)
            inventory = candidate.CMAKE_MODULES | {'libs/arm64-v8a/libexternal.so'}
            with patch.object(candidate, 'source_identity', return_value={'checkout': {}}), \
                    patch.object(candidate, 'required_members', return_value=(inventory, set())):
                value = candidate.record(root, root / 'libs', {}, [compiler])
                self.assertEqual(set(value['pendingNative']), candidate.CMAKE_MODULES)
                self.assertEqual(set(value['libraries']), {'libs/arm64-v8a/libexternal.so'})
                read = lambda _: json.dumps(value).encode()
                candidate.load(read, {}, require_native=False)
                with self.assertRaisesRegex(ValueError, 'omits required'):
                    candidate.load(read, {})
                candidate.complete_native(value, lambda _: self.elf)
                candidate.load(read, {})
                self.assertEqual(set(value['libraries']), inventory)
                dependency.unlink()
                with self.assertRaises(FileNotFoundError):
                    candidate.record(root, root / 'libs', {}, [compiler])
        real_libraries, _ = candidate.required_members()
        self.assertTrue(candidate.CMAKE_MODULES <= real_libraries)

    def test_receipt_covers_wine_without_retired_bridge(self):
        _, payload = candidate.required_members()
        for architecture in ('i386-windows', 'aarch64-windows'):
            self.assertIn('bin/' + architecture + '/kernelbase.dll', payload)
        self.assertFalse(any('lsteamclient' in name for name in payload))

    def test_new_or_omitted_hvigor_targets_fail_before_compilation(self):
        candidate.verify_native_targets(candidate.CMAKE_MODULES)
        probe = 'libs/arm64-v8a/libdisplay_route_napi.so'
        self.assertIn(probe, candidate.CMAKE_MODULES)
        with self.assertRaisesRegex(ValueError, 'missing:.*libdisplay_route_napi'):
            candidate.verify_native_targets(candidate.CMAKE_MODULES - {probe})
        self.assertNotIn('libs/arm64-v8a/libwinehua_native_memory_probe.so', candidate.CMAKE_MODULES)
        self.assertNotIn('libs/arm64-v8a/libsteamdecompress.so', candidate.CMAKE_MODULES)
        with self.assertRaisesRegex(ValueError, 'unregistered:.*libnew'):
            candidate.verify_native_targets(candidate.CMAKE_MODULES | {'libs/arm64-v8a/libnew.so'})

    def test_record_defers_cmake_outputs_even_if_stale_outputs_exist(self):
        name = sorted(candidate.CMAKE_MODULES)[0]
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory); libs = root / 'libs'
            stale = root / name; stale.parent.mkdir(parents=True)
            stale.write_bytes(b'stale previous build')
            venus = root / 'bin/guest_vulkan/lib/libvulkan_virtio.so'
            venus.parent.mkdir(parents=True); venus.write_bytes(self.elf)
            with patch.object(candidate, 'source_identity', return_value={'checkout': {}}), \
                    patch.object(candidate, 'required_members', return_value=({name}, set())):
                result = candidate.record(root, libs, {}, [stale])
            self.assertEqual(result['pendingNative'], [name])
            self.assertEqual(result['libraries'], {})

    def test_finalize_pairs_hvigor_outputs_without_changing_other_payload_members(self):
        value = self.pending_receipt(); name = value['pendingNative'][0]
        inventory = set(value['libraries']) | {name}
        buffer = io.BytesIO()
        with ZipFile(buffer, 'w') as payload:
            payload.writestr(candidate.RECORD, json.dumps(value))
            payload.writestr('retained-game-runtime.bin', b'keep this exact content')
        before = buffer.getvalue()
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory); hap = root / 'first.hap'
            payload_path = root / 'wine-data.zip'; payload_path.write_bytes(before)
            outer = {'payloadSha256': candidate.sha(before), 'validationPolicy': candidate.POLICY,
                     'sourceBuild': {'commit': 'a' * 40}, 'deviceValidated': False}
            outer_path = root / 'wine-runtime-manifest.json'
            outer_path.write_text(json.dumps(outer))
            version_path = root / 'wine_runtime.version'
            version_path.write_text('content=' + candidate.sha(before)[:16] + ';wine-valve=unchanged\n')
            with ZipFile(hap, 'w') as output:
                output.writestr('resources/rawfile/wine-data.zip', before)
                output.writestr(name, self.elf)
            with patch.object(candidate, 'required_members', return_value=(inventory, set())):
                candidate.finalize(hap, root, value['sources']['checkout'])
                with ZipFile(payload_path) as payload:
                    result = candidate.load(payload.read, value['sources']['checkout'])
                    self.assertEqual(payload.read('retained-game-runtime.bin'), b'keep this exact content')
                    self.assertEqual(set(payload.namelist()),
                                     {candidate.RECORD, 'retained-game-runtime.bin'})
                self.assertEqual(result['pendingNative'], [])
                candidate.validate_member(result, 'libraries', name, self.elf)
                after = payload_path.read_bytes()
                final_outer = json.loads(outer_path.read_text())
                self.assertEqual(final_outer, dict(outer, payloadSha256=candidate.sha(after)))
                self.assertEqual(version_path.read_text(),
                                 'content=' + candidate.sha(after)[:16] + ';wine-valve=unchanged\n')
                # A HAP with a different first-stage runtime cannot supply identities.
                snapshot = (after, outer_path.read_bytes(), version_path.read_bytes())
                with self.assertRaisesRegex(ValueError, 'another runtime'):
                    candidate.finalize(hap, root, value['sources']['checkout'])
                self.assertEqual(snapshot,
                                 (payload_path.read_bytes(), outer_path.read_bytes(), version_path.read_bytes()))


if __name__ == '__main__':
    unittest.main()
