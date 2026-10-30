#!/usr/bin/env python3
"""Check root-renderer replacement and permanent EGL failure retirement."""
from pathlib import Path
import shutil
import os
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
COMPILER = os.environ.get('CXX', 'c++')


def function(text, signature):
    start = text.index(signature)
    opening = text.index('{', start)
    end, depth = opening + 1, 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


@unittest.skipUnless(shutil.which(COMPILER), 'requires host C++ compiler')
class RendererLifetimeTests(unittest.TestCase):
    def compile_and_run(self, code):
        with tempfile.TemporaryDirectory(prefix='winehua-renderer-lifetime-') as directory:
            src = Path(directory) / 'test.cpp'
            exe = Path(directory) / ('test.exe' if sys.platform == 'win32' else 'test')
            src.write_text(code, encoding='utf-8')
            flags = ['-std=c++17', '-g', '-O1', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter']
            flags += ['-static'] if sys.platform == 'win32' else ['-fsanitize=address,undefined', '-pthread']
            subprocess.run([COMPILER, *flags, str(src), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True, timeout=30)

    def test_surface_resize_and_resume(self):
        source = (ROOT / 'entry/src/main/cpp/bridge/plugin_manager.cpp').read_text(encoding='utf-8')
        resize = function(source, 'void PluginManager::ResizeRenderer(')
        visible = function(source, 'void PluginManager::SetRendererVisible(')
        harness = r'''
#include <cassert>
#include <cstdint>
#include <memory>
#include <unordered_map>
#define OH_LOG_INFO(...) ((void)0)
#define OH_LOG_WARN(...) ((void)0)
enum class ToplevelEventType {Surface};
struct ToplevelEventBus {static int JsonSurface(int,int){return 0;}};
struct WaylandServer {
 int redraws=0, events=0;
 static WaylandServer* GetInstance(){static WaylandServer s;return &s;}
 void ForceToplevelRedraw(uint32_t){redraws++;}
 void PostToplevelEvent(uint32_t,ToplevelEventType,int){events++;}
};
struct Renderer {
 int64_t surface;bool valid=true,visible=true;int w=100,h=80,sets=0;
 explicit Renderer(int64_t value):surface(value){}
 int64_t GetSurfaceId()const{return surface;}
 bool IsValid()const{return valid;}
 int GetWidth()const{return w;}int GetHeight()const{return h;}
 void SetVisible(bool value){visible=value;}
 void SetSize(int x,int y){w=x;h=y;sets++;}
};
struct PluginManager {
 std::unordered_map<uint32_t,std::unique_ptr<Renderer>> toplevelRenderers_;
 int creates=0;
 void CreateRenderer(uint32_t id,int64_t surface){
  creates++;toplevelRenderers_[id]=std::make_unique<Renderer>(surface);
 }
 void ResizeRenderer(uint32_t,int,int,int64_t=0);
 void SetRendererVisible(uint32_t,bool);
};
RESIZE
VISIBLE
int main(){
 PluginManager p;
 p.ResizeRenderer(7,0,768,22);
 p.ResizeRenderer(7,1024,-1,22);
 assert(p.creates==0 && p.toplevelRenderers_.empty());
 p.ResizeRenderer(7,1024,768,22);
 assert(p.creates==1 && p.toplevelRenderers_.at(7)->w==1024);
 auto* renderer=p.toplevelRenderers_.at(7).get();
 p.ResizeRenderer(7,1,1,11);
 assert(p.creates==1 && renderer->w==1024 && renderer->sets==1);
 renderer->valid=false;
 p.SetRendererVisible(7,false);
 assert(p.creates==1 && !renderer->visible);
 p.SetRendererVisible(7,true);
 assert(p.creates==2 && p.toplevelRenderers_.at(7)->IsValid());
 assert(p.toplevelRenderers_.at(7)->w==1024 && p.toplevelRenderers_.at(7)->h==768);
 p.SetRendererVisible(7,true);
 assert(p.creates==2);
 p.toplevelRenderers_.at(7)->valid=false;
 p.ResizeRenderer(7,800,600,22);
 assert(p.creates==3 && p.toplevelRenderers_.at(7)->w==800);
}
'''.replace('RESIZE', resize).replace('VISIBLE', visible)
        self.compile_and_run(harness)

    def test_stale_surface_destruction(self):
        source = (ROOT / 'entry/src/main/cpp/bridge/plugin_manager.cpp').read_text(encoding='utf-8')
        destroy = function(source, 'void PluginManager::DestroyRendererForSurface(')
        harness = r'''
#include <cassert>
#include <cstdint>
#include <memory>
#include <unordered_map>
#define OH_LOG_INFO(...) ((void)0)
struct Renderer {
 int64_t surface;
 explicit Renderer(int64_t value):surface(value) {}
 int64_t GetSurfaceId() const {return surface;}
};
struct PluginManager {
 std::unordered_map<uint32_t,std::unique_ptr<Renderer>> toplevelRenderers_;
 int destroys=0;
 void DestroyToplevel(uint32_t id) {destroys+=toplevelRenderers_.erase(id);}
 void DestroyRendererForSurface(uint32_t,int64_t);
};
FUNCTION
int main() {
 PluginManager p;
 p.toplevelRenderers_.emplace(7,std::make_unique<Renderer>(22));
 // The old XComponent sends its destruction after the replacement is bound.
 p.DestroyRendererForSurface(7,11);
 assert(p.destroys==0 && p.toplevelRenderers_.at(7)->surface==22);
 // The old root ID is no longer the map key after desktop migration.
 p.toplevelRenderers_.emplace(9,std::make_unique<Renderer>(9007199254740997LL));
 p.DestroyRendererForSurface(2,9007199254740997LL);
 assert(p.destroys==1 && !p.toplevelRenderers_.count(9));
 assert(p.toplevelRenderers_.count(7));
 p.DestroyRendererForSurface(7,22);
 p.DestroyRendererForSurface(7,22);
 assert(p.destroys==2 && p.toplevelRenderers_.empty());
 p.toplevelRenderers_.emplace(7,std::make_unique<Renderer>(33));
 p.DestroyRendererForSurface(7,0);
 assert(p.destroys==3 && p.toplevelRenderers_.empty());
}
'''.replace('FUNCTION', destroy)
        self.compile_and_run(harness)

    def test_actual_root_move(self):
        source = (ROOT / 'entry/src/main/cpp/bridge/plugin_manager.cpp').read_text(encoding='utf-8')
        move = function(source, 'void PluginManager::MoveRendererToToplevel(')
        harness = r'''
#include <cassert>
#include <cstdint>
#include <memory>
#include <unordered_map>
#define OH_LOG_INFO(...) ((void)0)
#define OH_LOG_WARN(...) ((void)0)
struct Renderer {
 int id; bool stopped=false; static int stops;
 explicit Renderer(int value):id(value) {}
 void Shutdown() { if (!stopped) { stopped=true; stops++; } }
 ~Renderer() { assert(stopped); }
};
int Renderer::stops=0;
struct PluginManager {
 std::unordered_map<uint32_t,std::unique_ptr<Renderer>> toplevelRenderers_;
 void MoveRendererToToplevel(uint32_t,uint32_t);
 ~PluginManager(){ for(auto &entry:toplevelRenderers_)entry.second->Shutdown(); }
};
FUNCTION
int main() {
 PluginManager p;
 for(unsigned i=1;i<100;i++)p.toplevelRenderers_.emplace(i,std::make_unique<Renderer>(i));
 p.MoveRendererToToplevel(1,2);
 assert(p.toplevelRenderers_.size()==98 && !p.toplevelRenderers_.count(1));
 assert(p.toplevelRenderers_.at(2)->id==1 && !p.toplevelRenderers_.at(2)->stopped);
 assert(Renderer::stops==1);
 p.MoveRendererToToplevel(2,2);p.MoveRendererToToplevel(1000,1001);
 assert(p.toplevelRenderers_.size()==98 && Renderer::stops==1);
 for(unsigned i=100;i<2100;i++) {
  p.MoveRendererToToplevel(i==100?2:i-1,i);
  assert(p.toplevelRenderers_.at(i)->id==1);
 }
 assert(p.toplevelRenderers_.size()==98 && Renderer::stops==1);
}
'''.replace('FUNCTION', move)
        self.compile_and_run(harness)

    def test_invalid_surface_threshold(self):
        source = (ROOT / 'entry/src/main/cpp/graphics/egl_renderer.cpp').read_text(encoding='utf-8')
        start = source.index('if (!swapOk &&', source.index('const bool swapOk = eglSwapBuffers'))
        condition = source[start + 4:source.index(' {', start) - 1]
        code = '''#include <cassert>
enum {EGL_SUCCESS, EGL_BAD_SURFACE, EGL_BAD_NATIVE_WINDOW, EGL_CONTEXT_LOST, EGL_BAD_ALLOC};
bool retire(bool swapOk,int swapFailStreak_,int swapError,bool visible=true){
 std::atomic<bool> visible_(visible);return CONDITION;}
int main(){
 for(int e: {EGL_BAD_SURFACE,EGL_BAD_NATIVE_WINDOW,EGL_CONTEXT_LOST}) {
  assert(!retire(false,1,e));assert(!retire(false,2,e));assert(retire(false,3,e));
  assert(!retire(true,3,e));
  assert(!retire(false,3,e,false));
 }
 assert(!retire(false,100,EGL_BAD_ALLOC));assert(!retire(true,0,EGL_SUCCESS));
}
'''.replace('#include <cassert>', '#include <cassert>\n#include <initializer_list>\n#include <atomic>').replace('CONDITION', condition)
        self.compile_and_run(code)


if __name__ == '__main__':
    unittest.main()
