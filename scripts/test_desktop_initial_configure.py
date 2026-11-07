#!/usr/bin/env python3
"""Compile the actual root-recognition hook and check first-buffer configuration."""
from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from test_renderer_lifetime import function

ROOT = Path(__file__).resolve().parents[1]


class DesktopInitialConfigureTests(unittest.TestCase):
    def test_root_without_pixels_receives_session_dimensions(self):
        source = (ROOT / 'entry/src/main/cpp/compositor/wl_core.cpp').read_text(encoding='utf-8')
        hook = function(source, 'void WaylandServer::CheckDesktopRootOnCommit(')
        code = r'''
#include <cassert>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>
#define OH_LOG_INFO(...) ((void)0)
#define OH_LOG_WARN(...) ((void)0)
namespace compositor_consts { const char *kAppIdExplorerTaskbar = "taskbar"; }
struct SurfaceData { bool hasToplevel = true; uint32_t toplevelId = 7; std::string appId = "desktop"; };
struct ToplevelManager {
    struct State { bool frame = false; bool HasFrame() const { return frame; } } state;
    std::mutex mutex;
    auto Lock() { return std::unique_lock<std::mutex>(mutex); }
    State *FindToplevelLocked(uint32_t) { return &state; }
};
struct DesktopRootManager {
    struct CheckRootResult { uint32_t moveRendererFrom = 0, moveRendererTo = 0; bool fireDesktopRoot = false; };
    bool promote = true;
    CheckRootResult CheckRootLocked(SurfaceData *, bool opportunity) {
        return {0, 0, promote && opportunity};
    }
};
struct PluginManager {
    static PluginManager *GetInstance() { static PluginManager p; return &p; }
    void MoveRendererToToplevel(uint32_t, uint32_t) {}
};
enum class ToplevelEventType { DesktopRoot };
struct WaylandServer {
    struct Session { uint32_t desktopRootToplevelId = 0, taskbarId = 0; int outputW = 1024, outputH = 768; } session_;
    struct PolicyState { bool desktop = true; bool RootCompositing() const { return desktop; } } policy;
    ToplevelManager toplevelMgr_;
    DesktopRootManager desktopRootMgr_;
    std::vector<int> events;
    uint32_t configuredId = 0;
    int configuredW = 0, configuredH = 0;
    const PolicyState &Policy() const { return policy; }
    void MarkDesktopRootDirtyLocked() {}
    void ResetFirstFrame() { events.push_back(1); }
    void NotifyToplevelResize(uint32_t id, int w, int h) {
        assert(toplevelMgr_.mutex.try_lock()); // No recursive manager lock.
        toplevelMgr_.mutex.unlock();
        configuredId = id; configuredW = w; configuredH = h; events.push_back(2);
    }
    void PostToplevelEvent(uint32_t, ToplevelEventType) { events.push_back(3); }
    void CheckDesktopRootOnCommit(SurfaceData *, bool);
};
HOOK
int main() {
    SurfaceData sd;
    WaylandServer fixed;
    fixed.CheckDesktopRootOnCommit(&sd, true);
    assert(fixed.configuredId == 7 && fixed.configuredW == 1024 && fixed.configuredH == 768);
    assert((fixed.events == std::vector<int>{1, 2, 3}));
    assert(fixed.session_.outputW == 1024 && fixed.session_.outputH == 768);

    WaylandServer automatic;
    automatic.session_.outputW = 793; automatic.session_.outputH = 351;
    automatic.CheckDesktopRootOnCommit(&sd, true);
    assert(automatic.configuredW == 793 && automatic.configuredH == 351);
    automatic.session_.desktopRootToplevelId = sd.toplevelId;
    automatic.events.clear();
    automatic.CheckDesktopRootOnCommit(&sd, true);
    assert(automatic.events.empty()); // Existing-root commits do not loop configure.

    WaylandServer painted;
    painted.toplevelMgr_.state.frame = true;
    painted.CheckDesktopRootOnCommit(&sd, true);
    assert(!painted.configuredId && (painted.events == std::vector<int>{1, 3}));

    WaylandServer disabled;
    disabled.policy.desktop = false;
    disabled.CheckDesktopRootOnCommit(&sd, true);
    assert(disabled.events.empty());

    WaylandServer unrecognized;
    unrecognized.desktopRootMgr_.promote = false;
    unrecognized.CheckDesktopRootOnCommit(&sd, true);
    assert(unrecognized.events.empty());

    WaylandServer noSize;
    noSize.session_.outputW = 0;
    noSize.CheckDesktopRootOnCommit(&sd, true);
    assert(!noSize.configuredId && (noSize.events == std::vector<int>{1, 3}));
}
'''.replace('HOOK', hook)
        compiler = os.environ.get('CXX') or shutil.which('c++') or shutil.which('g++')
        self.assertIsNotNone(compiler, 'A C++17 host compiler is required.')
        with tempfile.TemporaryDirectory(prefix='winehua-initial-configure-') as directory:
            directory = Path(directory)
            src = directory / 'test.cpp'
            exe = directory / ('test.exe' if sys.platform == 'win32' else 'test')
            src.write_text(code, encoding='utf-8')
            flags = ['-std=c++17', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter']
            flags += ['-static'] if sys.platform == 'win32' else ['-fsanitize=address,undefined', '-pthread']
            subprocess.run([compiler, *flags, str(src), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True, timeout=20)


if __name__ == '__main__':
    unittest.main()
