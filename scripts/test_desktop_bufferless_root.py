#!/usr/bin/env python3
"""Exercise actual compositor gates, base copies and scene signatures without root SHM."""
from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from test_renderer_lifetime import function

ROOT = Path(__file__).resolve().parents[1]


class BufferlessDesktopRootTests(unittest.TestCase):
    def test_real_child_pixels_do_not_wait_for_shell_backdrop(self):
        source = (ROOT / 'entry/src/main/cpp/compositor/frame/frame_pipeline.cpp').read_text(encoding='utf-8')
        functions = '\n'.join(function(source, name) for name in (
            'void FillBlackRect(', 'FramePlanOutcome FramePlanner::GateDesktopDirtyLocked(',
            'void FramePlanner::CopyBaseToOutputLocked(',
            'uint64_t FramePlanner::ComputeCompositionSignatureLocked('))
        harness = r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <vector>
#include <unordered_map>
#include "compositor/frame/blit_clip.h"
#define OH_LOG_INFO(...) ((void)0)
#define OH_LOG_WARN(...) ((void)0)
namespace compositor_consts {
constexpr uint64_t kFnv1aOffsetBasis = 14695981039346656037ull;
constexpr uint64_t kFnv1aPrime = 1099511628211ull;
}
struct DamageRect { int x=0,y=0,w=0,h=0; bool full=true; };
struct SubsurfaceLayer {
    std::vector<uint8_t> pixels;
    void* surface = nullptr;
    int vpDstW=-1,vpDstH=-1;
};
struct CompositorLayer {
    enum class Type { Root,Toplevel,Subsurface };
    Type type=Type::Root;
    bool visible=true, zcActive=false, fullscreen=false;
    uint32_t toplevelId=0;
    int x=0,y=0,w=0,h=0;
    const SubsurfaceLayer* sub=nullptr;
    bool ShouldSkipCpu() const { return !visible || zcActive; }
};
struct ToplevelManager {
    struct ToplevelState {
        std::vector<uint8_t> pixels;
        bool dirty=true;
        int w=0,h=0;
        bool HasFrame() const { return !pixels.empty(); }
        bool IsDirty() const { return dirty; }
        void ClearDirty() { dirty=false; }
        int Width() const { return w; } int Height() const { return h; }
        int ShmFormat() const { return 1; }
        const std::vector<uint8_t>& Pixels() const { return pixels; }
    };
    std::unordered_map<uint32_t,ToplevelState> states;
    std::vector<uint32_t> order;
    ToplevelState* FindToplevelLocked(uint32_t id) {
        auto it=states.find(id);return it==states.end()?nullptr:&it->second;
    }
    const auto& toplevelZOrder() const { return order; }
};
struct DesktopCompositor {
    int outputW_=7,outputH_=5;
    bool desktopOutputInitialized_=false;
    uint32_t desktopRootlessSceneId_=0;
    uint64_t desktopOutputRootFrameSerial_=0,desktopRootFrameSerial_=0,desktopCompositionSignature_=0;
    std::vector<SubsurfaceLayer> subsurfaceLayers_;
    std::vector<CompositorLayer> layers;
    auto BuildLayerListLocked(int,int) const { return layers; }
};
struct FramePlan {
    int rootW=0,rootH=0;
    bool rootHasPixels=false,rebuildBase=false;
    uint64_t compositionSignature=0;
    DamageRect dmg;
    std::vector<CompositorLayer> layers;
};
struct PresentedFrame {
    enum class Kind { Composed };
    enum class BaseSpace { Desktop };
    Kind kind; BaseSpace baseSpace;
    int w=0,h=0,contentW=0,contentH=0;
    bool opaque=false;
};
enum class FramePlanOutcome { kNoFrame,kFastPath,kCompose };
struct FramePlanner {
    DesktopCompositor& comp_; ToplevelManager& tmgr_;
    FramePlanOutcome GateDesktopDirtyLocked(uint32_t,FramePlan&,std::vector<uint8_t>&,PresentedFrame&,
                                            ToplevelManager::ToplevelState*&);
    void CopyBaseToOutputLocked(const ToplevelManager::ToplevelState*,FramePlan&,std::vector<uint8_t>&);
    uint64_t ComputeCompositionSignatureLocked(uint32_t,const FramePlan&) const;
};
FUNCTIONS
int main() {
    DesktopCompositor comp; ToplevelManager tm;
    auto& root=tm.states[1];
    FramePlanner planner{comp,tm};
    std::vector<uint8_t> out; PresentedFrame frame;
    ToplevelManager::ToplevelState* state=nullptr;
    auto gate=[&](FramePlan& p){return planner.GateDesktopDirtyLocked(1,p,out,frame,state);};
    FramePlan empty;
    assert(gate(empty)==FramePlanOutcome::kNoFrame); // Do not manufacture a guest's first frame.
    assert(!root.HasFrame() && comp.desktopRootlessSceneId_==0);
    tm.states[2].pixels.assign(3*2*4,255);tm.states[2].w=3;tm.states[2].h=2;
    tm.order={2};
    CompositorLayer child;child.type=CompositorLayer::Type::Toplevel;child.toplevelId=2;
    child.x=1;child.y=1;child.w=3;child.h=2;comp.layers={child};
    FramePlan first;
    assert(gate(first)==FramePlanOutcome::kCompose);
    assert(!first.rootHasPixels && first.rootW==7 && first.rootH==5);
    assert(!root.HasFrame() && root.w==0 && root.h==0); // Root state is untouched.
    first.rebuildBase=true;first.compositionSignature=planner.ComputeCompositionSignatureLocked(1,first);
    planner.CopyBaseToOutputLocked(&root,first,out);
    assert(out.size()==7*5*4);
    for(size_t i=0;i<out.size();i+=4) {
        assert(out[i]==0 && out[i+1]==0 && out[i+2]==0 && out[i+3]==255);
    }
    // Local redraw clears only its damage; output outside the region is retained.
    std::fill(out.begin(),out.end(),0x77);
    FramePlan partial;partial.rootW=7;partial.rootH=5;
    partial.dmg={2,1,3,2,false};
    planner.CopyBaseToOutputLocked(&root,partial,out);
    for(int y=0;y<5;y++)for(int x=0;x<7;x++) {
        const size_t i=(y*7+x)*4;
        const bool dirty=x>=2 && x<5 && y>=1 && y<3;
        assert(out[i]==(dirty?0:0x77) && out[i+3]==(dirty?255:0x77));
    }
    // Real root pixels invalidate the background signature even at the same size.
    FramePlan painted=first;painted.rootHasPixels=true;
    assert(planner.ComputeCompositionSignatureLocked(1,painted)!=first.compositionSignature);
    root.pixels.assign(7*5*4,0x53);root.w=7;root.h=5;
    painted.rebuildBase=true;planner.CopyBaseToOutputLocked(&root,painted,out);
    assert(out==root.pixels);
    FramePlan normal;assert(gate(normal)==FramePlanOutcome::kCompose);
    assert(normal.rootHasPixels && comp.desktopRootlessSceneId_==0);
    // Preserve the normal root-only fast path.
    comp.layers.clear();tm.order.clear();comp.subsurfaceLayers_.clear();
    FramePlan fast;assert(gate(fast)==FramePlanOutcome::kFastPath);
    assert(out==root.pixels && !root.dirty);
    root.pixels.clear();root.w=root.h=0;root.dirty=true;
    comp.desktopRootlessSceneId_=0;
    // Hidden/empty/ZC placeholder layers do not count as real CPU content.
    child.visible=false;comp.layers={child};
    FramePlan hidden;assert(gate(hidden)==FramePlanOutcome::kNoFrame);
    child.visible=true;child.zcActive=true;comp.layers={child};
    FramePlan zc;assert(gate(zc)==FramePlanOutcome::kNoFrame);
    // A subsurface can be ready before its parent supplies a backing buffer.
    SubsurfaceLayer sub;sub.pixels.assign(2*2*4,0xff);
    child.type=CompositorLayer::Type::Subsurface;child.zcActive=false;child.sub=&sub;
    comp.layers={child};
    FramePlan subframe;assert(gate(subframe)==FramePlanOutcome::kCompose);
    // Removing the last child (including after a direct pass) clears once.
    comp.layers.clear();
    FramePlan removed;assert(gate(removed)==FramePlanOutcome::kCompose);
    removed.rebuildBase=true;
    planner.CopyBaseToOutputLocked(&root,removed,out);
    assert(out[0]==0 && out[3]==255);
    root.ClearDirty();
    for(int i=0;i<1000;i++){FramePlan idle;assert(gate(idle)==FramePlanOutcome::kNoFrame);}
    root.dirty=true;comp.outputW_=0;
    FramePlan invalid;assert(gate(invalid)==FramePlanOutcome::kNoFrame);
    tm.states.erase(1);
    FramePlan missing;assert(gate(missing)==FramePlanOutcome::kNoFrame);
}
'''.replace('FUNCTIONS', functions)
        compiler = os.environ.get('CXX') or shutil.which('c++') or shutil.which('g++')
        self.assertIsNotNone(compiler, 'Set CXX to a host C++17 compiler.')
        with tempfile.TemporaryDirectory(prefix='winehua-bufferless-root-') as directory:
            src = Path(directory) / 'test.cpp'
            exe = Path(directory) / ('test.exe' if sys.platform == 'win32' else 'test')
            src.write_text(harness, encoding='utf-8')
            flags = ['-std=c++17', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
                     '-I', str(ROOT / 'entry/src/main/cpp')]
            flags += ['-static'] if sys.platform == 'win32' else ['-fsanitize=address,undefined', '-pthread']
            subprocess.run([compiler, *flags, str(src), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True, timeout=30)


if __name__ == '__main__':
    unittest.main()
