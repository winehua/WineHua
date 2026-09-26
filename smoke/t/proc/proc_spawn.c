/* winehua_t_proc_spawn — 子进程创建/等待/退出码（P0）。
 * 判定规格见 docs/engineering/testing-programs.md §3.9。
 * 失败特征：spawn 失败/挂起 = broker 代 spawn 链断。
 * 协议：argv[1] == "--t-child" 时为子模式，直接 exit(42)。
 */
#include "../common/winehua_t_check.h"

static int run_child_mode(void)
{
    /* 子进程自证被真实拉起：存活 50ms 再退出，父进程等待有实际时长 */
    Sleep(50);
    return 42;
}

int main(int argc, char **argv)
{
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    char cmdline[MAX_PATH + 32];
    DWORD rc, exit_code, wait_rc, start, elapsed;

    t_begin("winehua_t_proc_spawn", argc, argv);

    if (argc > 1 && !lstrcmpA(argv[1], "--t-child"))
        return run_child_mode();

    /* 正向：拉起自身 --t-child，退出码必须为 42 */
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    memset(&pi, 0, sizeof(pi));
    rc = GetModuleFileNameA(NULL, cmdline, (DWORD)(sizeof(cmdline) - 32));
    if (rc == 0 || rc >= sizeof(cmdline) - 32)
    {
        t_check("self-path", 0, "GetModuleFileName rc=%lu", rc);
        return t_finish();
    }
    lstrcatA(cmdline, " --t-child");
    start = GetTickCount();
    rc = CreateProcessA(NULL, cmdline, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi);
    t_check("create-process", rc != 0, "err=%lu", rc == 0 ? GetLastError() : 0);
    if (rc == 0)
        return t_finish();
    t_check("handles-valid", pi.hProcess != NULL && pi.hThread != NULL, "process/thread handles");

    wait_rc = WaitForSingleObject(pi.hProcess, 30000);
    t_check("wait-completed", wait_rc == WAIT_OBJECT_0, "wait_rc=%lu", wait_rc);
    exit_code = 0;
    TCHECK("get-exit-code", (GetExitCodeProcess(pi.hProcess, &exit_code)));
    t_check("exit-code-42", exit_code == 42, "exit_code=%lu", exit_code);
    elapsed = GetTickCount() - start;
    t_check("child-ran-briefly", elapsed >= 30 && elapsed < 30000,
            "elapsed=%lums", elapsed);
    t_metric("spawn_elapsed_ms", "%lu", elapsed);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    /* 负向：不存在的 exe 必须以 ERROR_FILE_NOT_FOUND 拒绝 */
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    memset(&pi, 0, sizeof(pi));
    rc = CreateProcessA(NULL, (LPSTR)"Z:\\definitely_missing_binary_42.exe",
                        NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi);
    t_check("missing-exe-rejected", rc == 0 && GetLastError() == ERROR_FILE_NOT_FOUND,
            "rc=%ld err=%lu", (long)rc, rc == 0 ? GetLastError() : 0);
    if (rc)
    {
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    return t_finish();
}
