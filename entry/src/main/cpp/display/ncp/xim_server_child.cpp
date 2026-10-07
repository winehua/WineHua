/* xim_server_child.cpp — XIM server NCP 子进程 (XIM spec §3 NCP 承载)。
 *
 * 为什么在子进程: X1 探针实测 app 主进程 XOpenDisplay 挂起不返回 (X socket
 * 对主进程 mount namespace 不可达, 2026-10-07, 提交 1392d60); xclient_child
 * 所在的 NCP 子进程与 Xwayland 同 namespace, 连接可行 (xclient_child.cpp
 * /tmp 探针 + sockets.c 标准路径优先补丁)。
 *
 * 入口形态: OH_Ability_StartNativeChildProcess("libxim_server_child.so:Main")
 *   entryParams: "<stderrPath>|<xdgDir>"
 *   fdList: "xim_fd" = socketpair 子端 (主进程 xim_bridge_channel_init 创建,
 *           主端在合成器进程, drain 经它转投 commit 请求)
 * 通道协议: {magic u32 = 0x57485349 ('WHSI'), len u32, utf8 bytes}
 *
 * XIM 注册面 (libX11 ximcp 发现序列):
 *   1. root 的 XIM_SERVERS property (ATOM) 列出 "winehua"
 *   2. selection XIM_SERVERS 的 owner = 本进程 server 窗口
 *   3. wine 侧 XOpenIM (@im=winehua, XMODIFIERS 由 wine_env 下发) 读
 *      property → XConvertSelection → SelectionRequest → 应答 → 握手
 */
#include <X11/Xlib.h>
#include <X11/Xatom.h>

#include <AbilityKit/native_child_process.h>
#include <hilog/log.h>

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <pthread.h>
#include <string>
#include <unistd.h>

#define TAG "xim-server"
#define OHLOG(...) OH_LOG_INFO(LOG_APP, __VA_ARGS__)
#define OHERR(...) OH_LOG_ERROR(LOG_APP, __VA_ARGS__)

namespace {

/* 按名取父进程传下的 fd (同 xwayland_child.cpp:44 的 FdByName) */
bool FdByName(const NativeChildProcess_Args &args, const char *name, int *out)
{
    for (NativeChildProcess_Fd *node = args.fdList.head; node; node = node->next)
    {
        if (node->fdName && strcmp(node->fdName, name) == 0)
        {
            *out = node->fd;
            return true;
        }
    }
    return false;
}

Display *g_dpy;
Window g_srv_win;
int g_chan_fd = -1;
std::atomic<bool> g_run{true};

/* socket 读线程: 主进程转投的 commit 请求 (Task 4 接 XIM_COMMIT 发送)。 */
void *ChanThread(void *arg)
{
    (void)arg;
    uint32_t hdr[2];
    while (g_run.load())
    {
        ssize_t n = read(g_chan_fd, hdr, sizeof(hdr));
        if (n == 0)
            break; /* 主端关闭 */
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            break;
        }
        if (n != sizeof(hdr) || hdr[0] != 0x57485349u)
        {
            OHERR("chan: bad header n=%{public}zd magic=%{public}u", n, hdr[0]);
            continue;
        }
        std::string text(hdr[1], '\0');
        size_t off = 0;
        while (off < hdr[1])
        {
            n = read(g_chan_fd, &text[off], hdr[1] - off);
            if (n <= 0)
                return NULL;
            off += (size_t)n;
        }
        OHLOG("chan: text len=%{public}u (commit 接线 Task 4)", hdr[1]);
    }
    OHLOG("chan: closed");
    return NULL;
}

} // namespace

extern "C" __attribute__((visibility("default"))) void Main(NativeChildProcess_Args args)
{
    const char *params = args.entryParams ? args.entryParams : "";
    /* NCP 子进程的 hilog LOG_APP 不可见 (Task 2 实测: spawn 后 0 行), 关键
     * 状态走 fprintf(stderr)——stderr 已重定向到 xim_server_stderr.log。 */
    fprintf(stderr, "[xim-server] Main enter pid=%d params=%s\n", getpid(), params);
    OHLOG("Main enter pid=%{public}d params=%{public}s", getpid(), params);

    /* entryParams: "<stderrPath>|<xdgDir>" (同 xclient_child 解析) */
    std::string entryParams(params);
    std::string parts[2];
    size_t start = 0;
    for (int i = 0; i < 2; ++i)
    {
        size_t bar = entryParams.find('|', start);
        parts[i] = bar == std::string::npos ? entryParams.substr(start)
                                            : entryParams.substr(start, bar - start);
        if (bar == std::string::npos) break;
        start = bar + 1;
    }
    if (!parts[0].empty())
    {
        int logFd = open(parts[0].c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (logFd >= 0)
        {
            dup2(logFd, STDOUT_FILENO);
            dup2(logFd, STDERR_FILENO);
            if (logFd > STDERR_FILENO) close(logFd);
        }
    }
    if (!parts[1].empty())
        setenv("XDG_RUNTIME_DIR", parts[1].c_str(), 1);

    if (!FdByName(args, "xim_fd", &g_chan_fd) || g_chan_fd < 0)
    {
        fprintf(stderr, "[xim-server] missing xim_fd, aborting\n");
        OHERR("missing xim_fd, aborting");
        return;
    }
    fprintf(stderr, "[xim-server] chan fd=%d\n", g_chan_fd);
    OHLOG("chan fd ready fd=%{public}d", g_chan_fd);

    g_dpy = XOpenDisplay(":0");
    if (!g_dpy)
    {
        fprintf(stderr, "[xim-server] XOpenDisplay FAILED\n");
        OHERR("XOpenDisplay FAILED (namespace 与 xclient 不同?)");
        return;
    }
    fprintf(stderr, "[xim-server] X connected vendor=%s\n", XServerVendor(g_dpy));
    OHLOG("X connected vendor=%{public}s", XServerVendor(g_dpy));

    /* XIM 注册面 */
    Window root = DefaultRootWindow(g_dpy);
    Atom servers = XInternAtom(g_dpy, "XIM_SERVERS", False);
    Atom winehua = XInternAtom(g_dpy, "winehua", False);
    XChangeProperty(g_dpy, root, servers, XA_ATOM, 32, PropModeReplace,
                    (const unsigned char *)&winehua, 1);
    XSetWindowAttributes swa = {};
    g_srv_win = XCreateWindow(g_dpy, root, -1, -1, 1, 1, 0, CopyFromParent,
                              InputOnly, CopyFromParent, CWEventMask, &swa);
    XSetSelectionOwner(g_dpy, servers, g_srv_win, CurrentTime);
    if (XGetSelectionOwner(g_dpy, servers) != g_srv_win)
    {
        fprintf(stderr, "[xim-server] selection ownership FAILED\n");
        OHERR("selection ownership FAILED");
        return;
    }
    fprintf(stderr, "[xim-server] registered XIM_SERVERS owner=0x%lx\n",
            (unsigned long)g_srv_win);
    OHLOG("registered XIM_SERVERS owner=0x%{public}lx",
          (unsigned long)g_srv_win);

    pthread_t chan;
    if (pthread_create(&chan, NULL, ChanThread, NULL) == 0)
        pthread_detach(chan);

    /* X 事件循环: SelectionRequest 应答 (发现面) + ClientMessage 状态机
     * (Task 3 握手 / Task 4 COMMIT)。 */
    while (g_run.load())
    {
        while (XPending(g_dpy))
        {
            XEvent ev;
            XNextEvent(g_dpy, &ev);
            if (ev.type == SelectionRequest)
            {
                XSelectionEvent sev = {};
                sev.type = SelectionNotify;
                sev.display = g_dpy;
                sev.requestor = ev.xselectionrequest.requestor;
                sev.selection = ev.xselectionrequest.selection;
                sev.target = ev.xselectionrequest.target;
                sev.property = ev.xselectionrequest.property;
                sev.time = CurrentTime;
                XChangeProperty(g_dpy, sev.requestor, sev.property, XA_ATOM, 32,
                                PropModeReplace,
                                (const unsigned char *)&winehua, 1);
                XSendEvent(g_dpy, sev.requestor, False, 0, (XEvent *)&sev);
                OHLOG("selection request served req=0x%{public}lx",
                      (unsigned long)sev.requestor);
            }
        }
        usleep(20 * 1000); /* 事件泵 50Hz (Task 3 起 XPending 阻塞版可选) */
    }
}
