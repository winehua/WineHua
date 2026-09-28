/*
 * xclient_child.cpp — mini X client NCP 子进程 (libxclient_child.so, M0-T9)
 *
 * M0 出口件: 依赖面恰 = winex11.drv 硬依赖 (libX11+libXext ONLY, 用户范围
 * 决议 2026-09-27)。经 XOpenDisplay(":0") 连 Xwayland (NCP 形态同 T7 shim,
 * 但无命名 fd——X 连接由 Xlib 自行发起, 这正是 M0 出口要验证的事)。
 * 序列: connect → 建窗 → XStoreName → XPutImage 图案 → XMapWindow
 * → Expose 重绘循环。hilog 标签 XCLIENT-NCP。
 *
 * T2 起支持双窗口 (mode=2): 两窗不同图案色 + 周期 XMoveWindow +
 * StructureNotify 日志, 供合成器多窗口 blit / 注入命中 / 焦点切换的门判。
 * 单窗模式 (mode=1) 保持 M0/T1 行为。键盘回显带窗口 id (M1-T1)。
 *
 * entryParams: "<stderrPath>|<xdgDir>|<mode>" (mode 缺省 1)
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

// 窗口上下文: 双窗口模式下每窗独立图案/GC/XImage
struct WinCtx
{
    Window win = None;
    GC gc = nullptr;
    XImage* img = nullptr;
    int w = 0, h = 0;
    int base_x = 0, base_y = 0;
    uint32_t color_a = 0, color_b = 0;
    const char* tag = "win";
    int exposes = 0;
};

// 对角滚动条纹 + 白边框; color_a/b 为 ABGR 像素值 (0x00RRGGBB 直填低 24 位,
// 位序由 XImage/视觉决定, 测试图案只要求可辨)
uint32_t PatternPixel(const WinCtx& c, int x, int y, int frame)
{
    if (x < 4 || x >= c.w - 4 || y < 4 || y >= c.h - 4)
        return 0xFFFFFF; /* 白边框 */
    return ((x + y + frame) / 16) % 2 ? c.color_a : c.color_b;
}

void DrawFrame(Display* dpy, WinCtx& c, int frame)
{
    uint32_t* px = reinterpret_cast<uint32_t*>(c.img->data);
    for (int y = 0; y < c.h; ++y)
        for (int x = 0; x < c.w; ++x)
            px[(size_t)y * c.w + x] = PatternPixel(c, x, y, frame);
    XPutImage(dpy, c.win, c.gc, c.img, 0, 0, 0, 0, c.w, c.h);
    XFlush(dpy);
}

void SetupWindow(Display* dpy, Window root, int scr, WinCtx& c,
                 int x, int y, int w, int h, uint32_t ca, uint32_t cb,
                 const char* name)
{
    c.w = w;
    c.h = h;
    c.base_x = x;
    c.base_y = y;
    c.color_a = ca;
    c.color_b = cb;
    c.tag = name;
    c.win = XCreateSimpleWindow(dpy, root, x, y, w, h, 2,
                                BlackPixel(dpy, scr), WhitePixel(dpy, scr));
    XStoreName(dpy, c.win, name);
    // T2: StructureNotify 看 XMoveWindow 的随动; 键盘回显见事件循环
    XSelectInput(dpy, c.win,
                 ExposureMask | KeyPressMask | KeyReleaseMask | StructureNotifyMask);
    c.img = XCreateImage(dpy, DefaultVisual(dpy, scr), DefaultDepth(dpy, scr),
                         ZPixmap, 0, nullptr, w, h, 32, 0);
    c.img->data = static_cast<char*>(calloc(1, static_cast<size_t>(w) * h * 4));
    c.img->byte_order = LSBFirst;
    c.gc = XCreateGC(dpy, c.win, 0, nullptr);
}

} // namespace

extern "C" __attribute__((visibility("default"))) void Main(NativeChildProcess_Args args)
{
    const char* params = args.entryParams ? args.entryParams : "";
    OH_LOG_INFO(LOG_APP, "Main enter pid=%{public}d params=%{public}s", getpid(), params);

    // entryParams: "<stderrPath>|<xdgDir>|<mode>"
    std::string entryParams(params);
    std::string parts[3];
    size_t start = 0;
    for (int i = 0; i < 3; ++i)
    {
        size_t bar = entryParams.find('|', start);
        parts[i] = bar == std::string::npos ? entryParams.substr(start)
                                            : entryParams.substr(start, bar - start);
        if (bar == std::string::npos) break;
        start = bar + 1;
    }
    std::string stderrPath = parts[0];
    std::string xdgDir = parts[1];
    int mode = atoi(parts[2].c_str());
    if (mode != 2) mode = 1;

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

    Window root = DefaultRootWindow(dpy);
    // 窗口摆位与 display_input.c 的 T2 注入脚本坐标成对维护:
    // win1 @60,80 320x240 (品红/青), win2 @380,300 280x200 (绿/黄)
    WinCtx w1, w2;
    SetupWindow(dpy, root, scr, w1, 60, 80, 320, 240,
                0xFF00FF, 0x00FFFF, "WineHua-MiniX-1");
    if (mode == 2)
        SetupWindow(dpy, root, scr, w2, 380, 300, 280, 200,
                    0x00FF00, 0xFFFF00, "WineHua-MiniX-2");

    XMapWindow(dpy, w1.win);
    if (mode == 2) XMapWindow(dpy, w2.win);
    XFlush(dpy);
    OH_LOG_INFO(LOG_APP, "mode=%{public}d mapped (win1=0x%{public}lx 320x240 @60,80"
                "%{public}s), center1=(220,200) center2=(520,400)",
                mode, (unsigned long)w1.win,
                mode == 2 ? ", win2=280x200 @380,300" : "");

    int frame = 0;
    int moves = 0;
    // 事件循环: Expose/按键/结构随动; 无事件每秒重绘 (条纹滚动 = 人眼判活)
    while (true)
    {
        bool hasEvent = XPending(dpy) > 0;
        if (!hasEvent)
        {
            DrawFrame(dpy, w1, frame);
            if (mode == 2) DrawFrame(dpy, w2, frame);
            ++frame;
            // T2: 周期移动 win1 (8 步往返, 每步 8px), 验证 xs 位置随动
            if (mode == 2 && frame % 3 == 0)
            {
                int step = moves % 8;
                int dx = (step < 4 ? step : 7 - step) * 8;
                XMoveWindow(dpy, w1.win, w1.base_x + dx, w1.base_y);
                XFlush(dpy);
                ++moves;
            }
            // mode=2 以 ~30fps 重绘 (T3 帧率测量的内容源; 单窗模式保持
            // 1Hz 人眼判活节奏)
            if (mode == 2)
                usleep(33000);
            else
                sleep(1);
        }
        while (XPending(dpy) > 0)
        {
            XEvent ev;
            XNextEvent(dpy, &ev);
            WinCtx* c = nullptr;
            if (ev.xany.window == w1.win) c = &w1;
            else if (mode == 2 && ev.xany.window == w2.win) c = &w2;
            if (!c) continue;
            if (ev.type == Expose)
            {
                ++c->exposes;
                if (c->exposes <= 3 || c->exposes % 50 == 0)
                    OH_LOG_INFO(LOG_APP, "expose #%{public}d %{public}s draw",
                                c->exposes, c->tag);
                DrawFrame(dpy, *c, frame++);
            }
            else if (ev.type == KeyPress || ev.type == KeyRelease)
            {
                // XLookupString 需要 event.display 补全 (XNextEvent 不填)
                ev.xkey.display = dpy;
                char buf[16] = {0};
                KeySym ks = NoSymbol;
                int n = XLookupString(&ev.xkey, buf, sizeof(buf) - 1, &ks, nullptr);
                OH_LOG_INFO(LOG_APP,
                            "key %{public}s win=%{public}lx '%{public}s' (sym=%{public}lu n=%{public}d)",
                            ev.type == KeyPress ? "press" : "release",
                            (unsigned long)c->win, n > 0 ? buf : "",
                            (unsigned long)ks, n);
            }
            else if (ev.type == ConfigureNotify)
            {
                if (moves <= 8 || moves % 20 == 0)
                    OH_LOG_INFO(LOG_APP,
                                "configure win=%{public}lx %{public}dx%{public}d @%{public}d,%{public}d",
                                (unsigned long)c->win, ev.xconfigure.width,
                                ev.xconfigure.height, ev.xconfigure.x,
                                ev.xconfigure.y);
            }
        }
    }
}
