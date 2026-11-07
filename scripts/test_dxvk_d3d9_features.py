"""Exercise the actual patched D3D9 feature selection, including strict defaults."""
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent


class D3d9FeatureTests(unittest.TestCase):
    def test_explicit_compatibility_and_strict_default(self):
        with tempfile.TemporaryDirectory(prefix='winehua-d3d9-features-') as temp:
            work = Path(temp)
            source_file = work/'src/d3d9/d3d9_device.cpp'
            source_file.parent.mkdir(parents=True)
            source_file.write_text((ROOT/'thirdparty/dxvk/src/d3d9/d3d9_device.cpp').read_text(encoding='utf-8'), encoding='utf-8')
            subprocess.run(['git','init','-q',str(work)],check=True)
            patch_file = str(ROOT/'scripts/patches/dxvk-legacy-d3d9-features.patch')
            applied = subprocess.run(['git','-C',str(work),'apply','--reverse','--check',patch_file],
                stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL).returncode == 0
            if not applied:
                subprocess.run(['git','-C',str(work),'apply',patch_file],check=True)
            source = source_file.read_text(encoding='utf-8')
            start = source.index('  DxvkDeviceFeatures D3D9DeviceEx::GetDeviceFeatures(')
            end = source.index('  void D3D9DeviceEx::DetermineConstantLayouts',start)
            function = source[start:end]
            fields = {}
            for chain in re.findall(r'(?:supported|enabled)\.([A-Za-z0-9_.]+)',function):
                node = fields
                parts = chain.split('.')
                for part in parts[:-1]: node = node.setdefault(part,{})
                node[parts[-1]] = None
            def members(node):
                return '\n'.join('struct { '+members(child)+' } '+name+';' if child is not None
                                 else 'int '+name+' = 0;' for name,child in node.items())
            harness = r'''
#include <cassert>
#include <memory>
#include <string>
constexpr int VK_TRUE = 1;
struct DxvkDeviceFeatures {
''' + members(fields) + r'''
};
struct DxvkAdapter {
    DxvkDeviceFeatures value;
    DxvkDeviceFeatures features() const { return value; }
};
template<class T> using Rc = std::shared_ptr<T>;
static std::string option;
namespace env { std::string getEnvVar(const char*) { return option; } }
struct D3D9DeviceEx {
    static DxvkDeviceFeatures GetDeviceFeatures(const Rc<DxvkAdapter>&);
};
''' + function + r'''
int main() {
    auto adapter = std::make_shared<DxvkAdapter>();
    for (const char* strict : {"", "0", "true"}) {
        option = strict;
        auto selected = D3D9DeviceEx::GetDeviceFeatures(adapter);
        assert(selected.core.features.textureCompressionBC == VK_TRUE);
        assert(selected.core.features.multiViewport == VK_TRUE);
    }
    option = "1";
    auto missing = D3D9DeviceEx::GetDeviceFeatures(adapter);
    assert(!missing.core.features.textureCompressionBC && !missing.core.features.multiViewport);
    // Required shader functionality must remain required in compatibility mode.
    assert(missing.core.features.geometryShader == VK_TRUE);
    assert(missing.core.features.shaderStorageImageWriteWithoutFormat == VK_TRUE);
    adapter->value.core.features.textureCompressionBC = VK_TRUE;
    adapter->value.core.features.multiViewport = VK_TRUE;
    auto supported = D3D9DeviceEx::GetDeviceFeatures(adapter);
    assert(supported.core.features.textureCompressionBC == VK_TRUE);
    assert(supported.core.features.multiViewport == VK_TRUE);
}
'''
            cpp = work/'features.cpp'
            cpp.write_text(harness,encoding='utf-8')
            exe = work/('features.exe' if os.name == 'nt' else 'features')
            compiler = os.environ.get('CXX') or shutil.which('c++') or shutil.which('clang++')
            self.assertIsNotNone(compiler,'C++ compiler required')
            subprocess.run([compiler,'-std=c++17','-Wall','-Wextra','-Werror',str(cpp),'-o',str(exe)],check=True)
            subprocess.run([str(exe)],check=True)


if __name__ == '__main__':
    unittest.main()
