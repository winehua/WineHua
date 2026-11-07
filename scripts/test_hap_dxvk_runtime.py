"""Verify all advertised DXVK overlays before installing a HAP."""
import hashlib
import io
import json
import unittest
import zipfile

from validate_desktop_rendering import verify_dxvk_runtime


class DxvkPackageTests(unittest.TestCase):
    def package(self, missing=None, corrupt=False):
        stream = io.BytesIO()
        runtimes = {}
        with zipfile.ZipFile(stream, 'w') as archive:
            for profile, libraries in {
                'legacy': ('d3d9.dll', 'd3d10.dll', 'd3d10_1.dll', 'd3d10core.dll', 'd3d11.dll', 'dxgi.dll'),
                'modern-2.6': ('d3d11.dll', 'dxgi.dll')}.items():
                runtimes[profile] = {}
                for arch in ('x64', 'x86'):
                    runtimes[profile][arch] = {}
                    for library in libraries:
                        member = f'dxvk/{profile}/{arch}/{library}'
                        data = member.encode()
                        runtimes[profile][arch][library] = hashlib.sha256(data).hexdigest()
                        if member != missing: archive.writestr(member, data)
            if corrupt: runtimes['legacy']['x86']['d3d9.dll'] = '0' * 64
            archive.writestr('dxvk/manifest.json', json.dumps({'runtimes': runtimes}))
        stream.seek(0)
        return zipfile.ZipFile(stream)

    def test_complete_overlays(self):
        with self.package() as archive:
            self.assertEqual(len(verify_dxvk_runtime(archive)), 16)

    def test_missing_d3d9_and_d3d10_chain_are_rejected(self):
        for member in ('dxvk/legacy/x64/d3d9.dll', 'dxvk/legacy/x86/d3d10core.dll'):
            with self.package(missing=member) as archive:
                with self.assertRaisesRegex(AssertionError, 'Runtime DLL missing'):
                    verify_dxvk_runtime(archive)

    def test_mismatched_overlay_hash_is_rejected(self):
        with self.package(corrupt=True) as archive:
            with self.assertRaisesRegex(AssertionError, 'Runtime DLL hash mismatch'):
                verify_dxvk_runtime(archive)


if __name__ == '__main__':
    unittest.main()
