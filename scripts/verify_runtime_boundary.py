#!/usr/bin/env python3
"""Guard product identity and permit only the authorized backend settings UI."""
from pathlib import Path
import argparse,json,re,subprocess
from prepare_runtime_sources import load

ROOT=Path(__file__).resolve().parents[1]
BASE='40b747714c109a7cb13e38b3af93ec77e1d38594'
PROTECTED=('AppScope','entry/src/main/ets','entry/src/main/resources',
           'entry/src/main/module.json5','build-profile.json5.example',
           'oh-package.json5','entry/oh-package.json5')

# User-authorized UI wiring; branding, resources, permissions and other pages stay protected.
GRAPHICS_UI={
    'entry/src/main/ets/pages/Index.ets',
    'entry/src/main/ets/service/WineEnvService.ets',
    'entry/src/main/ets/service/GraphicsBackendPolicy.ets',
    'entry/src/main/ets/entryability/EntryAbility.ets',
}

def git(*args):
    return subprocess.check_output(['git','-c','gc.auto=0','-C',str(ROOT),*args]).decode()

def check(require_clean=False):
    changed=git('diff','--name-only',BASE,'--',*PROTECTED).splitlines()
    unexpected=[name for name in changed if name not in GRAPHICS_UI]
    if unexpected: raise ValueError('Runtime migration changed the upstream UI/product boundary: '+', '.join(unexpected))
    if require_clean:
        if git('diff', '--name-only', 'HEAD', '--ignore-submodules=all').strip():
            raise ValueError('Commit runtime source changes before building a source candidate')
        if git('ls-files', '--others', '--exclude-standard').strip():
            raise ValueError('Unregistered root source inputs must be committed or ignored before building')
    old=git('show',BASE+':entry/src/main/cpp/bridge/napi_init.cpp')
    native='\n'.join(p.read_text() for p in (ROOT/'entry/src/main/cpp').rglob('*.cpp'))
    exports=lambda text:set(re.findall(r'\{\s*"(\w+)"\s*,\s*(?:0|nullptr)\s*,',text))
    missing=exports(old)-exports(native)
    if missing: raise ValueError('WineHua master NAPI entry points were removed: '+', '.join(sorted(missing)))
    type_path='entry/src/main/cpp/types/libentry/Index.d.ts'
    old_types=git('show',BASE+':'+type_path)
    declarations=lambda text:set(re.findall(r'export const (\w+)',text))
    missing=declarations(old_types)-declarations((ROOT/type_path).read_text())
    if missing: raise ValueError('WineHua master ArkTS declarations were removed: '+', '.join(sorted(missing)))
    cmake=(ROOT/'entry/src/main/cpp/CMakeLists.txt').read_text()
    start=cmake.index('if(WINEHUA_BUILD_RUNTIME_PROBES)')
    depth=0;end=None
    for match in re.finditer(r'\b(if|endif)\s*\([^\n]*\)',cmake[start:]):
        depth += 1 if match[1]=='if' else -1
        if depth==0:
            end=start+match.end();break
    if end is None: raise ValueError('Unbalanced diagnostic CMake option')
    cmake=cmake[:start]+cmake[end:]
    from source_build_contract import verify_native_targets
    verify_native_targets('libs/arm64-v8a/lib'+name+'.so' for name in re.findall(r'add_library\((\w+)\s+SHARED',cmake))
    result={}
    for component in ('wine','fex'):
        lock,_=load(component)
        result[component]=dict(commit=lock['adaptedCommit'],patches=len(lock['patches']))
    result.update(upstream=BASE,productAndUiUnchanged=not changed,authorizedUiChanges=changed,upstreamNapiExportsPreserved=len(exports(old)))
    print(json.dumps(result))
    return result

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--require-clean',action='store_true')
    check(parser.parse_args().require_clean)
