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
// 销毁族 (M2-T1 启动失败统一收尾): 签名逐一核对 wlroots 头
// (render/wlr_renderer.h, backend.h, xwayland/xwayland.h —— 后者 C++
// 不安全, 只手写销毁原型)。注: 0.20.2 无 wlr_compositor_destroy
// (types/wlr_compositor.c 无此函数), compositor global 随
// wl_display_destroy 撤销。
void wlr_renderer_destroy(struct wlr_renderer *r);
void wlr_backend_destroy(struct wlr_backend *backend);
void wlr_xwayland_destroy(struct wlr_xwayland *wlr_xwayland);
// WineHua 补丁新增 (scripts/patches/wlroots-ohos-ncp-spawn.patch)
struct wlr_xwayland *wlr_xwayland_create_with_server(struct wl_display *display,
	struct wlr_compositor *compositor, struct wlr_xwayland_server *server);
bool wlr_xwayland_server_ohos_build_argv(struct wlr_xwayland_server *server,
	int notify_fd, char *argv[], size_t argv_max);
}

#include <AbilityKit/native_child_process.h>

#include "ohos_output.h"
#include "display_input.h"
#include "ohos_egl_import_probe.h"
#include "ohos_egl_import.h"

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
// gles2 是否已激活 (渲染器选择结果; 出图链延后启动时也要用, 故提到文件作用域)
static bool g_gles2_active = false;

// M2-T4: present 前置探针的定时器回调 —— 排在事件循环启动后 1s, 即晚于
// Xwayland/wine 这些 NCP 子进程的 fork (理由与实测见调用点)
static int PresentProbeTimer(void *data)
{
    (void)data;
    bool ok = wl_ohos_egl_present_probe(
        g_present_window,
        "/data/storage/el2/base/files/.wine/drive_c/displayroute-present-probe");
    OH_LOG_INFO(LOG_APP, "present probe done ok=%{public}d", ok ? 1 : 0);
    return 0; /* 一次性 */
}

/* M2-T4 次序约束 (真机实测): Xwayland 是 NCP 子进程 (fork 本进程), 而 gles2
 * 出图链的 EGL 导入/GL 渲染必须在**它 fork 之后**才开始 —— 反过来 (同步段里
 * 先做导入再 fork) 实测 Xwayland 在早期初始化处挂死, displayfd 握手永不完成,
 * 没有 X 客户端, 合成器 surfaces 恒 0。对照实验 (2026-09-30 设备 .5):
 *   pixman 渲染器 + 出图链照常 (无 GL 导入) → Xwayland 正常, X 客户端出图;
 *   gles2 渲染器 + 出图链 → Xwayland 挂死; 只建 gles2 渲染器不出图 → 正常。
 * 出图链因此延后到事件循环第一拍 (定时器晚于 Xwayland 的 spawn idle):
 * 先 fork 子进程, 再做 GL。 */
struct DeferredOutputChainStart {
    struct wlr_backend *backend;
    struct wlr_renderer *renderer;
    struct wl_event_loop *loop;
    struct wl_display *display;
    OHNativeWindow *window;
    struct wlr_xwayland *xwayland;
};
static struct DeferredOutputChainStart g_deferred_chain;

static void WriteDisplayRouteReady(void)
{
    FILE *f = fopen("/data/storage/el2/base/files/.wine/drive_c/displayroute-ready", "w");
    if (f)
    {
        fputs("ready\n", f);
        fclose(f);
        OH_LOG_INFO(LOG_APP, "displayroute-ready marker written");
    }
}

static int StartOutputChainTimer(void *data)
{
    struct DeferredOutputChainStart *c = (struct DeferredOutputChainStart *)data;
    int rc = wl_ohos_output_chain_start(c->backend, c->renderer, c->loop,
                                        c->display, c->window, c->xwayland);
    OH_LOG_INFO(LOG_APP, "output chain start rc=%{public}d (deferred)", rc);
    WriteDisplayRouteReady();

    // present 零拷贝前置探针: 再往后 1s (同样只为避开子进程 fork 窗口)
    const char *probe_env = getenv("WINEHUA_COMPOSITOR_PROBE");
    if (rc == 0 && g_gles2_active && c->window &&
        (probe_env == nullptr || strcmp(probe_env, "0") != 0))
    {
        struct wl_event_source *probe_timer =
            wl_event_loop_add_timer(c->loop, PresentProbeTimer, nullptr);
        if (probe_timer)
        {
            wl_event_source_timer_update(probe_timer, 1000);
            OH_LOG_INFO(LOG_APP, "present probe timer armed (+1000ms)");
        }
        else
        {
            OH_LOG_ERROR(LOG_APP, "present probe timer 建不起来");
        }
    }
    return 0; /* 一次性 */
}
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
            /* EAGAIN = 唤醒字节已在途 (管道满), 无害; 其余失败记日志
             * (M2-T1: 静默丢触发排查代价过高) */
            if (rc < 0 && errno != EAGAIN)
                OH_LOG_ERROR(LOG_APP, "retrigger write failed errno=%{public}d", errno);
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
        // 对象提升到函数顶部 (M2-T1): 失败路径统一 goto fail 收尾, 逆序
        // 销毁 —— 之前中途 return 泄漏已建对象 (known-issues §1.2), 且
        // g_started 不复位导致失败后无法重试。
        struct wl_display *wl = nullptr;
        struct wl_event_loop *loop = nullptr;
        struct wlr_renderer *renderer = nullptr;
        struct wlr_compositor *compositor = nullptr;
        struct wlr_backend *backend = nullptr;
        struct wlr_xwayland_server *server = nullptr;
        struct wlr_xwayland *xwayland = nullptr;
        // 渲染器选择 (M2-T4) 的判据也提升到顶: goto fail 不可跨带初始化的
        // 声明 (C++ 规则, T1 同款坑)
        bool gles2_active = false;
        const char *renderer_env = nullptr;

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

        wl = wl_display_create();
        if (!wl)
        {
            OH_LOG_ERROR(LOG_APP, "wl_display_create failed");
            goto fail;
        }
        loop = wl_display_get_event_loop(wl);
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

        // M2-T2: R-ZC ② 探针 —— 标记文件不存在时真跑一次 host EGL OHOS
        // 导入链, 结论落盘 (drive_c/displayroute-egl-import-probe), gles2
        // 零拷贝路线 (T4) 按它裁决。自缓存, 一次性几 ms。
        ohos_egl_import_probe_run();

        // M2-T4: 渲染器选择 —— 默认 gles2 (T2 探针裁决 EGL_NATIVE_BUFFER_OHOS
        // 导入链可用, 零拷贝据此成立), 建不起来自动回退 pixman;
        // WINEHUA_COMPOSITOR_RENDERER=pixman 强制旧路 (排障/对照用)。
        renderer_env = getenv("WINEHUA_COMPOSITOR_RENDERER");
        if (renderer_env == nullptr || strcmp(renderer_env, "pixman") != 0)
        {
            renderer = wl_ohos_egl_renderer_create();
            gles2_active = renderer != nullptr;
            g_gles2_active = gles2_active;
            if (!renderer)
                OH_LOG_ERROR(LOG_APP, "gles2 renderer 不可用, 回退 pixman");
        }
        if (!renderer)
        {
            renderer = wlr_pixman_renderer_create();
            if (!renderer)
            {
                OH_LOG_ERROR(LOG_APP, "pixman renderer create failed");
                goto fail;
            }
        }
        OH_LOG_INFO(LOG_APP, "compositor renderer=%{public}s",
                    gles2_active ? "gles2" : "pixman");
        compositor = wlr_compositor_create(wl, 5, renderer);
        if (!compositor)
        {
            OH_LOG_ERROR(LOG_APP, "compositor create failed");
            goto fail;
        }
        // M1-T1 输入链: seat + 虚拟键盘。须在 Xwayland server create 之前
        // (seat global 先于 Xwayland 客户端连接存在); set_seat 在下方
        // create_with_server 之后 (xwm 同步建立, M0 实证)
        if (wl_ohos_input_seat_create(wl, loop) != 0)
        {
            OH_LOG_ERROR(LOG_APP, "input seat create failed");
            goto fail;
        }
        // wl_shm/wl_drm 全局: wlr_compositor_create 不建, 必须显式初始化。
        // 缺它 Xwayland 的 registry 无 wl_shm → 首个窗口 Map 时
        // xwl_shm_create_pixmap 解引用 NULL proxy 段错误 (T9 真机 cppcrash
        // 20260928213732: MapWindow→compNewPixmap→wl_shm_create_pool(NULL))
        if (!wlr_renderer_init_wl_display(renderer, wl))
        {
            OH_LOG_ERROR(LOG_APP, "renderer_init_wl_display failed (无 shm 全局)");
            goto fail;
        }

        backend = wlr_headless_backend_create(loop);
        if (!backend || !wlr_backend_start(backend))
        {
            OH_LOG_ERROR(LOG_APP, "headless backend failed");
            goto fail;
        }

        // 二段式建 Xwayland (绕开 wlr_xwayland 结构的 `class` 成员, C++ 不可见):
        // 先 server (发起 NCP 启动), 挂 ready 监听, 再拼 XWM
        {
            struct wlr_xwayland_server_options options = {};
            options.lazy = false;
            options.enable_wm = true;
            server = wlr_xwayland_server_create(wl, &options);
        }
        if (!server)
        {
            OH_LOG_ERROR(LOG_APP, "wlr_xwayland_server_create failed");
            goto fail;
        }
        OH_LOG_INFO(LOG_APP, "server created, XDG_RUNTIME_DIR=%{public}s",
                    getenv("XDG_RUNTIME_DIR") ? getenv("XDG_RUNTIME_DIR") : "(unset)");
        g_xwayland_ready_listener.notify = HandleXwaylandReady;
        wl_signal_add(&server->events.ready, &g_xwayland_ready_listener);

        xwayland = wlr_xwayland_create_with_server(wl, compositor, server);
        if (!xwayland)
        {
            OH_LOG_ERROR(LOG_APP, "wlr_xwayland_create_with_server failed");
            goto fail;
        }
        // M1-T1: xwm 已同步建立 (M0 实证), 接 seat——"no seat assigned to
        // xwayland" 告警自此消失, 键盘/指针经 wl_seat 进 Xwayland
        wl_ohos_input_xwayland_set_seat(xwayland);

        // ── T8 出图链: 自定义 allocator + headless output + 帧直推
        //    (实现整体在 ohos_output.c, wlr_output.h 的 C++ 不兼容见其头注释);
        //    T9: 带 xwayland, 已映射 X client 窗口优先合成上屏
        if (g_present_window)
        {
            /* 出图链延后到事件循环第一拍 (定时器晚于 Xwayland 的 spawn idle):
             * 先 fork 子进程, 再做 GL —— 次序约束与实测见 DeferredOutputChainStart
             * 上方注释。就绪标记也在那一刻才写 (标记语义 = 出图链已就位)。 */
            g_deferred_chain = (struct DeferredOutputChainStart){
                backend, renderer, loop, wl, g_present_window, xwayland};
            struct wl_event_source *chain_timer =
                wl_event_loop_add_timer(loop, StartOutputChainTimer, &g_deferred_chain);
            if (chain_timer)
                wl_event_source_timer_update(chain_timer, 1);
            else
                OH_LOG_ERROR(LOG_APP, "output chain timer 建不起来 (出图链不会启动)");
        }
        else
        {
            OH_LOG_INFO(LOG_APP, "no present surface, T8 output chain skipped");
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

    fail:
        // 统一收尾 (M2-T1, known-issues §1.2): 正常退出与启动失败共用。
        // 逆序销毁; with_server 形态的 server 不归 wlr_xwayland_destroy 管
        // (own_server=false, xwayland.c:91), 须显式销毁。
        if (xwayland)
            wlr_xwayland_destroy(xwayland);
        if (server)
        {
            wl_list_remove(&g_xwayland_ready_listener.link);
            wlr_xwayland_server_destroy(server);
        }
        if (backend)
            wlr_backend_destroy(backend);
        /* compositor 无 destroy API (0.20.2): global 随下方
         * wl_display_destroy 撤销, 无需也无法单独销毁 */
        if (renderer)
            wlr_renderer_destroy(renderer);
        if (g_retrigger_pipe[0] >= 0)
        {
            close(g_retrigger_pipe[0]);
            close(g_retrigger_pipe[1]);
            g_retrigger_pipe[0] = g_retrigger_pipe[1] = -1;
        }
        if (wl)
            wl_display_destroy(wl);
        {
            // 状态复位 (锁内): 失败/停止后允许下次触发重新走完整 bring-up
            std::lock_guard<std::mutex> lock(g_mutex);
            if (g_present_window)
            {
                OH_NativeWindow_DestroyNativeWindow(g_present_window);
                g_present_window = nullptr;
            }
            g_started = false;
            g_stop = false;
        }
        OH_LOG_INFO(LOG_APP, "displayroute stopped or failed; cleaned up (retry possible)");
    }).detach();
}
