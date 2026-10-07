/* xim_bridge.c — x11 路线 IME/Unicode 通道 (XIM server, spec 2026-10-07-xim)。
 *
 * 背景: X 键盘链上的 Unicode 注入已被三层实测否证 (xkbcommon/xkbcomp 双编译
 * 器分叉 + xkm 缓存按名短路 + xkbcomp 无 Unicode keysym 写法, 记忆
 * xkeymap-dual-compiler-split); 治本 = XIM server——winex11 xim.c 走标准
 * libX11 XOpenIM (xim.c:422), 协议对端是 libX11 的 ximcp, commit 串不经
 * keymap。
 *
 * 线程纪律: 本文件的 Xlib 连接 (g_xdpy) 仅在合成器 loop 线程使用; 跨线程
 * 请求经 display_input 注入队列转交 (同 D19 剪贴板桥纪律)。
 *
 * X1 探针 (本 Task): 验证 (a) app 主进程 XOpenDisplay(":0") 可行 (b) root
 * window XIM_SERVERS property 写入成功。每次链启动都跑、结果只进 hilog
 * (不做标记文件缓存——部署代际无法感知)。
 */
#include "xim_bridge.h"

#include <X11/Xlib.h>
#include <X11/Xatom.h>

#include <pthread.h>

#include <hilog/log.h>

#define TAG "xim-bridge"
#define OHLOG(...) OH_LOG_INFO(LOG_APP, __VA_ARGS__)
#define OHERR(...) OH_LOG_ERROR(LOG_APP, __VA_ARGS__)

/* 连接保持: 后续 Task 在此连接上建 XIM server (Task 2 注册面起用)。
 * 竞态纪律: 探针线程是唯一写者; Task 2 起连接移交 loop 线程 (移交点
 * 加原子标记), 本 Task 内 loop 不读它。 */
static Display *g_xdpy;

/* XOpenDisplay 在 app 主进程实测会挂起 (2026-10-07 首版探针: loop 线程
 * 卡死在调用点后, 35808 线程 0 日志产出、State=S——X socket 对主进程
 * mount namespace 的可达性与 NCP 子进程不同, xclient_child 成功不外推)。
 * 因此探针必须独立线程: 挂起也不阻塞合成器事件循环, 挂多久都只是这个
 * 线程的事。 */
static void *probe_thread(void *arg)
{
    (void)arg;
    g_xdpy = XOpenDisplay(":0");
    if (!g_xdpy)
    {
        OHERR("X1 probe: XOpenDisplay(:0) FAILED (主进程 X socket 不可达?)");
        return NULL;
    }

    Window root = DefaultRootWindow(g_xdpy);
    Atom servers = XInternAtom(g_xdpy, "XIM_SERVERS", False);
    Atom winehua = XInternAtom(g_xdpy, "winehua", False);
    XChangeProperty(g_xdpy, root, servers, XA_ATOM, 32, PropModeReplace,
                    (const unsigned char *)&winehua, 1);

    /* 回读核对 (判定不靠写入返回值——X server 不回错, 必须回读) */
    Atom type = None;
    int fmt = 0;
    unsigned long n = 0, left = 0;
    unsigned char *data = NULL;
    int ok = 0;
    if (XGetWindowProperty(g_xdpy, root, servers, 0, 64, False, XA_ATOM,
                           &type, &fmt, &n, &left, &data) == Success && data)
    {
        ok = (n >= 1 && type == XA_ATOM);
        XFree(data);
    }
    OHLOG("X1 probe: display=%{public}s root_property_write=%{public}s "
          "vendor=%{public}s",
          DisplayString(g_xdpy), ok ? "OK" : "FAIL",
          XServerVendor(g_xdpy));
    return NULL;
}

int xim_bridge_probe_start(void)
{
    if (g_xdpy)
        return 0; /* 已连接 (链重启不重复探针) */

    pthread_t t;
    if (pthread_create(&t, NULL, probe_thread, NULL) != 0)
    {
        OHERR("X1 probe: pthread_create failed");
        return -1;
    }
    pthread_detach(t);
    return 0;
}
