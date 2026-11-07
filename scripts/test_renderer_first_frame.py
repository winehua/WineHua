#!/usr/bin/env python3
"""Exercise actual renderer creation and visibility-triggered presentation state."""
from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from test_renderer_lifetime import function

ROOT = Path(__file__).resolve().parents[1]
COMPILER = os.environ.get('CXX') or shutil.which('c++') or shutil.which('g++')


class RendererFirstFrameTests(unittest.TestCase):
    def compile_and_run(self, source):
        self.assertIsNotNone(COMPILER, 'A C++17 host compiler is required.')
        with tempfile.TemporaryDirectory(prefix='winehua-first-frame-') as directory:
            directory = Path(directory)
            src = directory / 'check.cpp'
            exe = directory / ('check.exe' if sys.platform == 'win32' else 'check')
            src.write_text(source, encoding='utf-8')
            flags = ['-std=c++17', '-g', '-O1', '-Wall', '-Wextra', '-Werror',
                     '-Wno-unused-parameter', '-I', str(ROOT / 'entry/src/main/cpp')]
            flags += ['-static'] if sys.platform == 'win32' else ['-fsanitize=address,undefined', '-pthread']
            subprocess.run([COMPILER, *flags, str(src), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True, timeout=30)

    def test_request_and_successful_presentation_order(self):
        self.compile_and_run((ROOT / 'host_tests/renderer_repaint_state_test.cpp').read_text(encoding='utf-8'))

    def test_same_size_native_buffer_requests_redraw(self):
        setter = function((ROOT / 'entry/src/main/cpp/graphics/egl_renderer.h').read_text(encoding='utf-8'),
                          'void SetSize(')
        self.compile_and_run(r'''
#include "graphics/renderer_repaint_state.h"
#include <cassert>
struct Renderer {
    winehua::RendererRepaintState repaint_;
    int expectW_ = 800, expectH_ = 600;
    SETTER
};
int main() {
    Renderer r;
    r.repaint_.DidPresent(r.repaint_.CaptureRequest());
    r.SetSize(800, 600);
    assert(r.repaint_.NeedsRedraw(r.repaint_.CaptureRequest()));
    assert(r.expectW_ == 800 && r.expectH_ == 600);
    r.repaint_.DidPresent(r.repaint_.CaptureRequest());
    assert(!r.repaint_.NeedsRedraw(r.repaint_.CaptureRequest()));
}
'''.replace('SETTER', setter))

    def test_failed_swap_retries_consumed_frame_at_same_size(self):
        source = (ROOT / 'entry/src/main/cpp/graphics/egl_renderer.cpp').read_text(encoding='utf-8')
        start = source.index('        if (swapOk) {', source.index('const bool swapOk = eglSwapBuffers'))
        end = source.index('        // A destroyed XComponent', start)
        self.compile_and_run(r'''
#include "graphics/renderer_repaint_state.h"
#include <cassert>
#define OH_LOG_WARN(...) ((void)0)
namespace winehua {
struct LivePerformance {
    int presents = 0;
    static LivePerformance& Instance() { static LivePerformance s; return s; }
    void Present() { presents++; }
};
}
struct Renderer {
    winehua::RendererRepaintState repaint_;
    int lastDrawW_ = 800, lastDrawH_ = 600, swapFailStreak_ = 0;
    void SwapResult(bool swapOk) {
        const auto repaintRequest = repaint_.CaptureRequest();
        const int drawW = 800, drawH = 600;
        RESULT
    }
};
int main() {
    Renderer r;
    r.SwapResult(true);
    assert(!r.repaint_.NeedsRedraw(r.repaint_.CaptureRequest()));
    // The next guest frame is consumed, but swapping it fails transiently.
    r.SwapResult(false);
    assert(r.repaint_.NeedsRedraw(r.repaint_.CaptureRequest()));
    assert(winehua::LivePerformance::Instance().presents == 1);
    r.SwapResult(false);
    assert(r.repaint_.NeedsRedraw(r.repaint_.CaptureRequest()));
    assert(r.swapFailStreak_ == 2);
    r.SwapResult(true);
    assert(!r.repaint_.NeedsRedraw(r.repaint_.CaptureRequest()));
    assert(r.swapFailStreak_ == 0);
    assert(winehua::LivePerformance::Instance().presents == 2);
}
'''.replace('RESULT', source[start:end]))

    def test_actual_create_requests_retained_first_frame(self):
        create = function((ROOT / 'entry/src/main/cpp/bridge/plugin_manager.cpp').read_text(encoding='utf-8'),
                          'void PluginManager::CreateRenderer(')
        self.compile_and_run(r'''
#include <cassert>
#include <cstdint>
#include <memory>
#include <unordered_map>
#define OH_LOG_INFO(...) ((void)0)
#define OH_LOG_WARN(...) ((void)0)
#define OH_LOG_ERROR(...) ((void)0)
struct OHNativeWindow {};
static OHNativeWindow nativeWindow;
static bool windowReady = true, initSuccess = true;
static int shutdowns = 0;
static bool x11Active = false, attachSuccess = true;
static int attaches = 0, destroyedWindows = 0;
static bool x11_toplevel_active() { return x11Active; }
static bool x11_toplevel_attach_window(uint32_t, OHNativeWindow *, int, int) {
    attaches++;
    return attachSuccess;
}
static void OH_NativeWindow_DestroyNativeWindow(OHNativeWindow *) { destroyedWindows++; }
static int OH_NativeWindow_CreateNativeWindowFromSurfaceId(uint64_t, OHNativeWindow **out) {
    *out = windowReady ? &nativeWindow : nullptr;
    return windowReady ? 0 : -1;
}
struct WaylandServer {
    int compositor = 0, redraws = 0;
    uint32_t dirty = 0;
    static WaylandServer *GetInstance() { static WaylandServer s; return &s; }
    int &GetDesktopCompositor() { return compositor; }
    void ForceToplevelRedraw(uint32_t id) { redraws++; dirty = id; }
};
struct EglRenderer {
    uint32_t id = 0;
    int64_t surface = 0;
    explicit EglRenderer(int &) {}
    void SetToplevelId(uint32_t value) { id = value; }
    void SetSurfaceId(int64_t value) { surface = value; }
    bool Init(OHNativeWindow *, int, int) { return initSuccess; }
    void Shutdown() { shutdowns++; }
};
struct PluginManager {
    std::unordered_map<uint32_t, std::unique_ptr<EglRenderer>> toplevelRenderers_;
    void CreateRenderer(uint32_t, int64_t);
};
CREATE
int main() {
    PluginManager p;
    auto &server = *WaylandServer::GetInstance();
    // No new commit, no resize callback: a first display still needs the
    // existing root frame, including after its previous consumer is gone.
    p.CreateRenderer(7, 11);
    assert(server.redraws == 1 && server.dirty == 7);
    server.dirty = 0; // Previous renderer consumed the frame.
    p.CreateRenderer(7, 22);
    assert(shutdowns == 1 && server.redraws == 2 && server.dirty == 7);
    assert(p.toplevelRenderers_.at(7)->surface == 22);
    windowReady = false;
    p.CreateRenderer(7, 33);
    assert(shutdowns == 1 && server.redraws == 2);
    assert(p.toplevelRenderers_.at(7)->surface == 22);
    windowReady = true;
    initSuccess = false;
    p.CreateRenderer(7, 44);
    assert(shutdowns == 2 && server.redraws == 2 && p.toplevelRenderers_.empty());
    x11Active = true;
    p.CreateRenderer(8, 55);
    assert(attaches == 1 && p.toplevelRenderers_.empty() && server.redraws == 2);
    attachSuccess = false;
    p.CreateRenderer(8, 66);
    assert(attaches == 2 && destroyedWindows == 1 && p.toplevelRenderers_.empty());
}
'''.replace('CREATE', create))

    def test_actual_visibility_requests_even_when_already_visible(self):
        visible = function((ROOT / 'entry/src/main/cpp/graphics/egl_renderer.cpp').read_text(encoding='utf-8'),
                           'void EglRenderer::SetVisible(')
        self.compile_and_run(r'''
#include "graphics/renderer_repaint_state.h"
#include <cassert>
#include <condition_variable>
#define OH_LOG_INFO(...) ((void)0)
struct EglRenderer {
    winehua::RendererRepaintState repaint_;
    std::atomic<bool> visible_{true};
    std::condition_variable visibilityCv_;
    void SetVisible(bool);
};
VISIBLE
int main() {
    EglRenderer r;
    r.repaint_.DidPresent(r.repaint_.CaptureRequest());
    r.SetVisible(true); // WINDOW_SHOWN after onForeground.
    assert(r.repaint_.NeedsRedraw(r.repaint_.CaptureRequest()));
    r.repaint_.DidPresent(r.repaint_.CaptureRequest());
    r.SetVisible(false);
    assert(!r.repaint_.NeedsRedraw(r.repaint_.CaptureRequest()));
    r.SetVisible(true);
    assert(r.repaint_.NeedsRedraw(r.repaint_.CaptureRequest()));
}
'''.replace('VISIBLE', visible))


if __name__ == '__main__':
    unittest.main()
