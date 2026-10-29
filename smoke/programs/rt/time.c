/* winehua_t_time — 时钟单调性与精度（P2，手段 R）。
 * 判定规格见 docs/engineering/testing-programs.md §3.12。
 * 失败特征：倒退/跳变 = 时钟源断（frame callback 墙钟问题的用户侧观测哨兵）。
 */
#include "../common/winehua_t_check.h"

#define SAMPLES 200

int main(int argc, char **argv)
{
    DWORD tick_prev, tick_now;
    ULONGLONG tick64_prev, tick64_now;
    LARGE_INTEGER qpc_prev, qpc_now, freq;
    int i, monotonic_tick = 1, monotonic_qpc = 1;
    DWORD delta, delta_min = 0xFFFFFFFF, delta_max = 0;

    t_begin("winehua_t_time", argc, argv);

    /* GetTickCount / GetTickCount64 单调性 + 步进 */
    tick_prev = GetTickCount();
    tick64_prev = GetTickCount64();
    for (i = 0; i < SAMPLES; ++i)
    {
        Sleep(5);
        tick_now = GetTickCount();
        tick64_now = GetTickCount64();
        if (tick_now < tick_prev || tick64_now < tick64_prev)
            monotonic_tick = 0;
        delta = tick_now - tick_prev;
        if (delta < delta_min) delta_min = delta;
        if (delta > delta_max) delta_max = delta;
        tick_prev = tick_now;
        tick64_prev = tick64_now;
    }
    t_check("tick-monotonic", monotonic_tick, "backward jump detected");
    /* 系统计时粒度（10-16ms）下 Sleep(5) 的 0 跳合法，只卡上限 */
    t_check("tick-sane-step", delta_max <= 100,
            "delta range %lu..%lums (5ms sleeps)", delta_min, delta_max);

    /* QPC：频率合理 + 单调 */
    t_check("qpf", QueryPerformanceFrequency(&freq) && freq.QuadPart > 1000000,
            "freq=%lld", (long long)freq.QuadPart);
    QueryPerformanceCounter(&qpc_prev);
    for (i = 0; i < 50; ++i)
    {
        QueryPerformanceCounter(&qpc_now);
        if (qpc_now.QuadPart < qpc_prev.QuadPart)
            monotonic_qpc = 0;
        qpc_prev = qpc_now;
        Sleep(2);
    }
    t_check("qpc-monotonic", monotonic_qpc, "backward jump detected");

    /* QPC 与墙钟一致性：300ms Sleep，QPC 换算与 GetTickCount64 同步 */
    {
        ULONGLONG wall0, wall1;
        LARGE_INTEGER q0, q1;
        long long qpc_ms;
        wall0 = GetTickCount64();
        QueryPerformanceCounter(&q0);
        Sleep(300);
        QueryPerformanceCounter(&q1);
        wall1 = GetTickCount64();
        qpc_ms = (long long)((q1.QuadPart - q0.QuadPart) * 1000 / freq.QuadPart);
        t_check("qpc-vs-wall", qpc_ms >= 250 && qpc_ms <= 500 &&
                (long long)(wall1 - wall0) >= 250 && (long long)(wall1 - wall0) <= 500,
                "qpc=%lldms wall=%lldms", qpc_ms, (long long)(wall1 - wall0));
    }

    /* Sleep 精度 ×5 */
    {
        int ok = 1;
        for (i = 0; i < 5; ++i)
        {
            DWORD t0 = GetTickCount();
            Sleep(100);
            delta = GetTickCount() - t0;
            if (delta < 90 || delta > 250)
                ok = 0;
        }
        t_check("sleep-100ms", ok, "5 samples outside 90..250ms");
    }
    return t_finish();
}
