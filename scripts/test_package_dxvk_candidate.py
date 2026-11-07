"""Incremental packaging preserves every unrelated runtime member and input."""
import hashlib
import json
from pathlib import Path
import tempfile
import unittest
import zipfile

from package_dxvk_candidate import DLLS, package_candidate


class CandidatePackagingTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.base = self.root / 'wine-data.zip'
        self.legacy = self.root / 'rebuilt'
        self.files = {}
        manifest = {'runtimes': {'legacy': {}, 'modern-2.6': {}}}
        with zipfile.ZipFile(self.base, 'w') as archive:
            archive.writestr('bin/x86_64-windows/imm32.dll', b'unchanged-wine-dll')
            archive.writestr('share/wine/wine.inf', b'unchanged-font-policy')
            for profile, libraries in (('legacy', DLLS[1:]), ('modern-2.6', ('d3d11.dll', 'dxgi.dll'))):
                for arch in ('x64', 'x86'):
                    manifest['runtimes'][profile][arch] = {}
                    for library in libraries:
                        member = f'dxvk/{profile}/{arch}/{library}'
                        data = member.encode()
                        archive.writestr(member, data)
                        manifest['runtimes'][profile][arch][library] = hashlib.sha256(data).hexdigest()
            archive.writestr('dxvk/manifest.json', json.dumps(manifest))
        for arch in ('x64', 'x86'):
            for library in DLLS:
                relative = f'{arch}/bin/{library}'
                path = self.legacy / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                data = ('rebuilt:' + relative).encode()
                path.write_bytes(data)
                self.files[relative] = hashlib.sha256(data).hexdigest()
        (self.legacy/'build-provenance.json').write_text(json.dumps({'files': self.files, 'sourceCommit': 'fixture'}))

    def test_preserves_input_and_every_unrelated_member(self):
        before = self.base.read_bytes()
        report = package_candidate(self.base, self.legacy, self.root/'out')
        self.assertEqual(self.base.read_bytes(), before)
        self.assertEqual(len(report['changedMembers']), 13)
        with zipfile.ZipFile(self.base) as original, zipfile.ZipFile(self.root/'out/wine-data.zip') as updated:
            for member in original.namelist():
                if member not in report['changedMembers']:
                    self.assertEqual(original.read(member), updated.read(member), member)
            self.assertEqual(updated.read('dxvk/legacy/x64/d3d9.dll'), b'rebuilt:x64/bin/d3d9.dll')

    def test_corrupt_rebuilt_artifact_is_rejected_before_output(self):
        (self.legacy/'x86/bin/d3d9.dll').write_bytes(b'tampered')
        with self.assertRaisesRegex(AssertionError, 'Unverified rebuilt DLL'):
            package_candidate(self.base, self.legacy, self.root/'out')
        self.assertFalse((self.root/'out/wine-data.zip').exists())

    def test_source_zip_cannot_be_output(self):
        before = self.base.read_bytes()
        with self.assertRaisesRegex(AssertionError, 'overwrite'):
            package_candidate(self.base, self.legacy, self.root)
        self.assertEqual(self.base.read_bytes(), before)


if __name__ == '__main__':
    unittest.main()
