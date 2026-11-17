"""Replay registered Mesa overlays from Git objects, never from a dirty tree."""
import hashlib
import json
from pathlib import Path
import re
import signal
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def function(source, marker):
    begin = source.index(marker)
    brace = source.index('{', begin)
    while ';' in source[begin:brace]:
        begin = source.index(marker, begin + len(marker))
        brace = source.index('{', begin)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[begin:end]


def replay(destination, mesa_git, stop_before=None):
    pin = json.loads((ROOT / 'docs/graphics/graphics-stack.lock.yaml').read_text())['submodules']['mesa']
    recipe = (ROOT / 'scripts/apply_mesa_ohos_patches.sh').read_text()
    patches = [ROOT / p for p in re.findall(
        r'\$SCRIPT_DIR/\.\./(patches/[^"\s]+\.patch)', recipe)]
    if stop_before:
        patches = patches[:next(i for i, p in enumerate(patches)
                                if p.name.startswith(stop_before))]
    paths = {'meson.build', 'src/gallium/drivers/zink/zink_resource.h',
             'src/gallium/drivers/zink/zink_resource.c',
             'src/gallium/drivers/zink/zink_screen.c',
             'src/gallium/drivers/zink/zink_bo.c',
             'src/gallium/drivers/zink/zink_context.c'}
    added = set()
    for patch in patches:
        text = patch.read_text()
        paths.update(re.findall(r'^\+\+\+ b/(.+)$', text, re.M))
        added.update(re.findall(r'^--- /dev/null\n\+\+\+ b/(.+)$', text, re.M))
    destination.mkdir(parents=True)
    subprocess.run(['git', 'init', '-q', str(destination)], check=True)
    for path in paths:
        result = subprocess.run(['git', '-C', str(mesa_git), 'show', pin + ':' + path],
                                capture_output=True)
        if result.returncode and path in added:
            continue
        result.check_returncode()
        target = destination / path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(result.stdout)
    for patch in patches if stop_before else []:
        if subprocess.run(['git', '-C', str(destination), 'apply', '--reverse',
                           '--check', str(patch)], capture_output=True).returncode == 0:
            continue
        result = subprocess.run(['git', '-C', str(destination), 'apply', str(patch)],
                                capture_output=True, text=True)
        if result.returncode:
            raise RuntimeError(patch.name + ': ' + result.stderr)
    if not stop_before:
        command = ['bash', str(ROOT / 'scripts/apply_mesa_ohos_patches.sh'), str(destination)]
        for _ in range(2):
            result = subprocess.run(command, capture_output=True, text=True)
            if result.returncode:
                raise RuntimeError('production replay: ' + result.stdout + result.stderr)
    source = destination / 'src/gallium/drivers/zink/zink_resource.c'
    print('Mesa pin', pin, 'overlays', len(patches), 'resource SHA-256',
          hashlib.sha256(source.read_bytes()).hexdigest(), flush=True)
    return source.read_text()


def compile_run(directory, code, name, sanitized=False, expect_failure=None):
    source = directory / (name + '.c')
    binary = directory / name
    source.write_text(code)
    flags = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer'] if sanitized else []
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-O1', '-g',
                    *flags, str(source), '-o', str(binary)], check=True)
    result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=30)
    if expect_failure:
        assert result.returncode == -signal.SIGABRT and expect_failure in result.stderr, (
            'negative control did not fail at its data/range assertion: ' + result.stdout + result.stderr)
        print(name + ': expected failure:', result.stderr.splitlines()[-1], flush=True)
    else:
        assert result.returncode == 0, result.stdout + result.stderr
        print(name + ': ' + result.stdout.strip(), flush=True)
