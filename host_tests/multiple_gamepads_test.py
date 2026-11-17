"""Run the production multi-device adapter, Hub and socket bridge together."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    with tempfile.TemporaryDirectory(prefix='vpp-multiple-gamepads-') as temporary:
        folder = Path(temporary)
        (folder / 'hilog').mkdir()
        (folder / 'hilog/log.h').write_text('''#pragma once
#define LOG_APP 0
template<typename... Args> static void TestLog(Args&&...) {}
#define OH_LOG_INFO(...) TestLog(__VA_ARGS__)
#define OH_LOG_WARN(...) TestLog(__VA_ARGS__)
#define OH_LOG_ERROR(...) TestLog(__VA_ARGS__)
''')
        controller = ROOT / 'entry/src/main/cpp/input/controller'
        executable = folder / 'multiple-gamepads'
        subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-pthread',
                        '-I' + str(folder), '-I' + str(ROOT / 'entry/src/main/cpp'),
                        str(ROOT / 'host_tests/multiple_gamepads_test.cpp'),
                        *(str(controller / name) for name in (
                            'physical_gamepad.cpp', 'controller_hub.cpp', 'gamepad_bridge.cpp')),
                        '-o', str(executable)], check=True, timeout=60)
        subprocess.run([str(executable), str(folder / 'whgp.sock')], check=True, timeout=20)


if __name__ == '__main__':
    main()
