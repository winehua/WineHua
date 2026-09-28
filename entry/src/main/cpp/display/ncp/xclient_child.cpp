/*
 * xclient_child.cpp — mini X client NCP 子进程 (libxclient_child.so, M0-T9)
 *
 * M0 出口件: 依赖面恰 = winex11.drv 硬依赖 (libX11+libXext ONLY, 用户范围
 * 决议 2026-09-27)。经 XOpenDisplay(":0") 连 Xwayland (NCP 形态同 T7 shim,
 * 但无命名 fd——X 连接由 Xlib 自行发起, 这正是 M0 出口要验证的事)。
 * 序列: connect → 建窗 320x240 → XStoreName → XPutImage 图案 (与 wlroots
 * 出图链测试图案不同的另一图案: 品红/青对角条纹, 每帧滚动) → XMapWindow
 * → Expose 重绘循环。hilog 标签 XCLIENT-NCP。
 *
 * entryParams: "<stderrPath>|<xdgDir>" (f0 stderr 落盘, f1 XDG_RUNTIME_DIR)
 */
#include <AbilityKit/native_child_process.h>
#include <hilog/log.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>

#define LOG_TAG "XCLIENT-NCP"

namespace {

// 品红/青对角滚动条纹 + 白边框——与 T8 出图链图案 (黄边框+正交渐变) 可辨
uint32_t PatternPixel(int x, int y, int frame)
{
    if (x < 4 || x >= 320 - 4 || y < 4 || y >= 240 - 4)
        return 0xFFFFFF; /* 白边框 */
    return ((x + y + frame) / 16) % 2 ? 0xFF00FF /* 品红 */ : 0x00FFFF /* 青 */;
}

void DrawFrame(Display* dpy, Window win, GC gc, XImage* img, int frame)
{
    uint32_t* px = reinterpret_cast<uint32_t*>(img->data);
    for (int y = 0; y < 240; ++y)
        for (int x = 0; x < 320; ++x)
            px[(size_t)y * 320 + x] = PatternPixel(x, y, frame);
    XPutImage(dpy, win, gc, img, 0, 0, 0, 0, 320, 240);
    XFlush(dpy);
}

} // namespace

extern "C" __attribute__((visibility("default"))) void Main(NativeChildProcess_Args args)
{
    const char* params = args.entryParams ? args.entryParams : "";
    OH_LOG_INFO(LOG_APP, "Main enter pid=%{public}d params=%{public}s", getpid(), params);

    // entryParams: "<stderrPath>|<xdgDir>"
    std::string entryParams(params);
    size_t bar = entryParams.find('|');
    std::string stderrPath = bar == std::string::npos ? entryParams : entryParams.substr(0, bar);
    std::string xdgDir = bar == std::string::npos ? "" : entryParams.substr(bar + 1);

    if (!stderrPath.empty())
    {
        int logFd = open(stderrPath.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (logFd >= 0)
        {
            dup2(logFd, STDOUT_FILENO);
            dup2(logFd, STDERR_FILENO);
            if (logFd > STDERR_FILENO) close(logFd);
        }
        else
        {
            OH_LOG_WARN(LOG_APP, "stderr redirect open failed errno=%{public}d", errno);
        }
    }
    if (!xdgDir.empty())
        setenv("XDG_RUNTIME_DIR", xdgDir.c_str(), 1);

    // ── /tmp 可写性探针 (M0-T9 实证项, 回填 spec):
    //    libX11 Xtrans 硬编码只探测 /tmp/.X11-unix; sockets.c 已改为标准路径
    //    优先, 这里从客户端视角确认沙箱里 /tmp 到底能不能用。
    if (mkdir("/tmp/.X11-unix", 0755) == 0)
        OH_LOG_INFO(LOG_APP, "probe: /tmp/.X11-unix created (writable)");
    else if (errno == EEXIST)
        OH_LOG_INFO(LOG_APP, "probe: /tmp/.X11-unix exists");
    else
        OH_LOG_INFO(LOG_APP, "probe: /tmp/.X11-unix mkdir errno=%{public}d", errno);

    Display* dpy = XOpenDisplay(":0");
    if (!dpy)
    {
        // 失败诊断 (仅失败路径): errno + 裸 abstract 连接对照
        int pfd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (pfd >= 0)
        {
            struct sockaddr_un pa = {};
            pa.sun_family = AF_UNIX;
            pa.sun_path[0] = '\0';
            strncpy(pa.sun_path + 1, "/tmp/.X11-unix/X0",
                    sizeof(pa.sun_path) - 2);
            int prc = connect(pfd, reinterpret_cast<struct sockaddr*>(&pa),
                              offsetof(struct sockaddr_un, sun_path) +
                                  1 + strlen(pa.sun_path + 1));
            OH_LOG_ERROR(LOG_APP, "XOpenDisplay failed errno=%{public}d | "
                         "raw abstract connect rc=%{public}d",
                         errno, prc);
            close(pfd);
        }
        else
        {
            OH_LOG_ERROR(LOG_APP, "XOpenDisplay failed errno=%{public}d "
                         "(probe socket create failed)", errno);
        }
        return;
    }
    int scr = DefaultScreen(dpy);
    OH_LOG_INFO(LOG_APP, "connect OK display=%{public}s %dx%d depth=%{public}d",
                DisplayString(dpy), DisplayWidth(dpy, scr), DisplayHeight(dpy, scr),
                DefaultDepth(dpy, scr));

    Window win = XCreateSimpleWindow(dpy, DefaultRootWindow(dpy),
                                     100, 100, 320, 240, 2,
                                     BlackPixel(dpy, scr), WhitePixel(dpy, scr));
    XStoreName(dpy, win, "WineHua-MiniX");
    // M1-T1: 键盘回显——注入链的出口证据 (seat → Xwayland → 本进程 X 事件)
    XSelectInput(dpy, win, ExposureMask | KeyPressMask | KeyReleaseMask);

    XImage* img = XCreateImage(dpy, DefaultVisual(dpy, scr), DefaultDepth(dpy, scr),
                               ZPixmap, 0, nullptr, 320, 240, 32, 0);
    if (!img)
    {
        OH_LOG_ERROR(LOG_APP, "XCreateImage failed");
        return;
    }
    img->data = static_cast<char*>(calloc(1, static_cast<size_t>(320) * 240 * 4));
    img->byte_order = LSBFirst;

    GC gc = XCreateGC(dpy, win, 0, nullptr);
    XMapWindow(dpy, win);
    XFlush(dpy);
    OH_LOG_INFO(LOG_APP, "window mapped (320x240 @100,100)");

    int frame = 0;
    int exposes = 0;
    // 事件循环: Expose 重绘; 无事件时每秒重绘一帧 (条纹滚动 = 人眼判活)
    while (true)
    {
        bool hasEvent = XPending(dpy) > 0;
        if (!hasEvent)
        {
            DrawFrame(dpy, win, gc, img, frame++);
            sleep(1);
        }
        while (XPending(dpy) > 0)
        {
            XEvent ev;
            XNextEvent(dpy, &ev);
            if (ev.type == Expose)
            {
                ++exposes;
                if (exposes <= 3 || exposes % 50 == 0)
                    OH_LOG_INFO(LOG_APP, "expose #%{public}d draw", exposes);
                DrawFrame(dpy, win, gc, img, frame++);
            }
            else if (ev.type == KeyPress || ev.type == KeyRelease)
            {
                // XLookupString 需要 event.display 补全 (XNextEvent 不填)
                ev.xkey.display = dpy;
                char buf[16] = {0};
                KeySym ks = NoSymbol;
                int n = XLookupString(&ev.xkey, buf, sizeof(buf) - 1, &ks, nullptr);
                OH_LOG_INFO(LOG_APP,
                            "key %{public}s '%{public}s' (sym=%{public}lu n=%{public}d)",
                            ev.type == KeyPress ? "press" : "release",
                            n > 0 ? buf : "", (unsigned long)ks, n);
            }
        }
    }
}
