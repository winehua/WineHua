"""Reject the incomplete HAP that linked locally but crashed on device startup."""
import io
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import zipfile

import validate_desktop_rendering as validation

LIBRARIES = ('box64.so', 'libc++_shared.so', 'libentry.so', 'libsmoke_napi.so', 'libwine_child.so',
             'libvirgl_child.so', 'libwayland-server.so.0', 'libxkbcommon.so.0',
             'libffi.so.8', 'libepoxy.so.0', 'libvirglrenderer.so.1',
             'libwinehua_vtest_server.so')


class NativePackageTests(unittest.TestCase):
    def verify(self, libraries, staged=None, compiled=None):
        data = io.BytesIO()
        with zipfile.ZipFile(data, 'w') as archive:
            for name in libraries:
                archive.writestr('libs/arm64-v8a/' + name, b'fixture-library')
        data.seek(0)
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            if compiled:
                cmake = root/'entry/src/main/cpp/CMakeLists.txt'
                cmake.parent.mkdir(parents=True)
                cmake.write_text('add_library(' + compiled + ' SHARED source.cpp)')
            if staged:
                path = root / 'entry/libs/arm64-v8a' / staged
                path.parent.mkdir(parents=True)
                path.write_bytes(b'new-dependency')
            with patch.object(validation, 'ROOT', root), zipfile.ZipFile(data) as archive:
                return validation.verify_native_libraries(archive)

    def test_complete_package(self):
        self.assertEqual(self.verify(LIBRARIES), sorted(LIBRARIES))

    def test_linked_but_incomplete_package(self):
        with self.assertRaisesRegex(AssertionError, 'native libraries missing'):
            self.verify(LIBRARIES[:6])

    def test_new_staged_dependency_must_be_packaged(self):
        with self.assertRaisesRegex(AssertionError, 'libfuture.so'):
            self.verify(LIBRARIES, staged='libfuture.so')

    def test_cmake_napi_module_is_required_without_a_staged_copy(self):
        with self.assertRaisesRegex(AssertionError, 'libdisplay_route_napi.so'):
            self.verify(LIBRARIES, compiled='display_route_napi')
        libraries = LIBRARIES + ('libdisplay_route_napi.so',)
        self.assertEqual(self.verify(libraries, compiled='display_route_napi'), sorted(libraries))


if __name__ == '__main__':
    unittest.main()
