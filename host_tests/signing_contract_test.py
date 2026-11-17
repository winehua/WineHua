"""Signing must preserve literal paths, fail closed, and keep secrets out of logs."""
import contextlib
import importlib.util
import io
from pathlib import Path
import subprocess
import sys
import tempfile
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
spec = importlib.util.spec_from_file_location('winehua_sign', ROOT / 'sign.py')
signing = importlib.util.module_from_spec(spec)
spec.loader.exec_module(signing)

with tempfile.TemporaryDirectory(prefix='signing contract ') as folder:
    output = Path(folder) / 'signed with spaces.hap'
    material = dict(certpath=str(Path(folder) / 'app cert.cer'), keyAlias='literal alias',
                    keyPassword='encrypted-key', storePassword='encrypted-store',
                    profile=str(Path(folder) / 'debug profile.p7b'), signAlg='SHA256withECDSA',
                    storeFile=str(Path(folder) / 'app store.p12'))
    secret_values = ['key $(must_stay_literal)', 'store secret with spaces']
    for fail in (False, True):
        calls = []
        def run(command, **kwargs):
            assert isinstance(command, list) and not kwargs.get('shell')
            calls.append(command)
            if len(calls) <= 2:
                return subprocess.CompletedProcess(command, 0, secret_values[len(calls)-1] + '\n', '')
            assert command[command.index('-keyPwd')+1] == secret_values[0]
            assert command[command.index('-keystorePwd')+1] == secret_values[1]
            assert command[command.index('-appCertFile')+1] == material['certpath']
            if not fail:
                output.write_bytes(b'signed-test-artifact')
            return subprocess.CompletedProcess(command, 1 if fail else 0,
                'tool output: ' + ' / '.join(secret_values), '')
        log = io.StringIO()
        with patch.object(signing, 'load', return_value={'app': {'signingConfigs': [{'material': material}]}}), \
                patch.object(signing.subprocess, 'run', side_effect=run), \
                patch.dict(signing.os.environ, {'TOOL_HOME': folder}), \
                patch.object(sys, 'argv', ['sign.py', 'input with spaces.hap', str(output)]), \
                contextlib.redirect_stdout(log):
            try:
                signing.main()
            except SystemExit:
                assert fail, 'Successful signing unexpectedly failed'
            else:
                assert not fail, 'Signing tool failure was accepted'
        assert all(value not in log.getvalue() for value in secret_values)
    print('Signing PASS: literal paths/arguments, tool failure propagation and credential redaction')
