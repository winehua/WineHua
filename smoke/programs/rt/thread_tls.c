/* winehua_t_thread_tls — TLS 与同步（P3，手段 R）。
 * 判定规格见 docs/engineering/testing-programs.md §3.9。
 * 失败特征：串扰/丢增=线程局部存储或锁语义断（box64/宿主线程栈回归）。
 * 协议：TlsAlloc → 4 线程各写各读校验（互不串扰）→ CriticalSection
 * 并发计数（8 线程 × 10000 次）无丢增。
 */
#include "../common/winehua_t_check.h"

#define TLS_THREADS 4
#define CS_THREADS 8
#define CS_ITERS 10000

static DWORD g_tls_index;
static volatile LONG g_tls_fail;
static LONG g_cs_counter;
static CRITICAL_SECTION g_cs;
static volatile LONG g_cs_fail;

static DWORD WINAPI tls_worker(LPVOID arg)
{
    int id = (int)(UINT_PTR)arg;
    int i;
    TlsSetValue(g_tls_index, (LPVOID)(UINT_PTR)(id + 1000));
    for (i = 0; i < 200; ++i)
    {
        UINT_PTR v = (UINT_PTR)TlsGetValue(g_tls_index);
        if (v != (UINT_PTR)(id + 1000))
        {
            InterlockedExchange(&g_tls_fail, v == 0 ? 2 : 1); /* 2=丢值 1=串扰 */
            return 0;
        }
        SwitchToThread();
    }
    return 0;
}

static DWORD WINAPI cs_worker(LPVOID arg)
{
    int i;
    (void)arg;
    for (i = 0; i < CS_ITERS; ++i)
    {
        EnterCriticalSection(&g_cs);
        {
            LONG v = g_cs_counter;
            SwitchToThread();
            g_cs_counter = v + 1;
        }
        LeaveCriticalSection(&g_cs);
    }
    return 0;
}

int main(int argc, char **argv)
{
    HANDLE th[TLS_THREADS > CS_THREADS ? TLS_THREADS : CS_THREADS];
    int i;

    t_begin("winehua_t_thread_tls", argc, argv);

    g_tls_index = TlsAlloc();
    t_check("tls-alloc", g_tls_index != TLS_OUT_OF_INDEXES, "idx=%lu", g_tls_index);
    if (g_tls_index == TLS_OUT_OF_INDEXES)
        return t_finish();

    for (i = 0; i < TLS_THREADS; ++i)
        th[i] = CreateThread(NULL, 0, tls_worker, (LPVOID)(UINT_PTR)i, 0, NULL);
    for (i = 0; i < TLS_THREADS; ++i)
    {
        WaitForSingleObject(th[i], 10000);
        CloseHandle(th[i]);
    }
    t_check("tls-no-crosstalk", g_tls_fail == 0, "fail=%ld", (long)g_tls_fail);
    /* 主线程自己的 TLS 独立于工作线程 */
    t_check("tls-main-isolated", TlsGetValue(g_tls_index) == NULL,
            "main=%p", TlsGetValue(g_tls_index));
    TlsFree(g_tls_index);

    InitializeCriticalSection(&g_cs);
    for (i = 0; i < CS_THREADS; ++i)
        th[i] = CreateThread(NULL, 0, cs_worker, NULL, 0, NULL);
    for (i = 0; i < CS_THREADS; ++i)
    {
        WaitForSingleObject(th[i], 60000);
        CloseHandle(th[i]);
    }
    t_check("cs-no-lost-update", g_cs_counter == CS_THREADS * CS_ITERS,
            "counter=%ld expect %ld", (long)g_cs_counter,
            (long)CS_THREADS * CS_ITERS);
    t_check("cs-no-fail", g_cs_fail == 0, "fail=%ld", (long)g_cs_fail);
    t_metric("cs-counter", "%ld", (long)g_cs_counter);
    DeleteCriticalSection(&g_cs);
    return t_finish();
}
