"""Sign a local HAP without exposing decrypted signing credentials."""
import os
from pathlib import Path
import subprocess
import sys

from scripts.json5_data import load


def main():
    if len(sys.argv) != 3:
        raise SystemExit('Usage: sign.py INPUT.hap OUTPUT.hap')
    config = load('build-profile.json5')['app']['signingConfigs'][0]['material']
    base_path = Path(config['certpath']).resolve().parent
    tool = Path(os.environ['TOOL_HOME'])
    node = tool / 'tool/node/bin/node'
    secrets = []
    for field in ('keyPassword', 'storePassword'):
        result = subprocess.run([str(node), 'sign.js', str(base_path), config[field]],
                                capture_output=True, text=True)
        value = result.stdout.strip()
        # sign.js reports decoder failures on stdout; never forward that output.
        if result.returncode or not value or '\n' in value or '\r' in value:
            raise SystemExit('Signing password decryption failed')
        secrets.append(value)
    command = ['java', '-jar', str(tool / 'sdk/default/openharmony/toolchains/lib/hap-sign-tool.jar'),
               'sign-app', '-keyAlias', config['keyAlias'], '-signAlg', config['signAlg'],
               '-mode', 'localSign', '-appCertFile', config['certpath'],
               '-profileFile', config['profile'], '-inFile', sys.argv[1],
               '-keystoreFile', config['storeFile'], '-outFile', sys.argv[2],
               '-keyPwd', secrets[0], '-keystorePwd', secrets[1]]
    result = subprocess.run(command, capture_output=True, text=True)
    output = result.stdout + result.stderr
    for value in secrets:
        output = output.replace(value, '[redacted]')
    if output:
        print(output, end='' if output.endswith('\n') else '\n')
    if result.returncode:
        raise SystemExit('HAP signing failed')
    if not Path(sys.argv[2]).is_file() or not Path(sys.argv[2]).stat().st_size:
        raise SystemExit('Signed HAP was not produced')


if __name__ == '__main__':
    main()
