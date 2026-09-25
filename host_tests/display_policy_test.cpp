// display_policy.h (承载/取帧路由命名查询) 的宿主机单元测试 (make test)。
// 覆盖: ⑥ RouteForSubsurface 按类路由 (协议判据 IsInlineClientSurface:
// 空输入区 + 无 viewport source = winewayland client surface) 的模式组合
// 与边界; ⑤ FrameRouteFor 保持既有 PC/Desktop 行为。
#include "compositor/frame/display_policy.h"
#include <cstdio>

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond, msg) do { \
    ++g_checks; \
    if (!(cond)) { \
        ++g_failures; \
        std::printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
    } \
} while (0)

using Route = DisplayPolicy::SubsurfaceRoute;

// -- 1. 判据谓词: 空输入区 + vpSrc 未设 = 客户区 --
static void TestIsInlineClientSurface()
{
    // winewayland client surface: input_region 空 (wayland_surface.c:1191)
    // + 从不设 viewport source (仅设 destination, :672-675)。
    CHECK(DisplayPolicy::IsInlineClientSurface(true, -1),
          "empty input + no vpSrc = client surface");
    // 菜单/子窗口: 每次 attach_shm 必设 source (>0) (wayland_surface.c:488-490)。
    CHECK(!DisplayPolicy::IsInlineClientSurface(true, 640),
          "empty input but vpSrc>0 is not client (e.g. transparent layered child)");
    CHECK(!DisplayPolicy::IsInlineClientSurface(false, -1),
          "input region present + no vpSrc is not client (menu without source)");
    CHECK(!DisplayPolicy::IsInlineClientSurface(false, 640),
          "normal menu is not client");
    // vpSrcW == 0 边界: 0 非合法 source 宽 (1x1 下限经 vpSrcW>=1), 视为未设。
    CHECK(DisplayPolicy::IsInlineClientSurface(true, 0),
          "empty input + vpSrc==0 treated as unset");
}

// -- 2. 路由三分支: Desktop 模式恒 DesktopLayer --
static void TestRouteDesktopMode()
{
    const DisplayPolicy desktop = DisplayPolicy::FromDesktopMode(true);
    CHECK(desktop.RouteForSubsurface(true, -1) == Route::DesktopLayer,
          "desktop mode inlines nothing (all layers into root frame)");
    CHECK(desktop.RouteForSubsurface(false, 640) == Route::DesktopLayer,
          "desktop mode menus also合成 in root frame");
}

// -- 3. 路由三分支: 多窗口 (PC/Pad) 模式按类分流 --
static void TestRouteWindowMode()
{
    const DisplayPolicy win = DisplayPolicy::FromDesktopMode(false);
    CHECK(win.RouteForSubsurface(true, -1) == Route::InlineClient,
          "window mode client surface → inline into parent frame");
    CHECK(win.RouteForSubsurface(false, 640) == Route::Popup,
          "window mode menu → popup subwindow (may overflow parent)");
    CHECK(win.RouteForSubsurface(true, 300) == Route::Popup,
          "window mode transparent layered child → popup (not client)");
}

// -- 4. 既有取帧路由行为不变 --
static void TestFrameRoute()
{
    const DisplayPolicy desktop = DisplayPolicy::FromDesktopMode(true);
    const DisplayPolicy win = DisplayPolicy::FromDesktopMode(false);
    CHECK(desktop.FrameRouteFor(7, 7) == DisplayPolicy::FrameRoute::DesktopRoot,
          "desktop root frame → root composer");
    CHECK(desktop.FrameRouteFor(8, 7) == DisplayPolicy::FrameRoute::Window,
          "desktop non-root → window composer");
    CHECK(win.FrameRouteFor(7, 7) == DisplayPolicy::FrameRoute::Window,
          "window mode never routes to root composer");
}

int main()
{
    TestIsInlineClientSurface();
    TestRouteDesktopMode();
    TestRouteWindowMode();
    TestFrameRoute();

    std::printf("display_policy_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
