/*
 * display_compositor.cpp — 显示路线 M0 bring-up (spec: x11-wlroots 设计)
 *
 * app 进程内起一个 wlroots 合成器内核:
 *   wl_display + headless backend + pixman renderer + wlr_compositor
 *   + wlr_xwayland_create (Xwayland rootless, 启动走 NCP)
 *
 * Xwayland 启动链 (R-SPAWN):
 *   wlroots server.c (补丁) → wlr_ohos_spawn_xwayland (本文件实现)
 *   → OH_Ability_StartNativeChildProcess("libxwayland_child.so:Main")
 *   → 命名 fd 五连 (x_fd0/x_fd1/wl_fd/wm_fd/displayfd) 随 args.fdList 下发
 *   → shim 在子进程内 dlopen libxwayland_ohos.so 调 main()
 *
 * 触发方式: smoke 调试命令 (测试设施不进产品路径, principles #22)
 * 由 smoke_napi.cpp 经 dlopen(libentry.so) 调 WineHua_DisplayRoute_Start。
 */
#include <hilog/log.h>

#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <mutex>
#include <string>
#include <thread>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/stat.h>

#include <wayland-server-core.h>

// wlroots 头文件不 C++ 安全 (color.h 的 C99 [static n] 参数语法; xwayland.h
// 的 `char *class` 撞 C++ 关键字), 只 include xwayland/server.h (include 链
// C++ 安全), 其余用不透明指针手写原型; 签名与 wlroots 0.20.2 逐一核对:
// backend/headless.h, render/pixman.h, types/wlr_compositor.h,
// xwayland/xwayland.h (wlr_xwayland_create_with_server)。
#define WLR_USE_UNSTABLE
extern "C" {
#include <wlr/util/log.h>
#include <wlr/xwayland/server.h>
struct wlr_renderer;
struct wlr_compositor;
struct wlr_backend;
struct wlr_xwayland;
struct wlr_renderer *wlr_pixman_renderer_create(void);
struct wlr_compositor *wlr_compositor_create(struct wl_display *display,
	uint32_t version, struct wlr_renderer *renderer);
struct wlr_backend *wlr_headless_backend_create(struct wl_event_loop *loop);
bool wlr_backend_start(struct wlr_backend *backend);
struct wlr_xwayland *wlr_xwayland_create_with_server(struct wl_display *display,
	struct wlr_compositor *compositor, struct wlr_xwayland_server *server);
// WineHua 补丁新增 (scripts/patches/wlroots-ohos-ncp-spawn.patch)
bool wlr_xwayland_server_ohos_build_argv(struct wlr_xwayland_server *server,
	int notify_fd, char *argv[], size_t argv_max);
}

#include <AbilityKit/native_child_process.h>

#define LOG_TAG "DisplayRoute"

namespace {

std::mutex g_mutex;
bool g_started = false;
bool g_stop = false;

std::string BoolStr(bool b) { return b ? "1" : "0"; }

// wlroots 日志桥 (bring-up 观测): wlroots 默认写 app stderr, 主进程不可见。
// va_list 不能直接透给 OH_LOG 的可变参数, 先 vsnprintf 再整串发。
void WlrLogBridge(enum wlr_log_importance importance, const char *fmt, va_list args)
{
    char buf[512];
    vsnprintf(buf, sizeof(buf), fmt, args);
    // wlr 严重度: ERROR=1 < INFO=2 < DEBUG=3 (数值越大越轻)
    OH_LOG_Print(LOG_APP, importance == WLR_ERROR ? LOG_ERROR : LOG_INFO,
                 0, "WLR", "%{public}s", buf);
}

// libwayland-server 核心日志桥 (协议错误如 dispatch/marshal 失败走这里)
void WlServerLogBridge(const char *fmt, va_list args)
{
    char buf[512];
    vsnprintf(buf, sizeof(buf), fmt, args);
    OH_LOG_INFO(LOG_APP, "wl-server: %{public}s", buf);
}

// displayfd 握手完成的 hilog 直证 (wlroots 自身日志走 stderr, 主进程不可见)
void HandleXwaylandReady(struct wl_listener *listener, void *data)
{
    OH_LOG_INFO(LOG_APP, "Xwayland ready signal received (displayfd handshake OK)");
}
struct wl_listener g_xwayland_ready_listener;

} // namespace

// ── wlroots 补丁的 spawn 钩子 (server.c 调用) ──────────────────────────
extern "C" bool wlr_ohos_spawn_xwayland(struct wlr_xwayland_server *server,
                                        int display_fd)
{
    // argv 哨兵版 (fd 数值由 shim 按名替换); 日志留证
    char *argv[64] = {};
    if (!wlr_xwayland_server_ohos_build_argv(server, display_fd, argv, 64))
    {
        OH_LOG_ERROR(LOG_APP, "build argv failed");
        return false;
    }
    std::string joined;
    for (size_t i = 0; argv[i]; ++i)
        joined += (i ? " " : "") + std::string(argv[i]);
    OH_LOG_INFO(LOG_APP, "spawn argv: %{public}s", joined.c_str());

    // entryParams: name|terminateDelay|noTouch|xrandr|enableWm|stderrPath|appPid
    //              |xdgRuntimeDir|argv0|argv1|...
    // argv 从第 9 字段起逐项传递——app 侧 build_argv 是 argv 的唯一来源
    // (shim 只解析, 不再硬编码副本; 两份人肉对齐的实现曾漂移出缺 -xkbdir
    // 的 argc=16, T7 实测)。fd 哨兵 (@XFD0@ 等) 由 shim 按名替换为实际 fd。
    // xdgRuntimeDir: NCP 子进程不继承 app 环境, 显式下发——Xwayland 的
    // OutputDirectory (xkb/ddxLoad.c:65) 依赖它定位 xkm 缓存目录。
    // stderrPath: Xwayland stdout/stderr 落盘文件 (沙箱内, 供排障)
    std::string xdg = getenv("XDG_RUNTIME_DIR") ? getenv("XDG_RUNTIME_DIR") : "";
    std::string params = std::string(server->display_name) + "|" +
        std::to_string(server->options.terminate_delay) + "|" +
        BoolStr(server->options.no_touch_pointer_emulation) + "|" +
        BoolStr(server->options.force_xrandr_emulation) + "|" +
        BoolStr(server->options.enable_wm) + "|" +
        (xdg.empty() ? "" : xdg + "/xwayland_stderr.log") + "|" +
        std::to_string(getpid()) + "|" + xdg;
    for (size_t i = 0; argv[i]; ++i)
        params += std::string("|") + argv[i];

    // 命名 fd 五连; 成功后所有权转移给 NCP 框架, 不得再 close
    NativeChildProcess_Fd nodes[5] = {};
    int n = 0;
    nodes[n].fdName = const_cast<char*>("x_fd0");
    nodes[n++].fd = server->x_fd[0];
    nodes[n].fdName = const_cast<char*>("x_fd1");
    nodes[n++].fd = server->x_fd[1];
    nodes[n].fdName = const_cast<char*>("wl_fd");
    nodes[n++].fd = server->wl_fd[1];
    if (server->options.enable_wm)
    {
        nodes[n].fdName = const_cast<char*>("wm_fd");
        nodes[n++].fd = server->wm_fd[1];
    }
    nodes[n].fdName = const_cast<char*>("displayfd");
    nodes[n++].fd = display_fd;
    for (int i = 0; i + 1 < n; ++i) nodes[i].next = &nodes[i + 1];

    NativeChildProcess_FdList fdList = {};
    fdList.head = &nodes[0];
    NativeChildProcess_Args args = {};
    args.entryParams = strdup(params.c_str());
    args.fdList = fdList;
    NativeChildProcess_Options options = {};
    options.isolationMode = NCP_ISOLATION_MODE_NORMAL;

    int32_t pid = -1;
    int32_t ret = OH_Ability_StartNativeChildProcess(
        const_cast<char*>("libxwayland_child.so:Main"), args, options, &pid);
    if (ret != 0)
    {
        OH_LOG_ERROR(LOG_APP, "StartNativeChildProcess ret=%{public}d", ret);
        return false;
    }
    OH_LOG_INFO(LOG_APP, "NCP spawned pid=%{public}d params=%{public}s", pid, params.c_str());
    return true;
}

// ── smoke 调试入口 ─────────────────────────────────────────────────────
extern "C" void WineHua_DisplayRoute_Start()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_started)
    {
        OH_LOG_INFO(LOG_APP, "already started");
        return;
    }
    g_started = true;
    g_stop = false;

    std::thread([] {
        // 日志桥先装: 后续 wlroots/协议层错误必须可见 (app stderr 不可观测)
        wlr_log_init(WLR_INFO, WlrLogBridge);
        wl_log_set_handler_server(WlServerLogBridge);

        // XDG_RUNTIME_DIR: 优先复用既有合成器设置的目录, 否则自建
        if (!getenv("XDG_RUNTIME_DIR"))
        {
            const char* base = "/data/storage/el2/base/files";
            std::string dir = std::string(base) + "/.x11-rt";
            mkdir(dir.c_str(), 0700);
            setenv("XDG_RUNTIME_DIR", dir.c_str(), 1);
        }

        struct wl_display *wl = wl_display_create();
        if (!wl)
        {
            OH_LOG_ERROR(LOG_APP, "wl_display_create failed");
            return;
        }
        struct wl_event_loop *loop = wl_display_get_event_loop(wl);

        struct wlr_renderer *renderer = wlr_pixman_renderer_create();
        if (!renderer)
        {
            OH_LOG_ERROR(LOG_APP, "pixman renderer create failed");
            return;
        }
        struct wlr_compositor *compositor = wlr_compositor_create(wl, 5, renderer);
        if (!compositor)
        {
            OH_LOG_ERROR(LOG_APP, "compositor create failed");
            return;
        }

        struct wlr_backend *backend = wlr_headless_backend_create(loop);
        if (!backend || !wlr_backend_start(backend))
        {
            OH_LOG_ERROR(LOG_APP, "headless backend failed");
            return;
        }

        // 二段式建 Xwayland (绕开 wlr_xwayland 结构的 `class` 成员, C++ 不可见):
        // 先 server (发起 NCP 启动), 挂 ready 监听, 再拼 XWM
        struct wlr_xwayland_server_options options = {};
        options.lazy = false;
        options.enable_wm = true;
        struct wlr_xwayland_server *server =
            wlr_xwayland_server_create(wl, &options);
        if (!server)
        {
            OH_LOG_ERROR(LOG_APP, "wlr_xwayland_server_create failed");
            return;
        }
        OH_LOG_INFO(LOG_APP, "server created, XDG_RUNTIME_DIR=%{public}s",
                    getenv("XDG_RUNTIME_DIR") ? getenv("XDG_RUNTIME_DIR") : "(unset)");
        g_xwayland_ready_listener.notify = HandleXwaylandReady;
        wl_signal_add(&server->events.ready, &g_xwayland_ready_listener);

        struct wlr_xwayland *xwayland =
            wlr_xwayland_create_with_server(wl, compositor, server);
        if (!xwayland)
        {
            OH_LOG_ERROR(LOG_APP, "wlr_xwayland_create_with_server failed");
            return;
        }

        OH_LOG_INFO(LOG_APP, "started, dispatching event loop");
        while (!g_stop)
        {
            // 宿主循环义务: libwayland 的回包只入队 (want_flush), flush 由
            // 循环宿主负责——wl_display_run = flush_clients + dispatch
            // (wayland-server.c:1738-1744)。漏掉则 Xwayland 的 displayfd/
            // roundtrip 回包永远滞留 out 缓冲, 子进程卡死在首次同步
            // (T7 bring-up 实测: 20s 内回包 0 到达)。
            wl_display_flush_clients(wl);
            wl_event_loop_dispatch(loop, -1);
            wl_display_flush_clients(wl);
        }
        OH_LOG_INFO(LOG_APP, "event loop exit");
    }).detach();
}
