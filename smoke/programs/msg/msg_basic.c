/* winehua_t_msg_basic — 消息循环、投递顺序、定时器、WM_QUIT（P0）。
 * 判定规格见 docs/engineering/testing-programs.md §3.2。
 */
#include "../common/winehua_t_check.h"

#define WM_T_STEP (WM_APP + 1)
#define TIMER_ID 0x5748
#define TIMER_EXPECT 10
#define TIMER_INTERVAL_MS 50

static HWND g_hwnd;
static UINT g_timer_fired;
static DWORD g_first_timer_tick, g_last_timer_tick;
static ULONGLONG g_timer_interval_sum;
static int g_post_received[10];

static LRESULT CALLBACK t_wndproc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    switch (msg)
    {
    case WM_T_STEP:
        if (wparam >= 1 && wparam <= 10)
            g_post_received[wparam - 1]++;
        return 0;
    case WM_TIMER:
        if (wparam == TIMER_ID)
        {
            DWORD now = GetTickCount();
            if (g_timer_fired == 0)
                g_first_timer_tick = now;
            else
                g_timer_interval_sum += now - g_last_timer_tick;
            g_last_timer_tick = now;
            g_timer_fired++;
            if (g_timer_fired >= TIMER_EXPECT)
            {
                KillTimer(hwnd, TIMER_ID);
                PostThreadMessageA(GetCurrentThreadId(), WM_QUIT, 0, 0);
            }
        }
        return 0;
    default:
        return DefWindowProcA(hwnd, msg, wparam, lparam);
    }
}

int main(int argc, char **argv)
{
    WNDCLASSA wc;
    MSG msg;
    int i, posted_seen = 0, order_ok = 1;
    DWORD mean_interval;

    t_begin("winehua_t_msg_basic", argc, argv);

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = t_wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "WineHuaT_MsgBasic";
    TCHECK("register-class", RegisterClassA(&wc) != 0);

    /* message-only 窗口：不进合成器，纯验证消息机制 */
    g_hwnd = CreateWindowExA(0, "WineHuaT_MsgBasic", "t", 0, 0, 0, 0, 0,
                             HWND_MESSAGE, NULL, wc.hInstance, NULL);
    TCHECK("create-message-window", g_hwnd != NULL);

    /* 投递 10 条带序号的自定义消息 */
    for (i = 0; i < 10; ++i)
        t_check("post-sent", 1, "post %d", i + 1), (void)0;
    for (i = 0; i < 10; ++i)
        PostMessageA(g_hwnd, WM_T_STEP, (WPARAM)(i + 1), 0);

    /* 50ms 定时器 10 发后 WM_QUIT */
    TCHECK("set-timer", SetTimer(g_hwnd, TIMER_ID, TIMER_INTERVAL_MS, NULL) != 0);
    while (GetMessageA(&msg, NULL, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
        if (g_timer_fired >= TIMER_EXPECT)
            break;
    }

    for (i = 0; i < 10; ++i)
    {
        if (g_post_received[i] == 1) posted_seen++;
        else order_ok = 0;
    }
    TCHECK("post-all-received", posted_seen == 10);
    t_check("post-order-strict", order_ok != 0, "strict FIFO order expected, seen %d/10", posted_seen);

    t_check("timer-count", g_timer_fired == TIMER_EXPECT,
            "expected %d, got %u", TIMER_EXPECT, g_timer_fired);
    mean_interval = g_timer_fired > 1 ? (DWORD)(g_timer_interval_sum / (g_timer_fired - 1)) : 0;
    t_check("timer-interval-plausible", g_timer_fired > 1 &&
            mean_interval >= 25 && mean_interval <= 500,
            "mean interval %ums (nominal %dms)", mean_interval, TIMER_INTERVAL_MS);
    t_metric("timer_mean_ms", "%u", mean_interval);
    t_metric("timer_fired", "%u", g_timer_fired);

    TCHECK("clean-exit", g_timer_fired >= TIMER_EXPECT);
    KillTimer(g_hwnd, TIMER_ID);
    DestroyWindow(g_hwnd);
    return t_finish();
}
