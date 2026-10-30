#!/usr/bin/env python3
"""Linux integration checks for ELF relocation (requires cc and readelf)."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

SCRIPT = Path(__file__).with_name('relocate_elf.py')


@unittest.skipUnless(sys.platform.startswith('linux') and shutil.which('cc') and shutil.which('readelf'),
                     'requires Linux cc/readelf')
class RelocateTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='winehua-elf-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.build = self.root / 'build'
        self.stage = self.root / 'stage'
        self.build.mkdir()
        self.stage.mkdir()
        dep = self.build / 'librelocate_dep.so'
        (self.build / 'dep.c').write_text('int dependency(void) { return 42; }\n')
        (self.build / 'versions.map').write_text('VERSION_1 { global: dependency; local: *; };\n')
        (self.build / 'consumer.c').write_text('extern int dependency(void); int answer(void) { return dependency(); }\n')
        subprocess.run(['cc', '-shared', '-fPIC', str(self.build / 'dep.c'),
                        '-Wl,--version-script=' + str(self.build / 'versions.map'), '-o', str(dep)], check=True)
        subprocess.run(['cc', '-shared', '-fPIC', str(self.build / 'consumer.c'), str(dep),
                        '-Wl,-rpath,' + str(self.build), '-o', str(self.build / 'libconsumer.so')], check=True)
        for name in ('libconsumer.so', 'librelocate_dep.so'):
            shutil.copyfile(self.build / name, self.stage / name)
        self.consumer = self.stage / 'libconsumer.so'

    def relocate(self, check=True):
        return subprocess.run([sys.executable, str(SCRIPT), '--build-root', str(self.build), str(self.stage)],
                              check=check, capture_output=True, text=True)

    def readelf(self, *args):
        return subprocess.check_output(['readelf', *args, str(self.consumer)], text=True)

    def test_loadable_with_identical_layout_and_versioned_dependency(self):
        before = self.consumer.read_bytes()
        headers = self.readelf('-lW')
        strings = self.readelf('-x', '.dynstr')
        self.assertIn(str(self.build), self.readelf('-dW'))
        self.relocate()
        self.assertEqual(len(before), self.consumer.stat().st_size)
        self.assertEqual(headers, self.readelf('-lW'))
        self.assertEqual(strings, self.readelf('-x', '.dynstr'))
        self.assertNotIn(str(self.build), self.readelf('-dW'))
        self.assertNotIn(str(self.build), self.readelf('--version-info'))
        after = self.consumer.read_bytes()
        self.relocate()
        self.assertEqual(after, self.consumer.read_bytes())
        # Remove original libraries: successful loading must use relocated files.
        shutil.rmtree(self.build)
        env = dict(os.environ, LD_LIBRARY_PATH=str(self.stage))
        code = 'import ctypes,sys; assert ctypes.CDLL(sys.argv[1]).answer() == 42'
        subprocess.run([sys.executable, '-c', code, str(self.consumer)], env=env, check=True)

    def test_missing_dependency_leaves_files_unchanged(self):
        (self.stage / 'librelocate_dep.so').unlink()
        before = self.consumer.read_bytes()
        result = self.relocate(check=False)
        self.assertNotEqual(0, result.returncode)
        self.assertIn('bundled dependency missing', result.stderr)
        self.assertEqual(before, self.consumer.read_bytes())


if __name__ == '__main__':
    unittest.main()
