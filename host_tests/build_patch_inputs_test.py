"""Missing/untracked patches must fail before expensive build setup."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
from check_build_patch_inputs import check_inputs, tracked_files


class BuildPatchInputsTests(unittest.TestCase):
    def test_checkout_file_must_also_be_in_the_git_index(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            subprocess.run(['git', 'init', '-q', str(root)], check=True)
            (root / 'scripts').mkdir()
            patch = 'patches/nettle/0001-ohos-arm64-fat-dispatch.patch'
            (root / 'scripts/build.sh').write_text('PATCH="$SCRIPT_DIR/../' + patch + '"\n')
            subprocess.run(['git', '-C', str(root), 'add', 'scripts/build.sh'], check=True)
            self.assertIn('not tracked', check_inputs(root, tracked_files(root))[1][0])
            (root / patch).parent.mkdir(parents=True)
            (root / patch).write_text('fixture patch\n')
            # A local-only file used by an incremental build must still fail.
            self.assertIn('not tracked', check_inputs(root, tracked_files(root))[1][0])
            subprocess.run(['git', '-C', str(root), 'add', patch], check=True)
            self.assertEqual(check_inputs(root, tracked_files(root))[1], [])
            (root / patch).write_text('')
            self.assertIn('missing or empty', check_inputs(root, tracked_files(root))[1][0])

    def test_script_make_and_lock_paths(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            files = {'scripts/helper.py': "P = Path(__file__).resolve().parent / 'patches/f.patch'\n",
                     'scripts/build.sh': 'P="$SCRIPT_DIR/patches/a.patch"\nP="${ROOT}/patches/b.patch"\n',
                     'Makefile': 'INPUT := $(ROOT)/patches/c.patch\nINPUT := $(SCRIPTS)/patches/d.patch\n',
                     'docs/runtime.lock.json': '{"patches": [{"path": "patches/e.patch"}], "productionAdaptations": [{"path": "patches/old.patch"}]}\n'}
            for name, text in files.items():
                (root / name).parent.mkdir(parents=True, exist_ok=True)
                (root / name).write_text(text)
            refs, errors = check_inputs(root, set(files))
            self.assertEqual(set(refs), {'scripts/patches/a.patch', 'patches/b.patch',
                                        'patches/c.patch', 'scripts/patches/d.patch', 'patches/e.patch', 'scripts/patches/f.patch'})
            self.assertEqual(len(errors), 6)

    def test_missing_nettle_patch_fails_before_env_setup_in_both_modes(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root / 'scripts').mkdir()
            script = root / 'scripts/build_gnutls.sh'
            script.write_bytes((ROOT / 'scripts/build_gnutls.sh').read_bytes())
            marker = root / 'env-was-run'
            (root / 'scripts/env.sh').write_text('touch "$TEST_MARKER"\nexit 99\n')
            for mode in ('auto', 'off'):
                result = subprocess.run(['bash', str(script), '--check-identity'],
                    env={**os.environ, 'VP_NETTLE_ASM': mode, 'TEST_MARKER': str(marker)},
                    capture_output=True, text=True)
                self.assertEqual(result.returncode, 1)
                self.assertIn('missing Nettle build input', result.stderr)
                self.assertFalse(marker.exists())


if __name__ == '__main__':
    unittest.main()
