from datetime import datetime
import json
from pathlib import Path
import re
import sys
import time

sys.path.insert(0, r'F:/WineHua/proton-ohos-worktree')
from automation.smoke import resolve_hdc, hdc_shell
hdc = resolve_hdc()
device = '5KPBB25818203996'
result = {'candidate': 'd37139a585b8', 'mode': 'automation',
          'actions': 'Temporarily shorten screen timeout, wait in foreground, Home, resume Desktop, restore timeout.',
          'redaction': 'Only power state, window locks and test-app Ability/lifecycle records.'}
_, ps = hdc_shell(hdc, device, 'ps -A -o PID,PPID,UID,NAME', timeout=10)
pid = next(int(l.split()[0]) for l in ps.splitlines() if len(l.split()) >= 4 and l.split()[3] == 'app.hackeris.winehua')
result['appPid'] = pid

def snapshot():
    stamp = datetime.now().astimezone().isoformat()
    rc, out = hdc_shell(hdc, device, 'hidumper -s PowerManagerService -a -s', timeout=10)
    state = [l for l in out.splitlines() if l.startswith(('Current State:', 'ScreenOffTime:'))]
    _, locks = hdc_shell(hdc, device, 'hidumper -s PowerManagerService -a -r', timeout=10)
    locks = [l for l in locks.splitlines() if 'type=SCREEN name=windowLock_' in l]
    _, missions = hdc_shell(hdc, device, 'aa dump -l', timeout=10)
    abilities = []
    for mission in missions.split('Mission ID #'):
        if 'bundle name [app.hackeris.winehua]' in mission:
            abilities.append([l for l in mission.splitlines() if any(k in l for k in ('main name', 'state #', 'app state #'))])
    return {'at': stamp, 'powerExit': rc, 'powerState': state, 'windowLocks': locks, 'appAbilities': abilities}

try:
    result['before'] = snapshot()
    result['override'] = hdc_shell(hdc, device, 'power-shell timeout -o 10000', timeout=10)
    print('SCREEN_TEST foreground idle 18 seconds', flush=True)
    time.sleep(18)
    result['foregroundAfter18Seconds'] = snapshot()
    result['home'] = hdc_shell(hdc, device, 'uitest uiInput keyEvent Home', timeout=10)
    time.sleep(1)
    result['background'] = snapshot()
finally:
    result['restore'] = hdc_shell(hdc, device, 'power-shell timeout -r', timeout=10)
    result['resume'] = hdc_shell(hdc, device, 'aa start -b app.hackeris.winehua -a DesktopAbility', timeout=10)
    _, logs = hdc_shell(hdc, device, 'hilog -z300 -T WineHuaScreenAwake', timeout=10)
    result['screenAwake'] = [l for l in logs.splitlines() if re.search(rf'\s{pid}\s', l) and 'owner=' in l]
    result['afterRestore'] = snapshot()
    path = Path(__file__).parent / 'screen-awake-device.json'
    path.write_text(json.dumps(result, indent=2, ensure_ascii=False) + '\n', encoding='utf8')
    print(json.dumps(result, indent=2, ensure_ascii=False))
