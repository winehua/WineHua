import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess

root = Path('/data/src/winehua')
out = root / 'build/direct-loader-screenawake-20260929'
out.mkdir(parents=True, exist_ok=True)
hap = root / 'entry/build/default/outputs/default/entry-default-signed.hap'
def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()
old_sha = sha(hap)
backup = out / f'entry-performance-{old_sha[:12]}-signed.hap'
if not backup.exists():
    shutil.copy2(hap, backup)
assert sha(backup) == old_sha
print('PRESERVED_SIGNED_HAP', old_sha, backup.stat().st_size, flush=True)
env = os.environ.copy()
env.update(TOOL_HOME='/apps/harmony', WINE_ARCH='aarch64', NATIVE_ARCH='arm64-v8a')
with (out / 'package.log').open('w') as log:
    code = subprocess.run(['bash', 'scripts/package.sh', 'hap'], cwd=root, env=env, stdout=log, stderr=subprocess.STDOUT).returncode
result = {'previousHapSha256': old_sha, 'backup': str(backup), 'packageExit': code}
if code == 0:
    result['candidateHapSha256'] = sha(hap)
    result['candidateHapBytes'] = hap.stat().st_size
    with (out / 'verify.log').open('w') as log:
        result['verifyExit'] = subprocess.run(['bash', 'scripts/w1-verify-candidate.sh', '--hap', str(hap)], cwd=root, env=env, stdout=log, stderr=subprocess.STDOUT).returncode
    candidate = out / f'entry-loader-screenawake-{result["candidateHapSha256"][:12]}-signed.hap'
    shutil.copy2(hap, candidate)
    result['candidate'] = str(candidate)
(out / 'identity.json').write_text(json.dumps(result, indent=2) + '\n')
print(json.dumps(result), flush=True)
if code:
    for line in (out / 'package.log').read_text(errors='replace').splitlines():
        if ('ERROR' in line or 'error:' in line) and not any(s in line.lower() for s in ('password', 'secret', 'token')):
            print(line[:500])
raise SystemExit(code or result.get('verifyExit', 1))
