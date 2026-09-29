import json
import argparse
from datetime import datetime
from pathlib import Path
import re
import subprocess
import sys

sys.path.insert(0, r'F:/WineHua/proton-ohos-worktree')
from automation.smoke import resolve_hdc, hdc_shell, hdc_sandbox_shell

parser = argparse.ArgumentParser()
parser.add_argument('--candidate', default='715fa38b')
parser.add_argument('--desktop-stall-seconds', default=0, type=int)
parser.add_argument('--runs', default=6, type=int, choices=range(1, 21))
parser.add_argument('--backend', default='venus', choices=('venus', 'direct', 'both'))
options = parser.parse_args()
root = Path(r'F:/WineHua/proton-ohos-worktree')
stamp = datetime.now().strftime('%H%M%S')
archive = Path(r'F:/WineHua/.temp/direct-loader-screenawake-pad-20260929') / stamp
archive.mkdir(parents=True, exist_ok=True)
hdc = resolve_hdc()
device = '5KPBB25818203996'
(archive / 'automation-policy.json').write_text(json.dumps({'mode': 'diagnostic', 'target': device, 'artifactDir': str(archive),
    'redaction': 'Only explicit-PID startup and sampler records; bounded shared stderr tail. No signing logs or full environment.',
    'actions': 'Cold App sessions with unchanged prefix/payload, opt-in sampling; no global HDC restart.'}, indent=2) + '\n')
try:
    hdc_shell(hdc, device, 'power-shell timeout -o 3600000', timeout=10)
    for number in range(1, options.runs + 1):
        backend = ('venus' if number % 2 else 'direct') if options.backend == 'both' else options.backend
        run_id = f'loader-screenawake-{options.candidate}-amd64-{backend}-{number}-20260929-{stamp}'
        hdc_shell(hdc, device, 'aa force-stop app.hackeris.winehua', timeout=10)
        rc, text = hdc_shell(hdc, device, 'ps -A -o PID,PPID,UID,NAME', timeout=10)
        remaining = [l for l in text.splitlines() if len(l.split()) >= 4 and l.split()[2] == '20020271']
        if rc or remaining:
            raise RuntimeError(f'Residual test app processes: {remaining}')
        print('DIAGNOSTIC_RUN', number, run_id, flush=True)
        args = [sys.executable, '-X', 'utf8', 'automation/smoke.py', 'run', '--suite', 'direct-performance-amd64', '--prefix', 'reuse',
            '--device', device, '--run-id', run_id, '--archive-root', str(archive), '--payload', 'build/smoke-performance-payload',
            '--desktop-mode', 'virtual', '--desktop-renderer', 'vulkan' if backend == 'direct' else 'egl', '--direct-ncp-session', '--desktop-stall-seconds', str(options.desktop_stall_seconds), '--env', f'WINEHUA_VULKAN_BACKEND={backend}',
            '--env', 'WINEHUA_STALL_DUMP=10', '--env', 'WINEHUA_BENCHMARK_WARMUP_MS=3000', '--env', 'WINEHUA_BENCHMARK_DRAWS=100',
            '--env', 'WINEHUA_BENCHMARK_SMALL_SCISSOR=1', '--env', 'WINEHUA_BENCHMARK_FPS=60', '--seconds', '12', '--timeout-minutes', '3',
            '--timeout-ms', '150000', '--poll-seconds', '5', '--keep-app', '--skip-push']
        try:
            code = subprocess.run(args, cwd=root, timeout=220).returncode
        except subprocess.TimeoutExpired:
            code = 124
        directory = archive / f'direct-performance-amd64-{run_id}'
        summary = directory / 'suite-summary.json'
        pid = None
        if summary.is_file():
            tests = json.loads(summary.read_text(encoding='utf8')).get('tests', [])
            if tests:
                pid = tests[0].get('pid')
        rc, out = hdc_sandbox_shell(hdc, device, 'tail -c 1048576 data/storage/el2/base/temp/wine_stderr_20260929.log', timeout=15)
        lines = [l for l in out.splitlines() if pid and re.search(rf'\b(?:pid=|PID=){pid}\b', l) and 'entryParams=' not in l]
        diagnostic = {'runId': run_id, 'backend': backend, 'exit': code, 'gamePid': pid, 'diagnosticOnly': True,
            'stderrReadExit': rc, 'tailLimitBytes': 1048576, 'explicitPidLines': lines}
        _, processes = hdc_shell(hdc, device, 'ps -A -o PID,PPID,UID,NAME', timeout=10)
        app_pids = [int(l.split()[0]) for l in processes.splitlines() if len(l.split()) >= 4 and l.split()[3] == 'app.hackeris.winehua']
        child_pids = [int(l.split()[0]) for l in processes.splitlines() if len(l.split()) >= 4 and l.split()[2] == '20020271']
        diagnostic['appProcesses'] = [l for l in processes.splitlines() if len(l.split()) >= 4 and l.split()[2] == '20020271']
        diagnostic['currentProcessStallLines'] = [l for l in out.splitlines() if '[stall-' in l and
            any(re.search(rf'\bpid={child_pid}\b', l) for child_pid in child_pids)]
        _, launches = hdc_shell(hdc, device, 'hilog -z1000 -T WL_NAPI', timeout=10)
        desktop_launches = [l for l in launches.splitlines() if any(re.search(rf'\s{app_pid}\s', l) for app_pid in app_pids) and 'explorer desktop pid=' in l]
        if desktop_launches:
            desktop_pid = int(re.search(r'explorer desktop pid=(\d+)', desktop_launches[-1]).group(1))
            diagnostic['desktopPid'] = desktop_pid
            diagnostic['desktopLaunch'] = desktop_launches[-1]
            diagnostic['desktopExplicitPidLines'] = [l for l in out.splitlines() if re.search(rf'\b(?:pid=|PID=){desktop_pid}\b', l) and 'entryParams=' not in l]
        rc, out = hdc_shell(hdc, device, 'hilog -z300 -T WineHuaScreenAwake', timeout=10)
        diagnostic['screenAwake'] = {'exit': rc, 'lines': [l for l in out.splitlines() if 'owner=' in l]}
        (archive / f'run-{number}-diagnostic.json').write_text(json.dumps(diagnostic, ensure_ascii=False, indent=2) + '\n', encoding='utf8')
        print('DIAGNOSTIC_EXIT', number, code, 'PID', pid, flush=True)
        if code:
            main_lines = [line for line in lines if re.search(rf'\btid={pid}\b', line) or 'why=' in line]
            print('\n'.join(main_lines[-50:]), flush=True)
            desktop_main = [l for l in diagnostic.get('desktopExplicitPidLines', []) if re.search(rf'\btid={diagnostic.get("desktopPid")}\b', l)]
            print('DESKTOP_MAIN', diagnostic.get('desktopPid'), '\n' + '\n'.join(desktop_main[-45:]), flush=True)
            break
finally:
    rc, text = hdc_shell(hdc, device, 'power-shell timeout -r', timeout=10)
    (archive / 'screen-timeout-restored.log').write_text(text, encoding='utf8')
