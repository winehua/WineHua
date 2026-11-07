"""Exercise real overlay construction and explicit overrides for fence waits."""
from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from test_renderer_lifetime import function

ROOT = Path(__file__).resolve().parents[1]


class VulkanFencePolicyTests(unittest.TestCase):
    def test_overlays_clear_inherited_fast_wait_and_allow_explicit_override(self):
        source = (ROOT/'entry/src/main/cpp/wine/wine_env.cpp').read_text(encoding='utf-8')
        functions = '\n'.join(function(source, signature) for signature in (
            'void UpsertEnvLine(', 'void AppendD3dBackendEnv('))
        harness = r'''
#include <algorithm>
#include <cassert>
#include <string>
#include <vector>
#include "dxvk_profile.h"
#include "mewgenics_graphics.h"
#define WINE_RUNTIME_ROOT "/managed/wine"
ARCHITECTURE
FUNCTIONS
static std::string value(const std::vector<std::string>& env, const std::string& key) {
    const std::string prefix = key + "=";
    std::string result;
    unsigned count = 0;
    for (const auto& line : env) if (line.rfind(prefix, 0) == 0) {
        result = line.substr(prefix.size());
        ++count;
    }
    assert(count == 1);
    return result;
}
int main() {
    const std::string key = "VN_WINEHUA_DIRECT_FENCE_WAIT";
    for (const char* backend : {"vkd3d_limited_500k", "dxvk_legacy", "dxvk_legacy_d3d9", "dxvk_modern_2_6"}) {
        for (const char* dxvk : {"dxvk_legacy", "dxvk_modern_2_6"}) {
            std::vector<std::string> env{key + "=1", "USER_SETTING=preserved"};
            AppendD3dBackendEnv(env, backend, dxvk, "/managed/wine/bin");
            assert(value(env, key) == "0");
            assert(value(env, "USER_SETTING") == "preserved");
            // The normal per-program override stage remains authoritative.
            UpsertEnvLine(env, key + "=1");
            assert(value(env, key) == "1");
        }
    }
    std::vector<std::string> gl;
    for (const auto& pair : winehua::MewgenicsGpuEnvironment("/gfx", "/managed/wine/bin"))
        gl.push_back(pair.first + "=" + pair.second);
    assert(value(gl, key) == "0");
    UpsertEnvLine(gl, key + "=1");
    assert(value(gl, key) == "1");
}
'''.replace('FUNCTIONS', functions)
        compiler = os.environ.get('CXX') or shutil.which('g++') or shutil.which('c++')
        self.assertIsNotNone(compiler, 'C++17 host compiler required')
        with tempfile.TemporaryDirectory(prefix='winehua-fence-policy-') as folder:
            work = Path(folder)
            for architecture in ('native', 'aarch64-policy'):
                code = harness.replace('ARCHITECTURE', '#define __aarch64__ 1' if architecture != 'native' else '')
                src = work/(architecture+'.cpp')
                exe = work/(architecture+('.exe' if sys.platform == 'win32' else ''))
                src.write_text(code, encoding='utf-8')
                flags = ['-std=c++17', '-O1', '-Wall', '-Wextra', '-Werror',
                         '-I', str(ROOT/'entry/src/main/cpp/wine')]
                flags += ['-static'] if sys.platform == 'win32' else ['-fsanitize=address,undefined']
                subprocess.run([compiler, *flags, str(src), '-o', str(exe)], check=True)
                subprocess.run([str(exe)], check=True, timeout=20)


if __name__ == '__main__':
    unittest.main()
