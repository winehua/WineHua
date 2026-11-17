#!/usr/bin/env python3
"""Execute the patched DXVK front-buffer policy and real rotation loop.

This checks retained contents after Present, plus fullscreen FLIP/DISCARD and
the legacy opt-out. It does not replace a Vulkan/device regression.
"""
import argparse
import io
import json
from pathlib import Path
import subprocess
import sys
import tarfile
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def function(text, name):
    start = text.index(name)
    begin = text.index('{', start)
    depth = 1
    end = begin + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


def run(source):
    text = (source / 'src/d3d9/d3d9_swapchain.cpp').read_text()
    policy = function(text, 'bool D3D9SwapChainEx::HasFrontBuffer() const')
    present = function(text, 'void D3D9SwapChainEx::PresentImage(')
    rotation_start = present.index('for (uint32_t i = 1; i < m_backBuffers.size(); i++)')
    rotation_end = present.index(';', present.index('->Swap(', rotation_start)) + 1
    rotation = present[rotation_start:rotation_end]
    create = function(text, 'void D3D9SwapChainEx::CreateBackBuffers(')
    allocation = next(line.strip() for line in create.splitlines() if 'int NumFrontBuffer =' in line)
    code = r'''
#include <cassert>
#include <cstdint>
#include <vector>
#include <iostream>
enum { D3DSWAPEFFECT_DISCARD=1, D3DSWAPEFFECT_FLIP=2,
       D3DSWAPEFFECT_COPY=3, D3DSWAPEFFECT_COPY_VSYNC=4 };
struct Options { bool noExplicitFrontBuffer=false; };
struct Parent { Options options; const Options* GetOptions() const { return &options; } };
struct Surface {
  int color=0;
  Surface* ptr() { return this; }
  Surface* operator->() { return this; }
  void Swap(Surface* other) { std::swap(color, other->color); }
};
class D3D9SwapChainEx {
 public:
  Parent parent;
  const Parent* m_parent=&parent;
  struct { int SwapEffect=1; uint32_t BackBufferCount=1; bool Windowed=true; } m_presentParams;
  std::vector<Surface> m_backBuffers;
  bool HasFrontBuffer() const;
  void create(uint32_t NumBackBuffers) {
    ALLOCATION
    m_backBuffers.resize(NumBackBuffers + NumFrontBuffer);
  }
  void present() { ROTATION }
};
POLICY
int main() {
  int cases=0;
  for (int effect : {1,2,3,4}) for (bool windowed : {false,true})
  for (uint32_t count : {1u,2u}) for (bool disabled : {false,true}) {
    D3D9SwapChainEx s;
    s.m_presentParams={effect,count,windowed};
    s.parent.options.noExplicitFrontBuffer=disabled;
    const bool retained = effect==3 || effect==4 || (effect==1 && windowed && count==1);
    const bool front = !disabled && !retained;
    assert(s.HasFrontBuffer()==front);
    s.create(count);
    assert(s.m_backBuffers.size()==count+uint32_t(front));
    // COPY has one legal backbuffer. A static background is drawn once;
    // subsequent Presents must not replace it with an unused black image.
    if (retained && count==1) {
      s.m_backBuffers[0].color=0x123456;
      for (int frame=0;frame<8;frame++) {
        assert(s.m_backBuffers[0].color==0x123456);
        s.present();
        assert(s.m_backBuffers[0].color==0x123456);
      }
    } else {
      // FLIP/fullscreen DISCARD must still rotate the requested ring.
      for (uint32_t i=0;i<s.m_backBuffers.size();i++) s.m_backBuffers[i].color=int(i+1);
      s.present();
      assert(s.m_backBuffers.back().color==1);
      if (s.m_backBuffers.size()>1) assert(s.m_backBuffers[0].color==2);
    }
    cases++;
  }
  std::cout << cases << " buffer-policy cases and retained-frame/rotation checks PASS\n";
}
'''.replace('ALLOCATION', allocation).replace('ROTATION', rotation).replace('POLICY', policy)
    with tempfile.TemporaryDirectory(prefix='dxvk9-backbuffer-') as folder:
        p = Path(folder)
        (p/'test.cpp').write_text(code)
        subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', str(p/'test.cpp'), '-o', str(p/'test')], check=True)
        subprocess.run([str(p/'test')], check=True)
        # Prove the original unconditional extra-frontbuffer policy fails
        # the same behavioral assertions, instead of only checking strings.
        old = code.replace(policy, '''bool D3D9SwapChainEx::HasFrontBuffer() const {
          return !m_parent->GetOptions()->noExplicitFrontBuffer;
        }''')
        (p/'old.cpp').write_text(old)
        subprocess.run(['g++', '-std=c++17', str(p/'old.cpp'), '-o', str(p/'old')], check=True)
        failed = subprocess.run([str(p/'old')], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        assert failed.returncode != 0, 'old policy unexpectedly passed'
        print('Original policy rejected by the same assertions')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repository', type=Path, default=ROOT / 'thirdparty/dxvk')
    parser.add_argument('--source', type=Path, help='Already patched source; otherwise replay the pinned overlay stack in a temporary tree')
    args = parser.parse_args()
    if args.source:
        run(args.source)
    else:
        sys.path.insert(0, str(ROOT / 'scripts'))
        from apply_dxvk_compat_patches import apply
        pin = json.loads((ROOT / 'docs/graphics/texture-compat-runtime.lock.json').read_text())['runtimes']['legacy']
        source = args.repository
        archive = subprocess.check_output(['git', '-c', 'safe.directory=' + str(source),
            '-C', str(source), 'archive', pin['commit'], 'src'])
        with tempfile.TemporaryDirectory(prefix='dxvk9-backbuffer-source-') as folder:
            proof = Path(folder)
            with tarfile.open(fileobj=io.BytesIO(archive)) as files:
                files.extractall(proof, filter='data')
            subprocess.run(['git', 'init', '-q', str(proof)], check=True)
            apply(proof)
            run(proof)
