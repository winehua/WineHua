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
#include <fcntl.h>
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
#include <wlr/render/allocator.h>
#include <wlr/interfaces/wlr_buffer.h>
struct wlr_renderer;
struct wlr_compositor;
struct wlr_backend;
struct wlr_xwayland;
struct wlr_output;
struct wlr_renderer *wlr_pixman_renderer_create(void);
// render/wlr_renderer.h:91 (头 C++ 不安全, 手写原型, 签名逐一核对)
bool wlr_renderer_init_wl_display(struct wlr_renderer *r,
	struct wl_display *wl_display);
struct wlr_compositor *wlr_compositor_create(struct wl_display *display,
	uint32_t version, struct wlr_renderer *renderer);
struct wlr_backend *wlr_headless_backend_create(struct wl_event_loop *loop);
bool wlr_backend_start(struct wlr_backend *backend);
// WineHua 补丁新增 (scripts/patches/wlroots-ohos-ncp-spawn.patch)
struct wlr_xwayland *wlr_xwayland_create_with_server(struct wl_display *display,
	struct wlr_compositor *compositor, struct wlr_xwayland_server *server);
bool wlr_xwayland_server_ohos_build_argv(struct wlr_xwayland_server *server,
	int notify_fd, char *argv[], size_t argv_max);
}

#include <AbilityKit/native_child_process.h>

#include "ohos_output.h"
#include "display_input.h"

extern "C" {
#include <native_buffer/native_buffer.h>
#include <native_window/external_window.h>
#include <native_window/buffer_handle.h>
}

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

    // T9 M0 出口: Xwayland 就绪 (XWM 已建) 即拉起 mini X client (NCP,
    // libX11+libXext ONLY)。连接由 Xlib 发起 (无命名 fd) —— 这正是 M0 出口
    // 要验证的事。entryParams: "<stderrPath>|<xdgDir>|<mode>"; T2 起
    // mode=2 (双窗口 + 周期移动, 与 display_input.c 注入脚本坐标成对)
    std::string xdg = getenv("XDG_RUNTIME_DIR") ? getenv("XDG_RUNTIME_DIR") : "";
    std::string params = (xdg.empty() ? "" : xdg + "/xclient_stderr.log") + "|" + xdg + "|2";
    NativeChildProcess_Args args = {};
    args.entryParams = strdup(params.c_str());
    NativeChildProcess_Options options = {};
    options.isolationMode = NCP_ISOLATION_MODE_NORMAL;
    int32_t pid = -1;
    int32_t ret = OH_Ability_StartNativeChildProcess(
        const_cast<char*>("libxclient_child.so:Main"), args, options, &pid);
    if (ret != 0)
        OH_LOG_ERROR(LOG_APP, "xclient StartNativeChildProcess ret=%{public}d", ret);
    else
        OH_LOG_INFO(LOG_APP, "xclient NCP spawned pid=%{public}d", pid);
}
struct wl_listener g_xwayland_ready_listener;

// ── T8 出图链: 测试图案 → OH_NativeBuffer → NativeWindow 直推 ─────────────
// ArkTS 侧经 XComponent surfaceId 创建的 NativeWindow; smoke 面板持有 surface
OHNativeWindow *g_present_window = nullptr;
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
extern "C" void WineHua_DisplayRoute_StartWithSurface(uint64_t surface_id,
                                                      bool script_enabled);

// 重复触发刷新通道: 刷新动作必须落在 loop 线程 (定时器/事件源操作非线程
// 安全, M1-T5 实测: 第二轮 smoke 复用既有链时 marker 不重写、注入脚本不
// 重挂, 编排整体落空)。loop 线程建 pipe 事件源, 触发侧只 write 一个字节
// (write 线程安全), loop 线程收到后执行刷新。
static int g_retrigger_pipe[2] = {-1, -1};

static int DisplayRouteRetriggerWake(int fd, uint32_t mask, void *data)
{
    (void)mask;
    (void)data;
    char b;
    while (read(fd, &b, 1) == 1)
    {
    }
    FILE *f = fopen("/data/storage/el2/base/files/.wine/drive_c/displayroute-ready", "w");
    if (f)
    {
        fputs("ready\n", f);
        fclose(f);
    }
    wl_ohos_input_script_restart();
    OH_LOG_INFO(LOG_APP, "displayroute retrigger: marker rewritten, script re-armed");
    return 0;
}

extern "C" void WineHua_DisplayRoute_Start()
{
    WineHua_DisplayRoute_StartWithSurface(0, false);
}

// surfaceId 非零时 (ArkTS XComponent) 建 NativeWindow, T8 出图链随之启动;
// 为 0 时维持 T7 行为 (仅合成器内核 + Xwayland, 不建 output)。
// script_enabled = 真机门自动注入脚本 (smoke 验证编排, 默认关 —— 测试资产
// 不默认进产品行为, 原则 #23)。
extern "C" void WineHua_DisplayRoute_StartWithSurface(uint64_t surface_id,
                                                      bool script_enabled)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_started)
    {
        OH_LOG_INFO(LOG_APP, "already started, refresh for retrigger");
        if (g_retrigger_pipe[1] >= 0)
        {
            char b = 'r';
            ssize_t rc = write(g_retrigger_pipe[1], &b, 1);
            (void)rc;
        }
        return;
    }
    g_started = true;
    g_stop = false;
    // 脚本门在 seat_create (子线程) 之前定值: 开启态决定注入编排定时器
    // 是否武装。每轮显示路线触发都经这里, retrigger 路径同样先过此门。
    wl_ohos_input_set_script_enabled(script_enabled);

    if (surface_id != 0)
    {
        int32_t rc = OH_NativeWindow_CreateNativeWindowFromSurfaceId(
            surface_id, &g_present_window);
        OH_LOG_INFO(LOG_APP, "present window from surface %{public}llu rc=%{public}d",
                    (unsigned long long)surface_id, rc);
        if (rc != 0)
            g_present_window = nullptr;
    }

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
        // 重复触发刷新通道 (pipe 事件源, 触发侧仅 write)。两端必须
        // O_NONBLOCK: wake 处理器的排空循环 read 到管道空时会一直阻塞
        // (pipe() 默认阻塞语义, libwayland 不改 added fd 的标志), loop
        // 线程卡死 = 整个合成器冻结 (T5 t5k 实测: 触发后帧时钟/Xwayland
        // 全停, 后续 X client 连接握手挂死)。
        if (pipe(g_retrigger_pipe) == 0)
        {
            for (int i = 0; i < 2; ++i)
                fcntl(g_retrigger_pipe[i], F_SETFL, O_NONBLOCK);
            wl_event_loop_add_fd(loop, g_retrigger_pipe[0], WL_EVENT_READABLE,
                                 DisplayRouteRetriggerWake, nullptr);
        }
        else
        {
            OH_LOG_ERROR(LOG_APP, "retrigger pipe create failed errno=%{public}d", errno);
        }

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
        // M1-T1 输入链: seat + 虚拟键盘。须在 Xwayland server create 之前
        // (seat global 先于 Xwayland 客户端连接存在); set_seat 在下方
        // create_with_server 之后 (xwm 同步建立, M0 实证)
        if (wl_ohos_input_seat_create(wl, loop) != 0)
        {
            OH_LOG_ERROR(LOG_APP, "input seat create failed");
            return;
        }
        // wl_shm/wl_drm 全局: wlr_compositor_create 不建, 必须显式初始化。
        // 缺它 Xwayland 的 registry 无 wl_shm → 首个窗口 Map 时
        // xwl_shm_create_pixmap 解引用 NULL proxy 段错误 (T9 真机 cppcrash
        // 20260928213732: MapWindow→compNewPixmap→wl_shm_create_pool(NULL))
        if (!wlr_renderer_init_wl_display(renderer, wl))
        {
            OH_LOG_ERROR(LOG_APP, "renderer_init_wl_display failed (无 shm 全局)");
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
        // M1-T1: xwm 已同步建立 (M0 实证), 接 seat——"no seat assigned to
        // xwayland" 告警自此消失, 键盘/指针经 wl_seat 进 Xwayland
        wl_ohos_input_xwayland_set_seat(xwayland);

        // ── T8 出图链: 自定义 allocator + headless output + 帧直推
        //    (实现整体在 ohos_output.c, wlr_output.h 的 C++ 不兼容见其头注释);
        //    T9: 带 xwayland, 已映射 X client 窗口优先合成上屏
        if (g_present_window)
        {
            int rc = wl_ohos_output_chain_start(backend, renderer, loop, wl,
                                                g_present_window, xwayland);
            OH_LOG_INFO(LOG_APP, "output chain start rc=%{public}d", rc);
        }
        else
        {
            OH_LOG_INFO(LOG_APP, "no present surface, T8 output chain skipped");
        }

        // M1-T4: 就绪标记 —— X socket/xwm/出图链全部就位。smoke 编排
        // (winemine/notepad 的 X 档位 spawn) 以该文件出现为同步判据;
        // 旧标记由触发方在 bring-up 前删除。
        {
            FILE *f = fopen("/data/storage/el2/base/files/.wine/drive_c/displayroute-ready", "w");
            if (f)
            {
                fputs("ready\n", f);
                fclose(f);
                OH_LOG_INFO(LOG_APP, "displayroute-ready marker written");
            }
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
