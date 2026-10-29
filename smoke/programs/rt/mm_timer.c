/* winehua_t_mm_timer — 多媒体定时器（P3，手段 R）。
 * 判定规格见 docs/engineering/testing-programs.md §3.19。
 * 失败特征：丢失/漂移=winmm 定时链（游戏循环类依赖）。
 * 协议：timeBeginPeriod(1) + timeSetEvent(10ms, TIME_PERIODIC) 运行
 * ~1s → timeKillEvent → 回调次数 80–110；timeGetTime 单调递增。
 */
#include "../common/winehua_t_check.h"
#include <mmsystem.h>

static volatile LONG g_ticks;

static void CALLBACK timer_cb(UINT id, UINT msg, DWORD_PTR user,
                              DWORD_PTR dw1, DWORD_PTR dw2)
{
    InterlockedIncrement(&g_ticks);
}

int main(int argc, char **argv)
{
    MMRESULT res, kill;
    DWORD t0, t1, prev;
    int i, mono_ok = 1;

    t_begin("winehua_t_mm_timer", argc, argv);

    res = timeBeginPeriod(1);
    t_check("begin-period", res == TIMERR_NOERROR, "res=%u", res);

    t0 = timeGetTime();
    Sleep(50);
    t1 = timeGetTime();
    t_check("time-gettime-runs", t1 >= t0 && t1 - t0 >= 40,
            "t0=%lu t1=%lu", (unsigned long)t0, (unsigned long)t1);

    /* 单调性：连续取样不回退 */
    prev = timeGetTime();
    for (i = 0; i < 200; ++i)
    {
        DWORD now = timeGetTime();
        if (now < prev) { mono_ok = 0; break; }
        prev = now;
        Sleep(1);
    }
    t_check("time-monotonic", mono_ok, "mono=%d", mono_ok);

    g_ticks = 0;
    res = timeSetEvent(10, 0, timer_cb, 0, TIME_PERIODIC);
    t_check("set-event", res != 0, "res=%u", res);
    if (res)
    {
        Sleep(1000);
        kill = timeKillEvent(res);
        t_check("kill-event", kill == TIMERR_NOERROR, "res=%u", kill);
        Sleep(100); /* 尾部回调收尾 */
        t_check("tick-count-80-110", g_ticks >= 80 && g_ticks <= 110,
                "ticks=%ld (10ms × ~1s, expect 80-110)",
                (long)g_ticks);
        t_metric("mm-ticks", "%ld", (long)g_ticks);
    }

    timeEndPeriod(1);
    return t_finish();
}
