"""Exercise the production bridge and controller hub with real Unix sockets."""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--baseline', action='store_true')
    options = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='gamepad-bridge-test-') as temp:
        folder = Path(temp)
        (folder / 'hilog').mkdir()
        (folder / 'hilog/log.h').write_text('''#pragma once
#define LOG_APP 0
template<typename... Args> static void TestLog(Args&&...) {}
#define OH_LOG_INFO(...) TestLog(__VA_ARGS__)
#define OH_LOG_WARN(...) TestLog(__VA_ARGS__)
#define OH_LOG_ERROR(...) TestLog(__VA_ARGS__)
''')
        bridge = ROOT / 'entry/src/main/cpp/input/controller/gamepad_bridge.cpp'
        if options.baseline:
            for name in ['gamepad_bridge.h', 'gamepad_bridge.cpp']:
                relative = 'entry/src/main/cpp/input/controller/' + name
                path = folder / 'input/controller' / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(subprocess.check_output(['git', 'show', 'HEAD:' + relative], cwd=ROOT))
            bridge = folder / 'input/controller/gamepad_bridge.cpp'
        exe = folder / 'test'
        subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-pthread',
                        '-I' + str(folder), '-I' + str(ROOT / 'entry/src/main/cpp'),
                        str(ROOT / 'host_tests/gamepad_bridge_test.cpp'), str(bridge),
                        str(ROOT / 'entry/src/main/cpp/input/controller/controller_hub.cpp'),
                        '-o', str(exe)], check=True)
        result = subprocess.run([str(exe), str(folder / 'whgp.sock')], timeout=20)
        if options.baseline:
            assert result.returncode != 0, 'old server must fail the multiple-client gate'
            print('baseline reproduced eviction of the first legitimate client')
        else:
            result.check_returncode()


if __name__ == '__main__':
    main()
