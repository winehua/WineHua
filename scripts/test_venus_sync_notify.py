"""Exercise the real ring notification expression, including publication order."""
from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class SyncNotifyTests(unittest.TestCase):
    def test_notification_policy_and_publication(self):
        patch = (ROOT / 'scripts/patches/mesa-venus-sync-notify.patch').read_text()
        self.assertIn('winehua_sync_notify && extra_shmem', patch)
        expression = '''vn_ring_submit_internal(ring, submit.submit, submit.cs, &seqno) ||
                        (ring->winehua_sync_notify && extra_shmem)'''
        code = r'''
#include <cassert>
#include <cstdint>
struct Ring { bool winehua_sync_notify, normal_notify; int published = 0; };
struct Submit { void *submit = nullptr, *cs = nullptr; };
static bool vn_ring_submit_internal(Ring *ring, void *, void *, uint32_t *seqno) {
    assert(ring->published == 0);
    ring->published++;
    *seqno = 42;
    return ring->normal_notify;
}
static bool policy(Ring *ring, void *extra_shmem) {
    Submit submit;
    uint32_t seqno = 0;
    const bool notify = EXPRESSION;
    assert(ring->published == 1 && seqno == 42);
    return notify;
}
int main() {
    int reply = 0;
    // Indexed by existing notification, sync option, and reply ownership.
    const bool expected[] = {false, false, false, true, true, true, true, true};
    for (unsigned mask = 0; mask != 8; mask++) {
        Ring ring{bool(mask & 2), bool(mask & 4)};
        assert(policy(&ring, mask & 1 ? &reply : nullptr) == expected[mask]);
    }
}
'''.replace('EXPRESSION', expression)
        compiler = os.environ.get('CXX') or shutil.which('g++') or shutil.which('c++')
        self.assertIsNotNone(compiler, 'host C++ compiler required')
        with tempfile.TemporaryDirectory(prefix='jiuyu-sync-notify-') as temp:
            cpp = Path(temp) / 'test.cpp'
            exe = Path(temp) / ('test.exe' if sys.platform == 'win32' else 'test')
            cpp.write_text(code)
            flags = ['-std=c++17', '-O1', '-Wall', '-Wextra', '-Werror']
            flags += ['-static'] if sys.platform == 'win32' else ['-fsanitize=address,undefined']
            subprocess.run([compiler, *flags, str(cpp), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    unittest.main()
