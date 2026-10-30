#!/usr/bin/env python3
"""Run host regressions and optionally verify an already-built signed HAP.

Run from any directory with host C/C++ compilers, Node and TypeScript installed:
  python scripts/validate_desktop_rendering.py --typescript /path/to/typescript.js
Use --cxx/--cc/--node for explicit tools and --hap for package verification.
Writes logs and result.json under --out (default: build/desktop-rendering-validation).
Does not install, launch, stop, reset, or otherwise change an attached device.
Linux renderer/readback harnesses retain their existing ASan/UBSan flags.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time
import zipfile

ROOT = Path(__file__).resolve().parents[1]


def digest(stream):
    result = hashlib.sha256()
    for block in iter(lambda: stream.read(1024 * 1024), b''):
        result.update(block)
    return result.hexdigest()


def verify_hap(path):
    with zipfile.ZipFile(path) as package:
        assert package.testzip() is None, 'HAP CRC failure'
        def member(suffix):
            matches = [name for name in package.namelist() if name.endswith(suffix)]
            assert len(matches) == 1, (suffix, matches)
            return matches[0]
        native = package.read(member('/arm64-v8a/libentry.so'))
        for marker in (b'request initial desktop frame', b'repaint cached frame',
                       b'ignore stale surface', b'retire invalid surface',
                       b'compose buffered children without root pixels'):
            assert marker in native, f'Native fix marker missing: {marker!r}'
        manifest = json.loads(package.read(member('/rawfile/wine-runtime-manifest.json')))
        with package.open(member('/rawfile/wine-data.zip')) as payload:
            payload_sha = digest(payload)
        assert payload_sha == manifest['payloadSha256'], 'Payload/manifest hash mismatch'
        raw = ROOT / 'entry/src/main/resources/rawfile'
        assert manifest == json.loads((raw / 'wine-runtime-manifest.json').read_text(encoding='utf-8'))
        with (raw / 'wine-data.zip').open('rb') as payload:
            assert digest(payload) == payload_sha, 'HAP contains an older runtime payload'
    with path.open('rb') as stream:
        hap_sha = digest(stream)
    return {'path': str(path.resolve()), 'hap_sha256': hap_sha,
            'native_sha256': hashlib.sha256(native).hexdigest(),
            'payload_sha256': payload_sha, 'crc': 'passed',
            'native_fix_markers': 'passed', 'payload_manifest': 'matched',
            'device_installation': 'not performed', 'signature_verification':
            'HAP signing is checked by the build; this script checks contents only'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cxx', default=os.environ.get('CXX', 'c++'))
    parser.add_argument('--cc', default=os.environ.get('CC', 'cc'))
    parser.add_argument('--node', default=shutil.which('node') or 'node')
    parser.add_argument('--typescript', default='typescript')
    parser.add_argument('--hap', type=Path)
    parser.add_argument('--out', type=Path, default=ROOT / 'build/desktop-rendering-validation')
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, CXX=args.cxx, CC=args.cc, PYTHONUTF8='1')
    results = []
    def run(name, command):
        started = time.monotonic()
        print('RUN', name, flush=True)
        log = args.out / (name + '.log')
        with log.open('wb') as output:
            proc = subprocess.run(command, cwd=ROOT, env=env, stdout=output,
                                  stderr=subprocess.STDOUT, timeout=180)
        results.append({'name': name, 'exit_code': proc.returncode,
                        'seconds': round(time.monotonic() - started, 3),
                        'log': str(log.resolve())})
        if proc.returncode:
            print(log.read_text(encoding='utf-8', errors='replace'), flush=True)
            raise RuntimeError(f'{name} failed ({proc.returncode})')
        print('PASS', name, flush=True)

    report = {'host_platform': sys.platform, 'steps': results,
              'device_regression': 'not performed', 'performance_ab': 'not performed'}
    try:
        run('renderer', [sys.executable, '-m', 'unittest', 'discover', '-s', 'scripts',
                         '-p', 'test_renderer_*.py'])
        run('initial-configure', [sys.executable, 'scripts/test_desktop_initial_configure.py'])
        run('bufferless-root', [sys.executable, 'scripts/test_desktop_bufferless_root.py'])
        run('output-policy', [args.node, 'scripts/test_desktop_output_policy.cjs', args.typescript])
        run('mouse-modes', [args.node, 'scripts/test_mouse_modes.cjs', args.typescript])
        run('venus-sync-notify', [sys.executable, 'scripts/test_venus_sync_notify.py'])
        run('direct-shm-readback', [sys.executable, 'scripts/test_direct_shm_readback.py'])
        run('font-policy', [sys.executable, 'scripts/test_font_policy.py'])
        for name in ('game_library', 'desktop_pointer', 'touch_controls', 'virtual_key_pulse', 'game_touch'):
            run(name, [args.node, 'scripts/test_' + name + '.cjs', args.typescript])
        with tempfile.TemporaryDirectory(prefix='winehua-desktop-profile-') as temp:
            for name in ('mewgenics_graphics', 'host_integration_policy', 'live_performance',
                         'legacy_text_compat', 'surface_queue_recovery', 'keyboard_target_policy'):
                exe = Path(temp) / (name + ('.exe' if sys.platform == 'win32' else ''))
                flags = ['-std=c++17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                         '-I', str(ROOT / 'entry/src/main/cpp'), '-I', str(ROOT / 'entry/src/main/cpp/wine')]
                flags += ['-static'] if sys.platform == 'win32' else ['-fsanitize=address,undefined', '-pthread']
                run(name + '-build', [args.cxx, *flags, str(ROOT / 'host_tests' / (name + '_test.cpp')),
                                     '-o', str(exe)])
                run(name, [str(exe)])
        if args.hap:
            report['package'] = verify_hap(args.hap)
        report['status'] = 'passed'
    except Exception as exc:
        report['status'] = 'failed'
        report['error'] = str(exc)
        raise
    finally:
        (args.out / 'result.json').write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
    print('PASS: host regression and requested package checks;', args.out / 'result.json')


if __name__ == '__main__':
    main()
