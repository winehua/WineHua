"""Exercise both build graphs and reject accidentally restored product gates."""
from pathlib import Path
import ast
import subprocess

ROOT=Path(__file__).resolve().parents[1]
for arch in ('arm64-v8a','x86_64'):
    wine_arch='aarch64' if arch=='arm64-v8a' else 'x86_64'
    result=subprocess.run(['make','--dry-run','NATIVE_ARCH='+arch,'WINE_ARCH='+wine_arch,'GUEST_ARCH='+wine_arch,'assemble'],cwd=ROOT,capture_output=True,text=True)
    assert result.returncode==0,result.stderr[-3000:]
    assert 'prepare_runtime_sources.py --component wine' in result.stdout
    if arch=='arm64-v8a':
        assert 'build_locked_fex.sh' in result.stdout and 'build_icu_compat.sh' in result.stdout
        assert 'local_source_receipt.py --output' in result.stdout
    for forbidden in ('prepare_windows_steam','package_selected_upstream','v2_product_contract'):
        assert forbidden not in result.stdout,forbidden
for path in (ROOT/'scripts').glob('*.sh'):
    subprocess.run(['bash','-n',str(path)],check=True)
for folder in ('scripts','automation'):
    for path in (ROOT/folder).glob('*.py'):
        ast.parse(path.read_text(),filename=str(path))
print('ARM64/x86_64 build graphs resolve; production FEX/ICU and source receipt are connected; script syntax PASS')
