"""Preserve rawfile inputs and logs when Hvigor deletes the candidate output tree."""
from contextlib import redirect_stdout
import hashlib
import io
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

import package_desktop_candidate as candidate


class CandidateCleanupTests(unittest.TestCase):
    def exercise(self, fail):
        with tempfile.TemporaryDirectory(prefix='winehua-candidate-test-') as temporary:
            root = Path(temporary).resolve()
            raw = root/'entry/src/main/resources/rawfile'
            raw.mkdir(parents=True)
            (raw/'wine-data.zip').write_bytes(b'original-runtime')
            (raw/'wine-runtime-manifest.json').write_bytes(b'original-manifest')
            selected = root/'selected.zip'
            selected.write_bytes(b'selected-runtime')
            manifest = root/'selected.json'
            manifest.write_text(json.dumps({'payloadSha256': hashlib.sha256(selected.read_bytes()).hexdigest()}))
            out = root/'build/candidate'
            def build(command, cwd, stdout, **kwargs):
                self.assertEqual(Path(cwd), root)
                self.assertEqual((raw/'wine-data.zip').read_bytes(), selected.read_bytes())
                stdout.write(b'build output survives cleanup\n')
                target = (root/'build').resolve()
                target.relative_to(root)
                shutil.rmtree(target)
                if fail: raise subprocess.CalledProcessError(1, command)
                hap = root/'entry/build/default/outputs/default/entry-default-signed.hap'
                hap.parent.mkdir(parents=True)
                hap.write_bytes(b'candidate-hap')
            argv = ['package_desktop_candidate.py','--payload',str(selected),
                    '--manifest',str(manifest),'--out',str(out)]
            with patch.object(candidate,'ROOT',root), patch.object(sys,'argv',argv), \
                 patch.object(candidate.subprocess,'run',side_effect=build), \
                 patch.object(candidate,'verify_hap',return_value={'status':'verified'}), \
                 redirect_stdout(io.StringIO()):
                if fail:
                    with self.assertRaises(subprocess.CalledProcessError): candidate.main()
                else: candidate.main()
            self.assertEqual((raw/'wine-data.zip').read_bytes(), b'original-runtime')
            self.assertEqual((raw/'wine-runtime-manifest.json').read_bytes(), b'original-manifest')
            self.assertEqual((out/'build.log').read_bytes(), b'build output survives cleanup\n')
            if not fail:
                self.assertEqual((out/'desktop-candidate-signed.hap').read_bytes(), b'candidate-hap')
                self.assertTrue(json.loads((out/'candidate.json').read_text())['working_runtime_restored'])

    def test_success_after_build_directory_cleanup(self): self.exercise(False)
    def test_build_failure_still_restores_inputs_and_log(self): self.exercise(True)


if __name__ == '__main__': unittest.main()
