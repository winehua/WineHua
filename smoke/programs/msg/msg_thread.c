/* winehua_t_msg_thread — 跨线程消息（P2，手段 R）。
 * 判定规格见 docs/engineering/testing-programs.md §3.2。
 * 失败特征：死锁/丢失 = 跨线程调度或 attached input 断。
 * 结构：主线程窗口 <-> 工作线程窗口。工作线程 PostMessage 200 条给主窗；
 * 主线程 SendMessage 给工作线程窗口（同步阻塞语义，工作线程消息循环应答）。
 */
#include "../common/winehua_t_check.h"

#define POST_COUNT 200

static HWND g_main_wnd;      /* 主线程窗口 */
static HWND g_worker_wnd;    /* 工作线程窗口 */
static volatile LONG g_received;
static CRITICAL_SECTION g_cs;

static LRESULT CALLBACK main_wndproc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    if (msg == WM_APP + 1)
    {
        EnterCriticalSection(&g_cs);
        g_received++;
        LeaveCriticalSection(&g_cs);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wparam, lparam);
}

static LRESULT CALLBACK worker_wndproc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    if (msg == WM_APP + 2)
        return wparam * 2 + 1; /* 校验返回值：跨线程 SendMessage 同步回传 */
    return DefWindowProcA(hwnd, msg, wparam, lparam);
}

static DWORD WINAPI worker_main(LPVOID arg)
{
    MSG msg;
    int i;
    WNDCLASSA wwc;
    (void)arg;

    /* 工作线程窗口必须用自己的类——类级 wndproc 是全局的，复用主线程类
     * 会让 WM_APP+2 落进 main_wndproc 而 DefWindowProc 吞掉返回 0 */
    memset(&wwc, 0, sizeof(wwc));
    wwc.lpfnWndProc = worker_wndproc;
    wwc.hInstance = GetModuleHandleA(NULL);
    wwc.lpszClassName = "WineHuaT_MsgThreadWorker";
    RegisterClassA(&wwc);

    g_worker_wnd = CreateWindowExA(0, "WineHuaT_MsgThreadWorker", "worker",
                                   WS_OVERLAPPEDWINDOW, 400, 300, 200, 150,
                                   NULL, NULL, GetModuleHandleA(NULL), NULL);
    PostThreadMessage(GetCurrentThreadId(), WM_NULL, 0, 0); /* 强制队列建立 */
    PeekMessageA(&msg, NULL, WM_USER, WM_USER, PM_NOREMOVE); /* 强制队列 */
    PostMessageA(g_main_wnd, WM_APP + 1, 0, 0); /* 就绪信号（计入总数无害） */

    for (i = 0; i < POST_COUNT; ++i)
        PostMessageA(g_main_wnd, WM_APP + 1, 0, 0);

    while (GetMessageA(&msg, NULL, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
        if (msg.message == WM_QUIT)
            break;
    }
    return 0;
}

int main(int argc, char **argv)
{
    WNDCLASSA wc;
    HANDLE thread;
    MSG msg;
    int i;
    LRESULT reply;

    t_begin("winehua_t_msg_thread", argc, argv);

    InitializeCriticalSection(&g_cs);
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = main_wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "WineHuaT_MsgThread";
    TCHECK("register-class", RegisterClassA(&wc) != 0);

    g_main_wnd = CreateWindowExA(0, "WineHuaT_MsgThread", "main",
                                 WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                                 60, 50, 300, 200, NULL, NULL, wc.hInstance, NULL);
    t_check("create-main", g_main_wnd != NULL, "err=%lu", g_main_wnd ? 0 : GetLastError());
    if (!g_main_wnd)
        return t_finish();

    thread = CreateThread(NULL, 0, worker_main, NULL, 0, NULL);
    t_check("spawn-worker", thread != NULL, "err=%lu", thread ? 0 : GetLastError());
    if (!thread)
        return t_finish();

    /* 等工作线程窗口就绪 */
    for (i = 0; i < 100 && !g_worker_wnd; ++i)
    {
        if (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        Sleep(20);
    }
    t_check("worker-window", g_worker_wnd != NULL, "waited %d00ms", i);

    /* 跨线程 SendMessage ×3：同步语义 + 返回值往返 */
    reply = SendMessageA(g_worker_wnd, WM_APP + 2, 21, 0);
    t_check("cross-thread-sendmessage", reply == 43, "reply=%ld expect 43", (long)reply);
    reply = SendMessageA(g_worker_wnd, WM_APP + 2, 100, 0);
    t_check("cross-thread-sendmessage-2", reply == 201, "reply=%ld expect 201", (long)reply);

    /* 收 post：POST_COUNT + 1（就绪信号） */
    {
        DWORD until = GetTickCount() + 5000;
        while (GetTickCount() < until)
        {
            while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE))
            {
                TranslateMessage(&msg);
                DispatchMessageA(&msg);
            }
            LONG n;
            EnterCriticalSection(&g_cs);
            n = g_received;
            LeaveCriticalSection(&g_cs);
            if (n >= POST_COUNT + 1)
                break;
            Sleep(20);
        }
        EnterCriticalSection(&g_cs);
        i = g_received;
        LeaveCriticalSection(&g_cs);
        t_check("post-no-loss", i == POST_COUNT + 1,
                "received %d expect %d", i, POST_COUNT + 1);
    }

    /* 结束工作线程 */
    PostThreadMessage(0, WM_QUIT, 0, 0); /* 占位无效，改发窗口消息 */
    PostMessageA(g_worker_wnd, WM_QUIT, 0, 0);
    WaitForSingleObject(thread, 3000);
    CloseHandle(thread);
    DeleteCriticalSection(&g_cs);
    DestroyWindow(g_main_wnd);
    return t_finish();
}
