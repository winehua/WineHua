/* winehua_t_input_wheel — 滚轮注入配对（P2，手段 I）。
 * 判定规格见 docs/engineering/testing-programs.md §3.3。
 * 失败特征：delta 单位错 = value120 协议断。
 * 协议：点击窗口中心激活（wheel 按 win32 焦点窗分发, server/queue.c:1827,
 * 未激活收不到）→ 注入 3 格滚轮 → WM_MOUSEWHEEL 累计 delta == 3*120。
 * D29 (2026-10-07) 起带对照臂：host 注入段之后进程内自 SendInput 3 格
 * （计数独立），区分「win32 轮焦点分发坏」与「X 级断链」——self 通而
 * host 断 = 断点在桥→Xwayland→Button4/5 段。曾据此定位 #93 根因（桥把
 * 格数当 v120 直传, 1 格被折成 1/120, DIX 凑不满增量不出按钮）。
 */
#include "../common/winehua_t_check.h"

#define CLIENT_W 320
#define CLIENT_H 240
#define ORIGIN_X 60
#define ORIGIN_Y 50
#define EXPECT_DELTA (3 * 120)

static volatile LONG g_wheel_total;
static int g_wheel_events;

static LRESULT CALLBACK t_wndproc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    if (msg == WM_MOUSEWHEEL)
    {
        g_wheel_total += (short)HIWORD(wparam);
        g_wheel_events++;
        return 0;
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
    HWND hwnd;
    char req_path[MAX_PATH], done_path[MAX_PATH];
    FILE *f;
    int wait;

    t_begin("winehua_t_input_wheel", argc, argv);

    snprintf(req_path, sizeof(req_path),
             "C:\\smoke\\inject-request-%s.json", g_t.options.test_id);
    snprintf(done_path, sizeof(done_path),
             "C:\\smoke\\inject-done-%s.json", g_t.options.test_id);

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = t_wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "WineHuaT_InputWheel";
    TCHECK("register-class", RegisterClassA(&wc) != 0);

    AdjustWindowRect(&frame, WS_OVERLAPPEDWINDOW & ~(WS_THICKFRAME | WS_MAXIMIZEBOX), FALSE);
    hwnd = CreateWindowExA(0, "WineHuaT_InputWheel", "wheel",
                           WS_OVERLAPPEDWINDOW & ~(WS_THICKFRAME | WS_MAXIMIZEBOX) | WS_VISIBLE,
                           ORIGIN_X, ORIGIN_Y,
                           frame.right - frame.left, frame.bottom - frame.top,
                           NULL, NULL, wc.hInstance, NULL);
    t_check("create-window", hwnd != NULL, "err=%lu", hwnd ? 0 : GetLastError());
    if (!hwnd)
        return t_finish();
    pump(800);

    DeleteFileA(req_path);
    DeleteFileA(done_path);
    {
        POINT origin = {0, 0};
        ClientToScreen(hwnd, &origin);
        f = fopen(req_path, "w");
        t_check("write-request", f != NULL, "err=%lu", f ? 0 : GetLastError());
        if (!f)
        {
            DestroyWindow(hwnd);
            return t_finish();
        }
        /* click 激活 (wheel 按 win32 焦点窗分发) → sleep 拉开激活完成 → wheel */
        fprintf(f, "{\"actions\":[{\"type\":\"mouse\",\"x\":%d,\"y\":%d},"
                "{\"type\":\"sleep\",\"ms\":400},"
                "{\"type\":\"wheel\",\"x\":%d,\"y\":%d,\"notches\":3}]}",
                origin.x + CLIENT_W / 2, origin.y + CLIENT_H / 2,
                origin.x + CLIENT_W / 2, origin.y + CLIENT_H / 2);
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

    /* host 注入段结果 (click + wheel 均已执行完): */
    t_check("wheel-events", g_wheel_events >= 1, "got %d events", g_wheel_events);
    t_check("wheel-delta", g_wheel_total == EXPECT_DELTA,
            "total %ld expect %d (notches*120)",
            (long)g_wheel_total, EXPECT_DELTA);
    t_metric("wheel-total", "%ld", (long)g_wheel_total);

    /* 对照臂 (D29): host 注入段跑完后, 进程内自 SendInput 3 格滚轮。
     * wheel 按 win32 焦点窗分发 (server/queue.c:1827), click 激活后焦点应
     * 已在本窗——本探针不经 X, 直接验证 win32 轮焦点分发这条回流路径自身
     * 完好: self 通 + host 断 = 断点在 X 级 (桥→Xwayland→Button4/5);
     * self 也断 = win32 分发本身坏。判定独立记账, 计数先清零防串段。 */
    {
        INPUT in;
        int i;
        g_wheel_events = 0;
        g_wheel_total = 0;
        SetFocus(hwnd); /* 双保险: 激活后焦点理论已在本窗 */
        pump(100);
        memset(&in, 0, sizeof(in));
        in.type = INPUT_MOUSE;
        in.mi.dwFlags = MOUSEEVENTF_WHEEL;
        for (i = 0; i < 3; ++i)
        {
            in.mi.mouseData = (DWORD)(SHORT)(-120); /* 向下 3 格 */
            SendInput(1, &in, sizeof(in));
            pump(30);
        }
        pump(300);
        t_check("self-wheel-events", g_wheel_events >= 3,
                "got %d events", g_wheel_events);
        t_check("self-wheel-delta", g_wheel_total == -3 * 120,
                "total %ld expect %d", (long)g_wheel_total, -3 * 120);
        t_metric("self-wheel-total", "%ld", (long)g_wheel_total);
    }

    DeleteFileA(req_path);
    DeleteFileA(done_path);
    DestroyWindow(hwnd);
    return t_finish();
}
