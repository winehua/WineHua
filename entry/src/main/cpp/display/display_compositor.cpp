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
#include "ohos_buffer.h" /* wl_ohos_present_slots_shutdown (停机回收 present slot) */
#include "display_input.h"
#include "display_guest_frames.h"
#include "compositor/wayland_server.h" /* WaylandServer session (无画布早启的输出尺寸源) */
#include "ohos_egl_import_probe.h"
#include "xim_bridge.h"
#include "ohos_egl_import.h"

#include "compositor/wayland_server.h" /* FireDesktopReady 经会话状态通道补发 */

// x11 路线的 evt:desktop-ready 补发 (M3a): 桌面根 toplevel 是 wayland 私有
// 协议概念, X 路线没有该事件 —— LaunchPadMode 超时发 state:ready-degraded
// 后, ArkTS 状态机靠 evt:desktop-ready 升级为正式 ready, 没有它 engineState
// 永停 degraded, smoke 跑测门永不过 (实测 2026-10-04)。触发点在 ohos_output
// 的 ClientMapRequest (首个 client XMapWindow = 桌面 shell 就绪的 X 等价
// 信号); 经 WaylandServer 单例的会话状态通道发同一消息 —— stateCb 是引擎
// 消息通道 (napi_init 注册, 与路线无关), wayland 合成器本体未运行也照达。
extern "C" void WineHua_DisplayRoute_FireDesktopReady()
{
    WaylandServer::GetInstance()->FireState("evt:desktop-ready");
}

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
// xclient 测试客户端门控 (M3a): mode=2 双窗口+周期移动是 M0-T9/M1-T2 的
// bring-up 测试资产 (与 display_input 注入脚本坐标成对), 只应在 smoke 编排
// (script_enabled=true) 拉起。无条件 spawn 让产品桌面里常驻一个 320x240
// 测试窗在桌面上晃 —— 真实桌面 shell (explorer, 引擎会话拉起) 的输入命中
// 也被它搅局 (2026-10-04 实测: 点击命中测试只能看到测试窗)。原则 #23。
static bool g_xclient_test_client_enabled;
static bool g_xwayland_ready_seen;
static bool g_xclient_spawned;

/* 拉起条件三合一: 门开 (最新 script 意图) + Xwayland 就绪 + 未拉过。
 * 从两个时机调用: Xwayland ready (常规路径) / retrigger 刷新 (补拉) ——
 * 竞态下 retrigger 可能晚于 ready (2026-10-04 实测: 产品链先建 → ready
 * 时门还关着 → smoke 触发补开门时 ready 事件已过, 不补拉则 smoke 套件
 * 永远等不到测试窗)。条件不满足时静默返回 (后续时机再试)。 */
static void TrySpawnXclientTestClient();

void HandleXwaylandReady(struct wl_listener *listener, void *data)
{
    OH_LOG_INFO(LOG_APP, "Xwayland ready signal received (displayfd handshake OK)");
    g_xwayland_ready_seen = true;
    if (!g_xclient_test_client_enabled)
        OH_LOG_INFO(LOG_APP, "xclient test client skipped (script disabled, product desktop)");
    TrySpawnXclientTestClient();
}

static void TrySpawnXclientTestClient()
{
    if (g_xclient_spawned)
        return;
    if (!g_xclient_test_client_enabled || !g_xwayland_ready_seen)
        return;
    g_xclient_spawned = true;
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
        /* files 根 (D10b): 与 egl-import 探针同款迁移, 理由见
         * ohos_egl_import_probe.c kMarkerPath 注释 —— drive_c 是 wine
         * prefix, fresh install 时不存在 */
        "/data/storage/el2/base/files/displayroute-present-probe");
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
    int out_w; /* ≤0 = 未指定 ⇒ 800x600 (smoke 台架口径) */
    int out_h;
    bool canvas_egl_present; /* 画布绑定 = EGL swap 呈现 (见 ohos_output.h) */
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
                                        c->display, c->window, c->xwayland,
                                        c->out_w, c->out_h,
                                        c->canvas_egl_present);
    OH_LOG_INFO(LOG_APP, "output chain start rc=%{public}d (deferred)", rc);
    WriteDisplayRouteReady();

    /* XIM 桥探针 (XIM spec Task 1) 已完成使命并撤调用: 主进程 XOpenDisplay
     * 实测挂起不返回 (2026-10-07, 探针线程化后无害但不撤则每次冷启泄漏一个
     * 挂起线程)。X1 结论 = 主进程 X 不可达, 桥承载改 NCP 子进程 (与
     * Xwayland 同 mount namespace, xclient_child 已证连接可行)——spec §3
     * 修正后 xim_bridge 文件按新架构重写。 */

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

    // 引擎包就绪门 (D8 冷启竞争, 2026-10-05 实测): want 时刻早启链后,
    // Xwayland 的 -xkbdir 指向引擎解包产物 (wine-data.zip), 全新安装首次
    // 启动时解包尚未发生 → XKB rules 缺失 → Xwayland "Failed to activate
    // virtual core keyboard" SIGABRT (xwayland_stderr.log 实证), 桌面黑屏
    // + root 恒超时。
    // 等待无超时 (23:33 实测教训: 引擎初始化可由用户交互/重试触发 ——
    // 带参冷启 failInit 后用户手动初始化, 包在任意时刻就绪; 30s 有界等待
    // 会把「晚到」误判成「不到」)。Xwayland 只能在包就绪后出生, 门等到位
    // 为止, 5s 心跳留证。阻塞发生在 compositor 线程 loop 启动前, 无并发
    // 消费者, 安全。
    for (size_t i = 0; argv[i]; ++i)
    {
        if (strcmp(argv[i], "-xkbdir") != 0 || !argv[i + 1])
            continue;
        char rules[512];
        snprintf(rules, sizeof(rules), "%s/rules/evdev", argv[i + 1]);
        struct stat st;
        int waited_ms = 0;
        bool announced = false;
        while (stat(rules, &st) != 0)
        {
            if (!announced)
            {
                OH_LOG_WARN(LOG_APP,
                            "engine payload not unpacked yet (%{public}s missing), waiting indefinitely",
                            rules);
                announced = true;
            }
            usleep(100 * 1000);
            waited_ms += 100;
            if (waited_ms % 5000 == 0)
                OH_LOG_WARN(LOG_APP, "engine payload wait: %{public}d s", waited_ms / 1000);
        }
        if (waited_ms > 0)
            OH_LOG_INFO(LOG_APP, "engine payload ready after %{public}d ms", waited_ms);
        break;
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
                                                      bool script_enabled,
                                                      int out_w, int out_h,
                                                      bool canvas_egl_present);

// 重复触发刷新通道: 刷新动作必须落在 loop 线程 (定时器/事件源操作非线程
// 安全, M1-T5 实测: 第二轮 smoke 复用既有链时 marker 不重写、注入脚本不
// 重挂, 编排整体落空)。loop 线程建 pipe 事件源, 触发侧只 write 一个字节
// (write 线程安全), loop 线程收到后执行刷新。
static int g_retrigger_pipe[2] = {-1, -1};

// 画布晚到交接槽 (D8, 2026-10-05): 链先建 (无画布, output 仍在场) 后,
// 画布 surface 才到达 —— 产品路径 want 时起链, DWA 画布页 engine-ready 才
// 开。触发侧 (任意线程) 把 NativeWindow 放进槽再写 retrigger 字节; loop
// 线程的 retrigger 处理取走并挂到既有 output 上 (present 从挂起转活跃)。
// 独立互斥锁 (不碰 g_mutex): wake 处理器跑在 loop 线程, 触发侧持 g_mutex
// 写 pipe, 两锁若相同会互相等。
static std::mutex g_late_window_mutex;
static OHNativeWindow *g_late_window = nullptr;
static bool g_late_canvas_egl = false; /* 挂载时补跑画布 EGL swap 建面 (M3a-T7) */

// 画布 resize 交接槽 (D10): 折叠/旋转使画布尺寸变化 (实测 2800x1840 ↔
// 1840x2800), ArkTS onSurfaceChanged 把逻辑尺寸投进槽 + retrigger 唤醒,
// loop 线程消费 (output/背景/buffer geometry 同步)。w>0 = 待处理。
static std::mutex g_resize_mutex;
static int g_resize_w = 0;
static int g_resize_h = 0;

static int DisplayRouteRetriggerWake(int fd, uint32_t mask, void *data)
{
    (void)mask;
    /* data = 本链的 wl_event_loop (注册处传入): 晚到画布挂载后在这里武装
     * present 探针定时器, 没有别的 loop 取数口 (本进程可能同时存在多条
     * wayland 链, 环境变量取 loop 不可行)。 */
    struct wl_event_loop *loop = (struct wl_event_loop *)data;
    char b;
    while (read(fd, &b, 1) == 1)
    {
        /* 协议字节: 's' = 停机 (M3a: 桌面 surface 销毁 → 主循环退出 → 走
         * 循环后统一收尾段)。其它字节 = retrigger (历史语义, 触发方只写
         * 任意非 's' 字节)。 */
        if (b == 's')
            g_stop = true;
    }
    if (g_stop)
        return 0; /* 主循环 while(!g_stop) 退出, 收尾在循环外 */
    FILE *f = fopen("/data/storage/el2/base/files/.wine/drive_c/displayroute-ready", "w");
    if (f)
    {
        fputs("ready\n", f);
        fclose(f);
    }
    wl_ohos_input_script_restart();
    /* script 是否真的 re-armed 由 restart 自己打点 (定时器缺建时不再无声) */
    OH_LOG_INFO(LOG_APP, "displayroute retrigger: marker rewritten");
    /* 画布晚到挂载 (D8): 槽里有窗口 = 本次重触发带着出画面任务。链已建
     * 而 output 无窗时挂上即开始 present; output 已有窗 (smoke 画布先行)
     * 时拒绝并销毁新窗 —— surface 重建走 stop/start 全链 (见 StartWithSurface
     * 重触发注释), 不存在运行中换窗的合法场景。 */
    OHNativeWindow *late = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_late_window_mutex);
        late = g_late_window;
        g_late_window = nullptr;
    }
    if (late)
    {
        bool egl_flag;
        {
            std::lock_guard<std::mutex> lk(g_late_window_mutex);
            egl_flag = g_late_canvas_egl;
            g_late_canvas_egl = false;
        }
        if (wl_ohos_output_attach_window(late, egl_flag) != 0)
        {
            OH_LOG_WARN(LOG_APP, "late canvas attach refused, destroying window");
            OH_NativeWindow_DestroyNativeWindow(late);
        }
        else
        {
            /* present 探针 (M2-T4) 随画布挂载武装 (2026-10-06 修): 原武装点
             * 在 chain_start 定时器, 条件 c->window 在 output/画布解耦
             * (67f287b) 后于产品路径恒 NULL —— 探针永不跑, 归档缺
             * displayroute-present-probe 标记, suite 判 FAIL
             * (job-r20261006-001311 实测; 211218 PASS 是解耦前的旧口径)。
             * attach 成功时刻 Xwayland 早已 fork (画布晚于 explorer 60s+),
             * 原「先 fork 子进程再 GL」次序约束天然满足; 1s 延迟保留, 避开
             * attach 当拍的 present 建立窗。
             * g_present_window 同步记名: 收尾段只销毁它 (槽位/EGL 面先收),
             * 晚到画布此前在 stop 时无人销毁 —— 顺带闭合。attach 一次性
             * (拒绝重复挂载), 探针至多武装一次。 */
            g_present_window = late;
            const char *probe_env = getenv("WINEHUA_COMPOSITOR_PROBE");
            if (g_gles2_active &&
                (probe_env == nullptr || strcmp(probe_env, "0") != 0))
            {
                struct wl_event_source *probe_timer =
                    wl_event_loop_add_timer(loop, PresentProbeTimer, nullptr);
                if (probe_timer)
                {
                    wl_event_source_timer_update(probe_timer, 1000);
                    OH_LOG_INFO(LOG_APP, "present probe timer armed on canvas attach (+1000ms)");
                }
                else
                {
                    OH_LOG_ERROR(LOG_APP, "present probe timer 建不起来");
                }
            }
        }
    }
    /* 画布 resize (D10): 旋转/折叠后 onSurfaceChanged 的逻辑尺寸到货 */
    {
        int rw = 0, rh = 0;
        {
            std::lock_guard<std::mutex> lk(g_resize_mutex);
            rw = g_resize_w;
            rh = g_resize_h;
            g_resize_w = 0;
            g_resize_h = 0;
        }
        if (rw > 0 && rh > 0)
            wl_ohos_output_resize(rw, rh);
    }
    /* 门被本次 retrigger 重开时, Xwayland ready 事件可能早已过去
     * (产品链先建 + ready 先到的竞态), 此处补拉测试窗。 */
    TrySpawnXclientTestClient();
    return 0;
}

extern "C" void WineHua_DisplayRoute_Start()
{
    WineHua_DisplayRoute_StartWithSurface(0, false, 0, 0, false);
}

// M3a: 停机入口 (桌面 surface 销毁 → x11 台架回收合成器)。经 retrigger
// pipe 写 's' 唤醒 loop 线程走统一收尾段 (g_started 复位, 可再次启动)。
// 未启动 = no-op。线程安全 (与触发侧同一把锁)。
extern "C" void WineHua_DisplayRoute_Stop()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_started || g_retrigger_pipe[1] < 0)
        return;
    ssize_t rc = write(g_retrigger_pipe[1], "s", 1);
    /* EAGAIN = 已有待处理字节 (含一次 stop), 无害; 其余失败记日志 */
    if (rc < 0 && errno != EAGAIN)
        OH_LOG_ERROR(LOG_APP, "displayroute stop write failed errno=%{public}d", errno);
}

// 画布 resize 入口 (D10): 折叠/旋转后 onSurfaceChanged 的逻辑尺寸。
// 未启动 = no-op (链首启自带尺寸); 已启动 → 槽 + retrigger, loop 线程
// 应用。尺寸无效值静默忽略。
extern "C" void WineHua_DisplayRoute_Resize(int w, int h)
{
    if (w <= 0 || h <= 0)
        return;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_started)
            return;
    }
    {
        std::lock_guard<std::mutex> lk(g_resize_mutex);
        g_resize_w = w;
        g_resize_h = h;
    }
    if (g_retrigger_pipe[1] >= 0)
    {
        char b = 'r';
        ssize_t rc = write(g_retrigger_pipe[1], &b, 1);
        if (rc < 0 && errno != EAGAIN)
            OH_LOG_ERROR(LOG_APP, "resize retrigger write failed errno=%{public}d", errno);
    }
}

// surfaceId 非零时 (ArkTS XComponent) 建 NativeWindow, T8 出图链随之启动;
// 为 0 时维持 T7 行为 (仅合成器内核 + Xwayland, 不建 output)。
// script_enabled = 真机门自动注入脚本 (smoke 验证编排, 默认关 —— 测试资产
// 不默认进产品行为, 原则 #23)。
// canvas_egl_present = 画布 (DesktopAbility 全屏窗) 绑定, present 走
// EGL swap (该 surface 对手工 FlushBuffer 冻结; 语义见 ohos_output.h)。
extern "C" void WineHua_DisplayRoute_StartWithSurface(uint64_t surface_id,
                                                      bool script_enabled,
                                                      int out_w, int out_h,
                                                      bool canvas_egl_present)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_started)
    {
        OH_LOG_INFO(LOG_APP, "already started, refresh for retrigger");
        // 重触发携带本次调用方的 script 意图 (最新意图优先): 冷启时产品桌面页
        // 与 smoke 编排都会调 StartWithSurface, 谁先到谁建链; 若重触发不刷新
        // script 门, 门就永远停在先到者的值 —— 2026-10-04 实测: DesktopWindow
        // 先启 (script=false), 随后 rate job 的 displayroute 触发 (script=true)
        // 落在早退上, xclient 测试窗不再出现, smoke 套件依赖的注入编排失效。
        // 产品桌面运行中不会被重触发 (surface 重建走 stop/start 全链), 不存在
        // "smoke 运行中被产品意图打断"的反向场景。
        wl_ohos_input_set_script_enabled(script_enabled);
        g_xclient_test_client_enabled = script_enabled;
        if (surface_id != 0)
        {
            // 画布晚到 (D8): 链已建而画布后才到 —— 从 surface 建 NativeWindow
            // 放进交接槽, 由 loop 线程 retrigger 处理挂载 (见槽定义处)。
            // 创建失败只丢画面不丢链 (与首启路径同判)。
            OHNativeWindow *late = nullptr;
            int32_t rc = OH_NativeWindow_CreateNativeWindowFromSurfaceId(
                surface_id, &late);
            OH_LOG_INFO(LOG_APP,
                        "late present window from surface %{public}llu rc=%{public}d",
                        (unsigned long long)surface_id, rc);
            if (rc == 0 && late)
            {
                std::lock_guard<std::mutex> lk(g_late_window_mutex);
                g_late_window = late;
                g_late_canvas_egl = canvas_egl_present;
            }
        }
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
    // xclient 测试客户端与注入脚本同源 (同一 smoke 编排语义), 共用此门。
    g_xclient_test_client_enabled = script_enabled;

    if (surface_id != 0)
    {
        int32_t rc = OH_NativeWindow_CreateNativeWindowFromSurfaceId(
            surface_id, &g_present_window);
        OH_LOG_INFO(LOG_APP, "present window from surface %{public}llu rc=%{public}d",
                    (unsigned long long)surface_id, rc);
        if (rc != 0)
            g_present_window = nullptr;
    }

    std::thread([out_w, out_h, canvas_egl_present] {
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
            /* data = loop: wake 处理器在晚到画布挂载后武装 present 探针
             * (见 DisplayRouteRetriggerWake 内注释) */
            wl_event_loop_add_fd(loop, g_retrigger_pipe[0], WL_EVENT_READABLE,
                                 DisplayRouteRetriggerWake, loop);
        }
        else
        {
            OH_LOG_ERROR(LOG_APP, "retrigger pipe create failed errno=%{public}d", errno);
        }

        // M2-T2: R-ZC ② 探针 —— 标记文件不存在时真跑一次 host EGL OHOS
        // 导入链, 结论落盘 (files 根/displayroute-egl-import-probe), gles2
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
        // D8 (2026-10-05): 出图链无条件启动, window 可为 NULL —— output/
        // wl_output 在链启动即存在, X 屏幕尺寸立刻正确; 画布 (present 窗)
        // 晚到经 g_late_window 槽挂载。实测: 产品路径画布 engine-ready 才开
        // (晚于 explorer 60s+), 无 output ⇒ X 屏幕 0x0 (T5) ⇒ 桌面窗口永不
        // map ⇒ root 恒不就绪。无窗时 present 段在 ohos_output 内挂起。
        {
            int cw = out_w, ch = out_h;
            if (!g_present_window && (cw <= 0 || ch <= 0))
            {
                /* 无画布早启: 尺寸取会话态 (setOutputSize) —— 与 explorer
                 * 桌面尺寸同源同值 (wine_launch Launch-Async 同款读法), X
                 * 屏幕 == wine 桌面由构造保证。smoke 台架带 dr surface
                 * (window 非 NULL), 仍走 ohos_output 的 800x600 校准口径。 */
                auto *wsess = WaylandServer::GetInstance();
                cw = wsess->OutputWidth() > 0 ? wsess->OutputWidth() : 1280;
                ch = wsess->OutputHeight() > 0 ? wsess->OutputHeight() : 720;
            }
            /* 出图链延后到事件循环第一拍 (定时器晚于 Xwayland 的 spawn idle):
             * 先 fork 子进程, 再做 GL —— 次序约束与实测见 DeferredOutputChainStart
             * 上方注释。就绪标记也在那一刻才写 (标记语义 = 出图链已就位)。 */
            g_deferred_chain = (struct DeferredOutputChainStart){
                backend, renderer, loop, wl, g_present_window, xwayland,
                cw, ch, canvas_egl_present};
            struct wl_event_source *chain_timer =
                wl_event_loop_add_timer(loop, StartOutputChainTimer, &g_deferred_chain);
            if (chain_timer)
                wl_event_source_timer_update(chain_timer, 1);
            else
                OH_LOG_ERROR(LOG_APP, "output chain timer 建不起来 (出图链不会启动)");
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
        // M2-T5: guest 帧接收侧先收 (摘帧 → 解绑 → 销毁消费者面), 早了会漏
        // 归还借来的队列帧, 晚了会在 scene 销毁后动悬垂节点。
        display_guest_frames_shutdown();
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
                // present slot 三元组 (wrapper/allocator/swapchain) 持有该
                // window 的队列槽位, 必须先于 window 本体销毁回收 (T6.5)。
                wl_ohos_present_slots_shutdown();
                // M3a-T7: 画布 EGL swap 呈现面同样持有该 window, 先于销毁。
                wl_ohos_egl_window_surface_destroy();
                OH_NativeWindow_DestroyNativeWindow(g_present_window);
                g_present_window = nullptr;
            }
            g_started = false;
            g_stop = false;
        }
        OH_LOG_INFO(LOG_APP, "displayroute stopped or failed; cleaned up (retry possible)");
    }).detach();
}
