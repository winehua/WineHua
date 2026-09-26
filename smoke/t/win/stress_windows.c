/* winehua_t_stress_windows — 窗口账目（P2，手段 R）。
 * 判定规格见 docs/engineering/testing-programs.md §3.16。
 * 失败特征：泄漏/计数漂移 = 会话状态/句柄表断。
 * 建 100 窗（交错显示/隐藏）→ EnumWindows 计数一致 → 分批销毁 → 归零。
 */
#include "../common/winehua_t_check.h"

#define N_WINDOWS 100

static HWND g_wnds[N_WINDOWS];

struct census
{
    DWORD pid;
    int visible;
    int total;
};

static BOOL CALLBACK census_proc(HWND hwnd, LPARAM lparam)
{
    struct census *c = (struct census *)lparam;
    DWORD pid = 0;
    char cls[64];
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != c->pid)
        return TRUE;
    /* 只数本测试的类——进程里还有 wine 的辅助窗口（IME/Message 等） */
    cls[0] = '\0';
    GetClassNameA(hwnd, cls, sizeof(cls));
    if (lstrcmpA(cls, "WineHuaT_StressWin") != 0)
        return TRUE;
    c->total++;
    if (IsWindowVisible(hwnd))
        c->visible++;
    return TRUE;
}

static int count_own_windows(int *visible_out)
{
    struct census c;
    c.pid = GetCurrentProcessId();
    c.visible = 0;
    c.total = 0;
    EnumWindows(census_proc, (LPARAM)&c);
    if (visible_out) *visible_out = c.visible;
    return c.total;
}

int main(int argc, char **argv)
{
    WNDCLASSA wc;
    int i, base, created, visible;
    HINSTANCE inst = GetModuleHandleA(NULL);

    t_begin("winehua_t_stress_windows", argc, argv);

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = inst;
    wc.lpszClassName = "WineHuaT_StressWin";
    TCHECK("register-class", RegisterClassA(&wc) != 0);

    base = count_own_windows(NULL);
    t_metric("baseline-windows", "%d", base);

    created = 0;
    for (i = 0; i < N_WINDOWS; ++i)
    {
        g_wnds[i] = CreateWindowExA(0, "WineHuaT_StressWin", "s",
                                    (i & 1) ? WS_OVERLAPPEDWINDOW | WS_VISIBLE
                                            : WS_OVERLAPPEDWINDOW, /* 隐藏 */
                                    (i % 10) * 30, (i / 10) * 20, 120, 80,
                                    NULL, NULL, inst, NULL);
        if (g_wnds[i])
            created++;
    }
    t_check("created-all", created == N_WINDOWS, "created %d", created);

    {
        int expect_visible = 0;
        for (i = 0; i < N_WINDOWS; ++i)
            if (g_wnds[i] && (i & 1)) expect_visible++;
        /* 建完先全量 ShowWindow 对齐状态，再按 parity 隐藏，避免创建竞态 */
        for (i = 0; i < N_WINDOWS; ++i)
            if (g_wnds[i]) ShowWindow(g_wnds[i], SW_SHOWNOACTIVATE);
        for (i = 0; i < N_WINDOWS; ++i)
            if (g_wnds[i] && !(i & 1)) ShowWindow(g_wnds[i], SW_HIDE);
        UpdateWindow(g_wnds[0]);

        {
            int total = count_own_windows(&visible);
            t_check("census-total", total == base + N_WINDOWS,
                    "total %d expect %d", total, base + N_WINDOWS);
            t_check("census-visible", visible == expect_visible,
                    "visible %d expect %d", visible, expect_visible);
            t_metric("windows-visible", "%d", visible);
        }
    }

    /* 分批销毁（25/批，批间泵消息），完成后归零 */
    {
        int batch, total;
        for (batch = 0; batch < 4; ++batch)
        {
            for (i = batch * 25; i < (batch + 1) * 25; ++i)
                if (g_wnds[i]) DestroyWindow(g_wnds[i]);
            UpdateWindow(g_wnds[0]);
        }
        total = count_own_windows(NULL);
        t_check("census-zero", total == base, "total %d expect baseline %d", total, base);
    }
    return t_finish();
}
