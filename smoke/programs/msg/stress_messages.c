/* winehua_t_stress_messages — 消息洪泛（P3，手段 R）。
 * 判定规格见 docs/engineering/testing-programs.md §3.15。
 * 失败特征：丢失/挂死=队列丢弃路径断（InputQueue 压力同源）。
 * 协议：向窗口 PostMessage 20000 条带序号消息（分 4 波，波间短暂处理），
 * 全量泵出后断言处理计数==成功投递计数；灌队列阶段记录 PostMessage
 * 满队列返回 FALSE 的次数（合法路径）；结束后 SendMessage 往返正常。
 */
#include "../common/winehua_t_check.h"

#define WM_APP_STRESS (WM_APP + 0x5150)
#define WAVES 4
#define PER_WAVE 5000

static volatile LONG g_processed;
static volatile LONG g_post_failed;
static LONG g_max_seen;

static LRESULT CALLBACK t_wndproc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    if (msg == WM_APP_STRESS)
    {
        InterlockedIncrement(&g_processed);
        if ((LONG)wparam > g_max_seen) g_max_seen = (LONG)wparam;
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wparam, lparam);
}

int main(int argc, char **argv)
{
    WNDCLASSA wc;
    HWND hwnd;
    MSG msg;
    int wave, i;
    LONG total_posted = 0;

    t_begin("winehua_t_stress_messages", argc, argv);

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = t_wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "WineHuaT_StressMsg";
    TCHECK("register-class", RegisterClassA(&wc) != 0);
    hwnd = CreateWindowExA(0, "WineHuaT_StressMsg", "stress",
                           WS_OVERLAPPEDWINDOW, 10, 10, 200, 150,
                           NULL, NULL, wc.hInstance, NULL);
    t_check("create-window", hwnd != NULL, "err=%lu", GetLastError());
    if (!hwnd)
        return t_finish();

    for (wave = 0; wave < WAVES; ++wave)
    {
        for (i = 0; i < PER_WAVE; ++i)
        {
            if (!PostMessageA(hwnd, WM_APP_STRESS, (WPARAM)(total_posted + 1), 0))
            {
                InterlockedIncrement(&g_post_failed);
                continue; /* 队列满被拒是合法路径，计数比对按成功投递算 */
            }
            total_posted++;
        }
        /* 波间泵一部分（周期处理语义） */
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
            if (g_processed >= total_posted) break;
        }
    }

    /* 全量泵出 */
    {
        DWORD until = GetTickCount() + 20000;
        while (GetTickCount() < until)
        {
            while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE))
            {
                TranslateMessage(&msg);
                DispatchMessageA(&msg);
            }
            if (g_processed >= total_posted) break;
            SwitchToThread();
        }
    }

    t_check("all-processed", g_processed == total_posted,
            "processed=%ld posted=%ld failed=%ld",
            (long)g_processed, (long)total_posted, (long)g_post_failed);
    t_check("no-dup-order-sane", g_max_seen == total_posted,
            "max_seen=%ld", (long)g_max_seen);

    /* 结束后消息链路响应正常 */
    {
        ULONG_PTR res = 0;
        DWORD tick = GetTickCount();
        SendMessageTimeoutA(hwnd, WM_APP, 0, 0, SMTO_NORMAL, 3000, &res);
        t_check("responsive-after", GetTickCount() - tick < 3000, "elapsed=%lu",
                (unsigned long)(GetTickCount() - tick));
    }
    t_metric("stress-result", "posted=%ld failed=%ld",
             (long)total_posted, (long)g_post_failed);
    DestroyWindow(hwnd);
    return t_finish();
}
