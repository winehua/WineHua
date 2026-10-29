/* winehua_t_input_relative — 相对指针（P2，手段 I）。
 * 判定规格见 docs/engineering/testing-programs.md §3.3。
 * 失败特征：delta=0 = wine 未进相对模式；绝对坐标同时变化 = 双通道串扰。
 * 协议：ClipCursor + ShowCursor(FALSE) 触发相对模式 → 注册 RAWINPUT →
 * 注入单段 swipe（RIDEV_INPUTSINK 全局收 raw，不依赖焦点）→ 判定
 * "连续步进段"之和与注入位移一致（±20%）。enter 定位校准会合法产生
 * 一条相对差分（SetCursorPos 落入 raw input），不计入判定。
 */
#include "../common/winehua_t_check.h"

#define CLIENT_W 300
#define CLIENT_H 200
#define ORIGIN_X 60
#define ORIGIN_Y 50
#define SWIPE_DX 150
#define SWIPE_DY 40

static long g_raw_dx, g_raw_dy;
static int g_raw_events;
/* 前 N 条 raw 事件轨迹 (flags,dx,dy)，用于定位异常增量的贡献源 */
#define RAW_TRACE_N 24
static int g_raw_trace_flags[RAW_TRACE_N];
static int g_raw_trace_dx[RAW_TRACE_N];
static int g_raw_trace_dy[RAW_TRACE_N];
static int g_raw_trace_n;

static LRESULT CALLBACK t_wndproc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    if (msg == WM_INPUT)
    {
        RAWINPUT raw;
        UINT size = sizeof(raw);
        if (GetRawInputData((HRAWINPUT)lparam, RID_INPUT, &raw, &size,
                            sizeof(RAWINPUTHEADER)) != (UINT)-1)
        {
            if (g_raw_trace_n < RAW_TRACE_N)
            {
                g_raw_trace_flags[g_raw_trace_n] = raw.data.mouse.usFlags;
                g_raw_trace_dx[g_raw_trace_n] = (short)raw.data.mouse.lLastX;
                g_raw_trace_dy[g_raw_trace_n] = (short)raw.data.mouse.lLastY;
                g_raw_trace_n++;
            }
            if (raw.data.mouse.usFlags == MOUSE_MOVE_ABSOLUTE)
            {
                /* 绝对坐标同时到达 = 双通道串扰（失败特征） */
                g_raw_events |= 0x4000;
            }
            else
            {
                g_raw_dx += (short)raw.data.mouse.lLastX;
                g_raw_dy += (short)raw.data.mouse.lLastY;
                g_raw_events++;
            }
        }
    }
    return DefWindowProcA(hwnd, msg, wparam, lparam);
}

static void pump(int ms)
{
    MSG msg;
    DWORD until = GetTickCount() + (DWORD)ms;
    while (GetTickCount() < until)
    {
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        Sleep(20);
    }
}

int main(int argc, char **argv)
{
    WNDCLASSA wc;
    RECT frame = {0, 0, CLIENT_W, CLIENT_H};
    RECT clip = {40, 30, 560, 420};
    HWND hwnd;
    char req_path[MAX_PATH], done_path[MAX_PATH];
    FILE *f;
    int wait;
    POINT pt;

    t_begin("winehua_t_input_relative", argc, argv);

    snprintf(req_path, sizeof(req_path),
             "C:\\smoke\\inject-request-%s.json", g_t.options.test_id);
    snprintf(done_path, sizeof(done_path),
             "C:\\smoke\\inject-done-%s.json", g_t.options.test_id);

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = t_wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "WineHuaT_InputRel";
    TCHECK("register-class", RegisterClassA(&wc) != 0);

    AdjustWindowRect(&frame, WS_OVERLAPPEDWINDOW & ~(WS_THICKFRAME | WS_MAXIMIZEBOX), FALSE);
    hwnd = CreateWindowExA(0, "WineHuaT_InputRel", "rel",
                           WS_OVERLAPPEDWINDOW & ~(WS_THICKFRAME | WS_MAXIMIZEBOX) | WS_VISIBLE,
                           ORIGIN_X, ORIGIN_Y,
                           frame.right - frame.left, frame.bottom - frame.top,
                           NULL, NULL, wc.hInstance, NULL);
    t_check("create-window", hwnd != NULL, "err=%lu", hwnd ? 0 : GetLastError());
    if (!hwnd)
        return t_finish();

    /* RAWINPUT 注册（鼠标相对增量） */
    {
        RAWINPUTDEVICE rid;
        memset(&rid, 0, sizeof(rid));
        rid.usUsagePage = 1;      /* generic desktop */
        rid.usUsage = 2;          /* mouse */
        rid.dwFlags = RIDEV_INPUTSINK;
        rid.hwndTarget = hwnd;
        t_check("register-rawinput", RegisterRawInputDevices(&rid, 1, sizeof(rid)),
                "err=%lu", GetLastError());
    }
    /* 触发相对模式：ClipCursor + 隐藏光标 */
    ClipCursor(&clip);
    ShowCursor(FALSE);
    pump(200);

    DeleteFileA(req_path);
    DeleteFileA(done_path);
    {
        POINT origin = {0, 0};
        ClientToScreen(hwnd, &origin);
        f = fopen(req_path, "w");
        t_check("write-request", f != NULL, "err=%lu", f ? 0 : GetLastError());
        if (!f)
            return t_finish();
        /* 单段 swipe（起点在窗口客户区）。不加 click 前缀：raw input 按
         * RIDEV_INPUTSINK 全局接收不依赖焦点，而 click 的 enter 定位会产生
         * 一条光标校准差分（实测 [0,173,148]）污染 raw 总量断言 */
        fprintf(f, "{\"actions\":["
                   "{\"type\":\"swipe\",\"x\":%d,\"y\":%d,\"dx\":%d,\"dy\":%d,\"steps\":10}"
                   "],\"actionGapMs\":300}",
                origin.x + 40, origin.y + CLIENT_H / 2, SWIPE_DX, SWIPE_DY);
        fclose(f);
    }

    for (wait = 0; wait < 300; ++wait)
    {
        FILE *done = fopen(done_path, "r");
        if (done)
        {
            char buf[256];
            memset(buf, 0, sizeof(buf));
            fgets(buf, sizeof(buf), done);
            fclose(done);
            t_metric("inject-done", "%s", buf);
            break;
        }
        pump(100);
    }
    t_check("inject-done-seen", wait < 300, "waited %d00ms", wait);
    pump(800);

    t_check("raw-events-received", (g_raw_events & 0x3FFF) >= 5,
            "events %d (0x%X)", g_raw_events & 0x3FFF, g_raw_events);
    t_check("raw-no-absolute", !(g_raw_events & 0x4000),
            "absolute-flagged events present (dual channel)");
    /* 判定收在"连续步进段"上：宿主注入是 10 条等步长 (15,4)，找到连续
     * >=8 条步进匹配的样本段，段和即注入位移。enter 定位校准（root/
     * MOVE-ENTER 的 SetCursorPos）合法产生一条相对差分并计入全局累计，
     * 段判定天然排除；全局累计仍作为 raw-total metric 留档 */
    {
        const int step_dx = SWIPE_DX / 10, step_dy = SWIPE_DY / 10;
        const int need = 8;
        int best_start = -1, best_len = 0, i = 0;
        while (i < g_raw_trace_n)
        {
            int len = 0;
            while (i + len < g_raw_trace_n &&
                   g_raw_trace_flags[i + len] == 0 &&
                   g_raw_trace_dx[i + len] == step_dx &&
                   g_raw_trace_dy[i + len] == step_dy)
                len++;
            if (len > best_len) { best_len = len; best_start = i; }
            i += (len > 0) ? len : 1;
        }
        long run_dx = 0, run_dy = 0;
        int k;
        for (k = 0; best_start >= 0 && k < best_len; ++k)
        {
            run_dx += g_raw_trace_dx[best_start + k];
            run_dy += g_raw_trace_dy[best_start + k];
        }
        t_check("raw-run-found", best_len >= need,
                "consecutive step run=%d (need %d, step %d,%d)",
                best_len, need, step_dx, step_dy);
        t_check("raw-run-dx", run_dx >= SWIPE_DX * 80 / 100 && run_dx <= SWIPE_DX * 120 / 100,
                "run dx=%ld expect %d±20%%", run_dx, SWIPE_DX);
        t_check("raw-run-dy", run_dy >= SWIPE_DY * 80 / 100 - 2 && run_dy <= SWIPE_DY * 120 / 100 + 2,
                "run dy=%ld expect %d±20%%", run_dy, SWIPE_DY);
        t_metric("raw-total", "%ld,%ld", g_raw_dx, g_raw_dy);
        t_metric("raw-run", "%ld,%ld len=%d", run_dx, run_dy, best_len);
        /* 逐条轨迹（flag 0x1=ABSOLUTE）：诊断异常增量的贡献源 */
        {
            char trace[1024];
            int off = 0;
            for (i = 0; i < g_raw_trace_n && off < (int)sizeof(trace) - 24; ++i)
                off += snprintf(trace + off, sizeof(trace) - off, "%s[%x,%d,%d]",
                                i ? " " : "", g_raw_trace_flags[i],
                                g_raw_trace_dx[i], g_raw_trace_dy[i]);
            t_metric("raw-trace", "%s", trace);
        }
    }

    /* GetCursorPos 被约束在 clip 矩形内 */
    GetCursorPos(&pt);
    t_check("cursor-clipped", pt.x >= clip.left && pt.x <= clip.right &&
            pt.y >= clip.top && pt.y <= clip.bottom,
            "cursor (%ld,%ld) clip (%ld,%ld)-(%ld,%ld)",
            (long)pt.x, (long)pt.y, (long)clip.left, (long)clip.top,
            (long)clip.right, (long)clip.bottom);

    ClipCursor(NULL);
    ShowCursor(TRUE);
    DeleteFileA(req_path);
    DeleteFileA(done_path);
    DestroyWindow(hwnd);
    return t_finish();
}
