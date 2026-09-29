"""Review this candidate's measured rows without mixing older HAP samples."""
import hashlib
import json
from pathlib import Path
import re
import statistics

root = Path(__file__).resolve().parent / 'perf-fixed60'
plan = json.loads((root / 'plan.json').read_text(encoding='utf8'))
rows = json.loads((root / 'outcomes.json').read_text(encoding='utf8'))
records = []
for index, row in enumerate(rows, 1):
    if not row.get('measurementValid'):
        continue
    m = row['validatedMetrics']
    total_cpu_per_second = m['cpuMs'] * 1000 / m['measuredMs'] + m['hapCpuMsPerSecond']
    after = row['environmentAfter']
    desktop_foreground = any(a.get('ability') == 'DesktopAbility' and a.get('state') == 'FOREGROUND'
        for a in after.get('appAbilities', {}).get('abilities', []))
    desktop_background = any('onBackground' in s for s in after.get('desktopAbilityLifecycle', {}).get('events', []))
    records.append({'index': index, 'runId': row['runId'], 'backend': row['backend'],
        'gameFps': m['averageFps'], 'hapAcceptedFps': m['hapAcceptedGamePresentFps'],
        'frameP95Ms': m['frameMsP95'], 'frameP99Ms': m['frameMsP99'],
        'gameCpuMsPerGameFrame': m['cpuMsPerFrame'],
        'estimatedHapCpuMsPerGameFrame': m['estimatedHapCpuMsPerGameFrame'],
        'observedTwoProcessCpuMsPerAcceptedPresent': total_cpu_per_second / m['hapAcceptedGamePresentFps'],
        'desktopForegroundAtEnd': desktop_foreground, 'desktopBackgroundObserved': desktop_background})
keys = ('gameFps', 'hapAcceptedFps', 'frameP95Ms', 'frameP99Ms', 'gameCpuMsPerGameFrame',
        'estimatedHapCpuMsPerGameFrame', 'observedTwoProcessCpuMsPerAcceptedPresent')
summary = {'hapSha256': plan['hapSha256'], 'completed': len(records), 'expected': len(plan['runs']),
    'metricsScope': 'Game + HAP only; accepted presents are not physical scan-out FPS. CPU excludes wineserver and system services.',
    'samples': records, 'medians': {}, 'fullSequenceCompleted': len(records) == len(plan['runs']),
    'performanceGainVerified': False, 'failures': [r for r in rows if not r.get('measurementValid')],
    'allDesktopForegroundAtEnd': all(r['desktopForegroundAtEnd'] for r in records),
    'anyDesktopBackgroundObserved': any(r['desktopBackgroundObserved'] for r in records)}
for backend in ('venus', 'direct'):
    samples = [r for r in records if r['backend'] == backend]
    if samples:
        summary['medians'][backend] = {k: {'median': statistics.median(r[k] for r in samples),
            'min': min(r[k] for r in samples), 'max': max(r[k] for r in samples)} for k in keys}
summary['completeBlocks'] = []
for begin in range(0, len(rows) - 3, 4):
    block = [r for r in records if begin < r['index'] <= begin + 4]
    if len(block) != 4:
        continue
    cpu = {b: statistics.mean(r['observedTwoProcessCpuMsPerAcceptedPresent'] for r in block if r['backend'] == b)
           for b in ('venus', 'direct')}
    summary['completeBlocks'].append({'block': begin // 4 + 1, 'meanCpu': cpu,
                                     'directCpuChangePercent': (cpu['direct'] / cpu['venus'] - 1) * 100})
thermal = {}
display_hashes = set()
for row in rows:
    for bound in ('environmentBefore', 'environmentAfter'):
        env = row.get(bound, {})
        for sensor, value in re.findall(r'Type: (\S+)\s+Temperature: (\d+)', env.get('thermal', {}).get('output', '')):
            thermal.setdefault(sensor, []).append(int(value))
        display_hashes.add(hashlib.sha256(env.get('display', {}).get('output', '').encode()).hexdigest())
summary['temperatureRawRanges'] = {s: {'min': min(v), 'max': max(v)} for s, v in thermal.items()}
summary['uniqueDisplayBoundaryDumps'] = len(display_hashes)
(root / 'review.json').write_text(json.dumps(summary, indent=2) + '\n', encoding='utf8')
table = ['| 轮次 | 后端 | accepted/s | P99 ms | 两进程 CPU ms/accepted |',
         '| --- | --- | ---: | ---: | ---: |']
for r in records:
    table.append(f"| {r['index']} | {r['backend']} | {r['hapAcceptedFps']:.3f} | {r['frameP99Ms']:.3f} | {r['observedTwoProcessCpuMsPerAcceptedPresent']:.3f} |")
(root / 'review-table.md').write_text('\n'.join(table) + '\n', encoding='utf8')
print(json.dumps({k: v for k, v in summary.items() if k not in ('samples', 'failures')}, indent=2))
