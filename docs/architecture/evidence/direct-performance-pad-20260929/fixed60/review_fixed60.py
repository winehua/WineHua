import json
import statistics
from pathlib import Path

root = Path(r'F:/WineHua/.temp/direct-performance-pad-20260929/fixed60-abba-20260929')
rows = json.loads((root/'outcomes.json').read_text(encoding='utf8'))
records = []
for index, row in enumerate(rows, 1):
    if 'validatedMetrics' not in row: continue
    m = row['validatedMetrics']
    total_cpu_per_second = m['cpuMs'] * 1000 / m['measuredMs'] + m['hapCpuMsPerSecond']
    records.append({'index':index,'runId':row['runId'],'backend':row['backend'],'gameFps':m['averageFps'],'hapAcceptedFps':m['hapAcceptedGamePresentFps'],'frameP95Ms':m['frameMsP95'],'frameP99Ms':m['frameMsP99'],'gameCpuMsPerGameFrame':m['cpuMsPerFrame'],'estimatedHapCpuMsPerGameFrame':m['estimatedHapCpuMsPerGameFrame'],'observedTwoProcessCpuMsPerGameFrame':m['cpuMsPerFrame']+m['estimatedHapCpuMsPerGameFrame'],'observedTwoProcessCpuMsPerAcceptedPresent':total_cpu_per_second/m['hapAcceptedGamePresentFps']})
summary={'completed':len(records),'expected':12,'metricsScope':'game process + HAP only; accepted presents are not scan-out FPS','samples':records,'medians':{},'fullSequenceCompleted':len(records)==12,'performanceGainVerified':False,'failures':[r for r in rows if 'validatedMetrics' not in r]}
keys=[k for k in records[0] if k not in ('index','runId','backend')]
for backend in ('venus','direct'):
    samples=[r for r in records if r['backend']==backend]
    if samples:
        summary['medians'][backend]={k:{'median':statistics.median(r[k] for r in samples),'min':min(r[k] for r in samples),'max':max(r[k] for r in samples)} for k in keys}
(root/'review.json').write_text(json.dumps(summary,indent=2)+'\n',encoding='utf8')
print(json.dumps({'completed':summary['completed'],'samples':records},indent=2))
