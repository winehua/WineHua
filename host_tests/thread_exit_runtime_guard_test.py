"""Reject stale or incomplete thread-exit candidates in the delivered HAP."""
import hashlib
import io
import json
from pathlib import Path
import sys
import unittest
from zipfile import ZipFile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
from runtime_library_guard import RuntimeLibraryError, THREAD_EXIT_MARKERS, validate_archive

MANIFEST = 'resources/rawfile/wine-runtime-manifest.json'
VERSION = 'resources/rawfile/wine_runtime.version'
PAYLOAD = 'resources/rawfile/wine-data.zip'
LIBRARY = 'libs/arm64-v8a/ntdll.so'


def archive(entries):
    buffer = io.BytesIO()
    with ZipFile(buffer, 'w') as out:
        for name, data in entries.items():
            out.writestr(name, data)
    return buffer.getvalue()


def sha(data):
    return hashlib.sha256(data).hexdigest()


def fixture(nested=False):
    # Minimal ELF used only for archive identity checks, not execution proof.
    header = bytearray(64)
    header[:6] = b'\x7fELF\x02\x01'
    header[18:20] = b'\xb7\x00'
    library = bytes(header) + b'\0'.join(THREAD_EXIT_MARKERS)
    members = ['native/ntdll.so'] if nested else []
    payload = archive({members[0]: library} if nested else {'readme.txt': b'runtime'})
    manifest = {'payloadSha256': sha(payload), 'threadExit': {
        'repairVersion': 2, 'library': LIBRARY, 'sha256': sha(library),
        'nestedMembers': members}}
    entries = {LIBRARY: library, MANIFEST: json.dumps(manifest).encode(),
               VERSION: ('content=paired;thread-exit=' + sha(library)[:16]).encode(), PAYLOAD: payload}
    return entries, manifest


class ThreadExitRuntimeGuardTest(unittest.TestCase):
    def reject(self, entries, expected):
        with self.assertRaisesRegex(RuntimeLibraryError, expected):
            validate_archive(archive(entries), 'candidate.hap')

    def test_matching_direct_and_nested_candidates_pass(self):
        for nested in (False, True):
            with self.subTest(nested=nested):
                entries, _ = fixture(nested)
                validate_archive(archive(entries), 'candidate.hap')

    def test_legacy_unmarked_runtime_is_supported(self):
        validate_archive(archive({MANIFEST: b'{"schemaVersion":1}'}), 'legacy.hap')

    def test_revision_one_record_remains_readable(self):
        entries, manifest = fixture()
        entries[LIBRARY] = entries[LIBRARY].split(THREAD_EXIT_MARKERS[3])[0]
        manifest['threadExit'].update(repairVersion=1, sha256=sha(entries[LIBRARY]))
        entries[VERSION] = ('thread-exit=' + sha(entries[LIBRARY])[:16]).encode()
        entries[MANIFEST] = json.dumps(manifest).encode()
        validate_archive(archive(entries), 'revision-one.hap')

    def test_stale_native_library_is_rejected(self):
        entries, _ = fixture()
        entries[LIBRARY] += b'old-build'
        self.reject(entries, 'checksum mismatch')

    def test_false_repair_fingerprint_is_rejected(self):
        entries, manifest = fixture()
        entries[LIBRARY] = entries[LIBRARY][:64]
        manifest['threadExit']['sha256'] = sha(entries[LIBRARY])
        entries[MANIFEST] = json.dumps(manifest).encode()
        self.reject(entries, 'missing thread-exit repair')

    def test_old_extraction_version_is_rejected(self):
        entries, _ = fixture()
        entries[VERSION] = b'content=old'
        self.reject(entries, 'runtime version mismatch')

    def test_stale_nested_library_is_rejected_even_with_updated_payload_hash(self):
        entries, manifest = fixture(True)
        entries[PAYLOAD] = archive({'native/ntdll.so': entries[LIBRARY] + b'stale'})
        manifest['payloadSha256'] = sha(entries[PAYLOAD])
        entries[MANIFEST] = json.dumps(manifest).encode()
        self.reject(entries, 'stale nested')

    def test_undeclared_nested_library_is_rejected(self):
        entries, manifest = fixture(True)
        manifest['threadExit']['nestedMembers'] = []
        entries[MANIFEST] = json.dumps(manifest).encode()
        self.reject(entries, 'nested member list mismatch')

    def test_missing_library_and_unsupported_repair_are_rejected(self):
        entries, _ = fixture()
        del entries[LIBRARY]
        self.reject(entries, 'incomplete thread-exit')
        entries, manifest = fixture()
        manifest['threadExit']['repairVersion'] = 3
        entries[MANIFEST] = json.dumps(manifest).encode()
        self.reject(entries, 'unsupported thread-exit')


if __name__ == '__main__':
    unittest.main()
