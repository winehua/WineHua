import hashlib
import json
from pathlib import Path
import shutil
import subprocess

root = Path('/data/src/winehua')
out = root / 'build/direct-surface-lock-20260929'
out.mkdir(parents=True, exist_ok=True)
build = root / 'build/wine-ohos-aarch64'
native = root / 'entry/libs/arm64-v8a/win32u.so'
binary = build / 'dlls/win32u/win32u.so'
def sha(path):
    return hashlib.file_digest(path.open('rb'), 'sha256').hexdigest()
old = {'nativeSha256': sha(native), 'debugSha256': sha(binary)}
for name, src in [('win32u-before-native.so', native), ('win32u-before-debug.so', binary)]:
    dest = out / name
    if not dest.exists():
        shutil.copy2(src, dest)
with (out / 'build.log').open('w') as log:
    code = subprocess.run(['make', '-j8', 'dlls/win32u/win32u.so'], cwd=build,
        stdout=log, stderr=subprocess.STDOUT).returncode
result = {'before': old, 'makeExit': code, 'target': 'dlls/win32u/win32u.so'}
if code == 0:
    shutil.copy2(binary, native)
    result['after'] = {'nativeSha256': sha(native), 'debugSha256': sha(binary)}
    assert result['after']['nativeSha256'] != old['nativeSha256']
    result['rawfileUnchanged'] = True
(out / 'build-result.json').write_text(json.dumps(result, indent=2) + '\n')
print(json.dumps(result), flush=True)
if code:
    print((out / 'build.log').read_text(errors='replace')[-6000:])
raise SystemExit(code)
