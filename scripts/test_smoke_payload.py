"""Reject broken smoke payloads before any device mutation can begin."""
import argparse
from contextlib import redirect_stderr
import hashlib
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from automation import smoke


class PayloadValidationTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        (self.root / 'x64').mkdir()
        (self.root / 'x64/probe.exe').write_bytes(b'probe-fixture')
        self.manifest = {'suiteVersion': 'fixture', 'files': {
            'x64/probe.exe': hashlib.sha256(b'probe-fixture').hexdigest()}}
        self.write_manifest()
        (self.root / 'suites.json').write_text(json.dumps({'suiteVersion': 'fixture'}))
        self.args = argparse.Namespace(payload=str(self.root))

    def write_manifest(self):
        (self.root / 'manifest.json').write_text(json.dumps(self.manifest))

    def rejected(self):
        with redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            smoke.ensure_payload(self.args)

    def test_valid_payload_and_legacy_windows_separators(self):
        self.assertEqual(smoke.ensure_payload(self.args), self.root)
        digest = self.manifest['files'].pop('x64/probe.exe')
        self.manifest['files']['x64' + chr(92) + 'probe.exe'] = digest
        self.write_manifest()
        self.assertEqual(smoke.ensure_payload(self.args), self.root)

    def test_literal_newline_escape_is_not_json_whitespace(self):
        with (self.root / 'suites.json').open('a') as stream:
            stream.write(chr(92) + 'n')
        self.rejected()

    def test_mismatched_versions(self):
        self.manifest['suiteVersion'] = 'different'
        self.write_manifest()
        self.rejected()

    def test_modified_or_missing_executable(self):
        (self.root / 'x64/probe.exe').write_bytes(b'changed')
        self.rejected()
        (self.root / 'x64/probe.exe').unlink()
        self.rejected()

    def test_paths_cannot_escape_payload(self):
        for name in ('../probe.exe', '/probe.exe', 'C:/probe.exe', '..' + chr(92) + 'probe.exe'):
            self.manifest['files'] = {name: '0' * 64}
            self.write_manifest()
            self.rejected()

    def test_d3d9_profile_is_a_distinct_supported_contract(self):
        smoke.reject_bad_backend('d3d9 fixture', {'d3d': 'dxvk_legacy_d3d9'})
        suite = smoke.load_suites(smoke.load_cases())['dxvk-d3d9']
        self.assertTrue(suite['tests'])
        for entry in suite['tests']:
            self.assertEqual(entry.to_suite_json()['d3dBackend'], 'dxvk_legacy_d3d9')
        with redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            smoke.reject_bad_backend('ambiguous fixture', {'d3d': 'dxvk'})

    def test_explicit_host_route_is_used_for_expected_presentation(self):
        parser = smoke.build_parser()
        default = smoke.build_job(parser.parse_args(['run', '--suite', 'core']))
        wayland = smoke.build_job(parser.parse_args([
            'run', '--suite', 'core', '--display-route', 'wayland']))
        self.assertEqual(default['params']['env']['WINEHUA_SMOKE_EXPECT_ROUTE'], 'x11')
        self.assertEqual(wayland['params']['env']['WINEHUA_SMOKE_EXPECT_ROUTE'], 'wayland')

    def test_explicit_host_route_does_not_erase_job_declaration(self):
        job = self.root/'declared.json'
        job.write_text(json.dumps({'suite': 'core', 'params': {
            'env': {'WINEHUA_DISPLAY_ROUTE': 'x11'}}}))
        args = smoke.build_parser().parse_args([
            'run', '--job', str(job), '--display-route', 'wayland'])
        env = smoke.build_job(args)['params']['env']
        self.assertEqual(env['WINEHUA_SMOKE_EXPECT_ROUTE'], 'wayland')
        self.assertEqual(env['WINEHUA_SMOKE_DECLARED_ROUTE'], 'x11')

    def test_run_reads_utf8_metadata_with_a_gbk_host_default(self):
        read_text = Path.read_text

        def host_read(path, encoding=None, errors=None):
            return read_text(path, encoding=encoding or 'gbk', errors=errors)

        args = smoke.build_parser().parse_args(['run', '--suite', 'core'])
        with patch.object(Path, 'read_text', host_read), \
                patch.object(smoke, 'ensure_payload', return_value=self.root), \
                patch.object(smoke, 'resolve_hdc', return_value='unused-hdc'), \
                patch.object(smoke, 'resolve_device', return_value='unused-device'), \
                patch.object(smoke, 'cmd_push', side_effect=RuntimeError('ready to push')):
            # Exercises case/suite parsing and cmd_run's desktopMode lookup.
            # Stop before the first device operation.
            with self.assertRaisesRegex(RuntimeError, 'ready to push'):
                smoke.cmd_run(args)

    def test_utf8_payload_and_job_roundtrip_with_a_gbk_host_default(self):
        read_text, write_text = Path.read_text, Path.write_text

        def host_read(path, encoding=None, errors=None):
            return read_text(path, encoding=encoding or 'gbk', errors=errors)

        def host_write(path, data, encoding=None, errors=None, newline=None):
            return write_text(path, data, encoding=encoding or 'gbk', errors=errors, newline=newline)

        label = '\u4e2d\u6587\U0001f9ea'
        case = smoke.Case('fixture', 'probe.exe', ['x64'])
        entry = smoke.TestEntry('probe-x64', case, 'x64', {'env': {'FIXTURE_LABEL': label}})
        job = self.root/'unicode-job.json'
        job.write_bytes(json.dumps({'params': {'env': {'FIXTURE_LABEL': label}}},
                                  ensure_ascii=False).encode('utf-8'))
        args = smoke.build_parser().parse_args(['run', '--job', str(job)])
        with patch.object(Path, 'read_text', host_read), patch.object(Path, 'write_text', host_write):
            smoke.write_payload(self.root, {'fixture': {'tests': [entry]}}, {'fixture': case}, {})
            self.assertEqual(smoke.build_job(args)['params']['env']['FIXTURE_LABEL'], label)
        payload = json.loads((self.root/'suites.json').read_bytes().decode('utf-8'))
        self.assertEqual(payload['suites']['fixture']['tests'][0]['env']['FIXTURE_LABEL'], label)


if __name__ == '__main__':
    unittest.main()
