#!/usr/bin/env python3
"""Run the actual Wine readback function against checked GL/Wayland endpoints."""
from pathlib import Path
import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
WINE_SOURCE = ROOT / 'thirdparty/wine/dlls/winewayland.drv/opengl_readback.c'
PATCH = ROOT / 'scripts/patches/wine-direct-shm-readback.patch'


def patched_source():
    source = WINE_SOURCE.read_text(encoding='utf-8')
    with tempfile.TemporaryDirectory(prefix='winehua-readback-source-') as temporary:
        root = Path(temporary)
        path = root / 'dlls/winewayland.drv/opengl_readback.c'
        path.parent.mkdir(parents=True)
        path.write_text(source, encoding='utf-8')
        subprocess.run(['git', 'init', '-q', str(root)], check=True)
        applied = subprocess.run(['git', '-C', str(root), 'apply', '--reverse', '--check', str(PATCH)],
                                 stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0
        if not applied:
            subprocess.run(['git', '-C', str(root), 'apply', str(PATCH)], check=True)
        return path.read_text(encoding='utf-8')


def actual_pipeline():
    source = patched_source()
    # Keep all state/slot lifetime functions and the entire real present path.
    start = source.index('struct winehua_readback_drawable\n')
    end = source.index('\nstatic BOOL winehua_readback_drawable_swap(', start)
    code = source[start:end]
    # Only these drawable hooks need the unrelated Wine debug infrastructure.
    start = code.index('static void winehua_readback_drawable_destroy(')
    end = code.index('static void winehua_readback_buffer_release(', start)
    return code[:start] + code[end:]


class DirectShmReadbackTests(unittest.TestCase):
    def test_actual_present_pipeline(self):
        compiler = os.environ.get('CC') or shutil.which('cc') or shutil.which('clang') or shutil.which('cl')
        self.assertIsNotNone(compiler, 'Run inside a host compiler environment, or set CC.')
        harness = (ROOT / 'host_tests/direct_shm_readback_harness.c').read_text(encoding='utf-8')
        self.assertEqual(harness.count('/* SOURCE */'), 1)
        with tempfile.TemporaryDirectory(prefix='winehua-direct-shm-') as directory:
            directory = Path(directory)
            src = directory / 'readback.c'
            exe = directory / ('readback.exe' if sys.platform == 'win32' else 'readback')
            src.write_text(harness.replace('/* SOURCE */', actual_pipeline()), encoding='utf-8')
            if Path(compiler).stem.lower() == 'cl':
                command = [compiler, '/nologo', '/std:c11', '/O1', '/W3', '/WX', '/utf-8',
                           str(src), '/Fe:' + str(exe)]
            else:
                command = [compiler, '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                           '-Wno-unused-function', '-Wno-unused-parameter', str(src), '-o', str(exe)]
                if sys.platform == 'win32' and Path(compiler).stem.lower() == 'clang':
                    target = subprocess.run([compiler, '-dumpmachine'], check=True,
                                            capture_output=True, text=True).stdout.strip()
                    if 'mingw' in target or 'windows-gnu' in target:
                        command += ['-static']
                    else:
                        command += ['--target=x86_64-pc-windows-msvc']
                if sys.platform != 'win32':
                    command += ['-fsanitize=address,undefined']
            subprocess.run(command, cwd=directory, check=True)
            subprocess.run([str(exe)], cwd=directory, check=True, timeout=60)

    def test_build_contains_reproducible_patch(self):
        patch = PATCH.read_text(encoding='utf-8')
        self.assertIn('wine-direct-shm-readback.patch',
                      (ROOT / 'scripts/build_wine.sh').read_text(encoding='utf-8'))
        # Reconstruct the preimage from the applied source, checking each hunk.
        lines = patched_source().splitlines(keepends=True)
        cursor = 0
        hunks = re.split(r'(?m)^@@ .* @@[^\n]*\n', patch)[1:]
        self.assertTrue(hunks, 'Expected a nonempty patch.')
        for hunk in hunks:
            body = hunk.splitlines(keepends=True)
            new = [line[1:] for line in body if line[:1] in (' ', '+')]
            old = [line[1:] for line in body if line[:1] in (' ', '-')]
            matches = [i for i in range(cursor, len(lines) - len(new) + 1)
                       if lines[i:i + len(new)] == new]
            self.assertEqual(len(matches), 1, 'Patch must match the applied source exactly.')
            offset = matches[0]
            lines[offset:offset + len(new)] = old
            cursor = offset + len(old)
        preimage = ''.join(lines)
        self.assertNotIn('WINEHUA_GL_DIRECT_SHM_READBACK', preimage)
        self.assertNotIn('!direct_shm', preimage)


if __name__ == '__main__':
    unittest.main()
