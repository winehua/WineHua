/* winehua_t_sync_kernel — 事件/信号量/跨进程命名事件（P1）。
 * 判定规格见 docs/engineering/testing-programs.md §3.17。
 * 失败特征：命名事件不通 = wineserver 对象命名空间断
 * （wineboot boot 事件挂死历史事故即此链，回归哨兵）。
 * 协议：argv[1]=="--t-child <handle值>" 子进程模式。
 */
#include "../common/winehua_t_check.h"

int main(int argc, char **argv)
{
    HANDLE ev_manual, ev_auto, sem, handles[2];
    DWORD rc;

    t_begin("winehua_t_sync_kernel", argc, argv);

    /* 子模式：打开命名事件 → 等 3 秒内被置位 → 退出 7 */
    if (argc > 1 && !lstrcmpA(argv[1], "--t-child"))
    {
        HANDLE named = OpenEventA(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, "WineHuaT_NamedEvent");
        if (!named)
            return 3;
        rc = WaitForSingleObject(named, 3000);
        return rc == WAIT_OBJECT_0 ? 7 : 4;
    }

    /* 手动复位事件 */
    ev_manual = CreateEventA(NULL, TRUE, FALSE, NULL);
    t_check("create-event-manual", ev_manual != NULL, "err=%lu", GetLastError());
    if (ev_manual)
    {
        rc = WaitForSingleObject(ev_manual, 0);
        t_check("manual-initial-timeout", rc == WAIT_TIMEOUT, "rc=%lu", rc);
        TEXPR(SetEvent(ev_manual));
        rc = WaitForSingleObject(ev_manual, 0);
        t_check("manual-stays-signaled", rc == WAIT_OBJECT_0, "rc=%lu", rc);
        rc = WaitForSingleObject(ev_manual, 0);
        t_check("manual-stays-signaled-2nd", rc == WAIT_OBJECT_0, "rc=%lu", rc);
        TEXPR(ResetEvent(ev_manual));
        rc = WaitForSingleObject(ev_manual, 0);
        t_check("manual-reset-timeout", rc == WAIT_TIMEOUT, "rc=%lu", rc);
        CloseHandle(ev_manual);
    }

    /* 自动复位事件 */
    ev_auto = CreateEventA(NULL, FALSE, TRUE, NULL);
    t_check("create-event-auto", ev_auto != NULL, "err=%lu", GetLastError());
    if (ev_auto)
    {
        rc = WaitForSingleObject(ev_auto, 0);
        t_check("auto-resets-on-wait", rc == WAIT_OBJECT_0, "rc=%lu", rc);
        rc = WaitForSingleObject(ev_auto, 0);
        t_check("auto-cleared-after-wait", rc == WAIT_TIMEOUT, "rc=%lu", rc);
        CloseHandle(ev_auto);
    }

    /* 信号量计数 */
    sem = CreateSemaphoreA(NULL, 2, 2, NULL);
    t_check("create-semaphore", sem != NULL, "err=%lu", GetLastError());
    if (sem)
    {
        rc = WaitForSingleObject(sem, 0);
        rc |= WaitForSingleObject(sem, 0);
        t_check("semaphore-two-acquires", rc == WAIT_OBJECT_0, "rc=%lu", rc);
        rc = WaitForSingleObject(sem, 0);
        t_check("semaphore-exhausted", rc == WAIT_TIMEOUT, "rc=%lu", rc);
        TEXPR(ReleaseSemaphore(sem, 1, NULL));
        rc = WaitForSingleObject(sem, 0);
        t_check("semaphore-released", rc == WAIT_OBJECT_0, "rc=%lu", rc);
        CloseHandle(sem);
    }

    /* WaitForMultipleObjects 混合等待 */
    ev_manual = CreateEventA(NULL, TRUE, TRUE, NULL);
    ev_auto = CreateEventA(NULL, FALSE, TRUE, NULL);
    handles[0] = ev_manual;
    handles[1] = ev_auto;
    rc = WaitForMultipleObjects(2, handles, FALSE, 0);
    t_check("wait-any-signaled", rc >= WAIT_OBJECT_0 && rc <= WAIT_OBJECT_0 + 1,
            "rc=%lu", rc);
    CloseHandle(ev_manual);
    CloseHandle(ev_auto);

    /* 跨进程命名事件：父进程先建命名事件 → 子进程 OpenEvent 等待 →
     * 父进程 SetEvent → 子进程应被唤醒并退出码 7 */
    {
        STARTUPINFOA si;
        PROCESS_INFORMATION pi;
        HANDLE named;
        char cmdline[MAX_PATH + 48];
        DWORD exit_code = 0;

        named = CreateEventA(NULL, TRUE, FALSE, "WineHuaT_NamedEvent");
        t_check("create-named-event", named != NULL, "err=%lu",
                named ? 0 : GetLastError());
        if (!named)
            return t_finish();
        memset(&si, 0, sizeof(si));
        si.cb = sizeof(si);
        memset(&pi, 0, sizeof(pi));
        rc = GetModuleFileNameA(NULL, cmdline, (DWORD)(sizeof(cmdline) - 48));
        if (rc == 0 || rc >= sizeof(cmdline) - 48)
        {
            t_check("self-path", 0, "GetModuleFileName rc=%lu", rc);
            CloseHandle(named);
            return t_finish();
        }
        lstrcatA(cmdline, " --t-child");
        rc = CreateProcessA(NULL, cmdline, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi);
        t_check("spawn-named-waiter", rc != 0, "err=%lu", rc == 0 ? GetLastError() : 0);
        if (rc)
        {
            /* 给子进程留出 OpenEvent 的时间，再跨进程置位 */
            Sleep(300);
            TEXPR(SetEvent(named));
            WaitForSingleObject(pi.hProcess, 5000);
            GetExitCodeProcess(pi.hProcess, &exit_code);
            t_check("named-event-cross-process", exit_code == 7,
                    "child exit=%lu (7=signaled 4=timeout 3=open-failed)", exit_code);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
        CloseHandle(named);
    }
    return t_finish();
}
