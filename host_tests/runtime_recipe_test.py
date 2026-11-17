#!/usr/bin/env python3
"""Exercise complete replay, reuse, dirty/foreign rejection and patch integrity."""
from pathlib import Path
import hashlib,importlib.util,json,subprocess,tempfile

ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('recipe',ROOT/'scripts/prepare_runtime_sources.py')
recipe=importlib.util.module_from_spec(spec);spec.loader.exec_module(recipe)
def git(repo,*args):return recipe.git(repo,*args).decode().strip()
def fails(fn):
    try:fn()
    except ValueError:return
    raise AssertionError('Unsafe state was accepted')

task=ROOT/'workspace_temp/runtime-check';task.mkdir(parents=True,exist_ok=True)
with tempfile.TemporaryDirectory(prefix='recipe-',dir=task) as temp:
    root=Path(temp);raw=root/'thirdparty/wine-valve';raw.mkdir(parents=True)
    subprocess.run(['git','init','-q',str(raw)],check=True)
    git(raw,'config','user.name','Runtime recipe test')
    git(raw,'config','user.email','runtime-test@example.invalid')
    git(raw,'config','commit.gpgsign','false')
    (raw/'source.c').write_text('int value = 1;\n');git(raw,'add','source.c');git(raw,'commit','-qm','upstream')
    base=git(raw,'rev-parse','HEAD')
    (raw/'source.c').write_text('int value = 2;\n');git(raw,'add','source.c');git(raw,'commit','-qm','adaptation')
    commit=git(raw,'rev-parse','HEAD');tree=git(raw,'rev-parse','HEAD^{tree}')
    patch=recipe.git(raw,'diff',base,commit)
    patchpath=root/'scripts/patches/change.patch';patchpath.parent.mkdir(parents=True)
    patchpath.write_bytes(patch)
    history={commit:recipe.git(raw,'cat-file','commit',commit).decode()}
    folder=root/'docs/upstream-sync';folder.mkdir(parents=True)
    historypath=folder/'wine-adapted-history.json';historypath.write_text(json.dumps(history))
    lock=dict(upstream=dict(commit=base),adaptedCommit=commit,adaptedTree=tree,
              history=dict(sha256=hashlib.sha256(historypath.read_bytes()).hexdigest()),
              patches=[dict(path='scripts/patches/change.patch',sha256=hashlib.sha256(patch).hexdigest(),commit=commit)])
    (folder/'wine-candidate.lock.json').write_text(json.dumps(lock))
    git(raw,'checkout','--detach',base)
    loaded,objects=recipe.load('wine',root)
    output=root/'build/runtime-sources/wine'
    recipe.prepare('wine',output,loaded,objects,root)
    assert (output/'source.c').read_text()=='int value = 2;\n'
    recipe.verify(output,loaded)
    recipe.prepare('wine',output,loaded,objects,root) # No second patch application.
    assert git(raw,'rev-parse','HEAD')==base and not git(raw,'status','--porcelain')
    (output/'source.c').write_text('user edit\n')
    fails(lambda:recipe.prepare('wine',output,loaded,objects,root))
    assert (output/'source.c').read_text()=='user edit\n'
    foreign=root/'external';foreign.mkdir();(foreign/'keep').write_text('user data')
    fails(lambda:recipe.prepare('wine',foreign,loaded,objects,root))
    assert (foreign/'keep').read_text()=='user data'
    patchpath.write_bytes(patch+b'corrupt')
    fails(lambda:recipe.load('wine',root))
print('Runtime recipe PASS: exact replay, reuse, unchanged upstream, dirty and foreign source protection, tamper rejection')
