"""Exercise partial result polling against a real local shell."""
import importlib.util
from pathlib import Path
import subprocess
import tempfile
import json
from types import SimpleNamespace
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('p0_smoke', ROOT / 'automation/smoke.py')
smoke = importlib.util.module_from_spec(spec)
import sys
sys.modules[spec.name] = smoke
spec.loader.exec_module(smoke)

class SandboxTextsTest(unittest.TestCase):
    def test_partial_results_survive_missing_summary(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'test.json').write_text('{"message":"fixed-frame"}')
            def local_shell(_hdc, _device, script, timeout):
                result = subprocess.run(['sh', '-c', script], capture_output=True, text=True)
                return result.returncode, result.stdout
            with patch.object(smoke, 'REAL_FILES', directory), patch.object(smoke, 'hdc_shell', local_shell):
                result = smoke.sandbox_texts('', '', ['test.json', 'suite-summary.json'])
                self.assertEqual(result['test.json'], '{"message":"fixed-frame"}')
                self.assertEqual(result['suite-summary.json'], '')
                (root / 'suite-summary.json').write_text('{"status":"PASS"}')
                result = smoke.sandbox_texts('', '', ['missing.json', 'suite-summary.json'])
                self.assertEqual(result['missing.json'], '')
                self.assertEqual(result['suite-summary.json'], '{"status":"PASS"}')

    def test_timeout_stays_distinct_from_missing_files(self):
        with patch.object(smoke, 'hdc_shell', return_value=(-1, '')):
            self.assertEqual(smoke.sandbox_texts('', '', ['test.json']), {})

    def test_recheck_preserves_original_subset(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'suite-summary.json').write_text(json.dumps({'suite': 'core', 'tests': []}))
            (root / 'job.json').write_text(json.dumps({'suite': 'core', 'tests': ['opengl-x64']}))
            suite = {'tests': [SimpleNamespace(test_id='opengl-x64'), SimpleNamespace(test_id='opengl-x86')]}
            verdict = {'status': 'PASS', 'passed': 1, 'total': 1, 'tests': []}
            with patch.object(smoke, 'load_cases', return_value={}), patch.object(smoke, 'load_suites', return_value={'core': suite}), patch.object(smoke, 'judge_run', return_value=verdict) as judge:
                self.assertEqual(smoke.cmd_check(SimpleNamespace(run_dir=directory)), 0)
                self.assertEqual(set(judge.call_args.args[1]), {'opengl-x64'})

if __name__ == '__main__':
    unittest.main()
