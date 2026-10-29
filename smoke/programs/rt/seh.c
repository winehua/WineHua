/* winehua_t_seh — 结构化异常分发链（P1）。
 * 判定规格见 docs/engineering/testing-programs.md §3.11。
 * 失败特征：崩溃/无痕迹 = 异常翻译链断（box64/FEX 下信号→异常路径回归）。
 * mingw GCC 无 __try/__except 语法，用 vectored handler 等价测量：
 *   case1 软件异常 CONTINUE_EXECUTION → RaiseException 返回后继续跑（exit 21）
 *   case2 除零硬件异常 → handler 记录痕迹后按异常码退出（0xC0000094）
 *   case3 访问违例   → 同上（0xC0000005）
 * 协议：argv[1]=="--t-seh <case> <trace文件>"，父进程 spawn 各 case 断退出码。
 */
#include "../common/winehua_t_check.h"
#include <stdlib.h>

#define SEH_SOFT 0xE0001234

static volatile LONG g_soft_caught;
static char g_trace_path[MAX_PATH];

static LONG CALLBACK soft_handler(PEXCEPTION_POINTERS ptrs)
{
    if (ptrs->ExceptionRecord->ExceptionCode == SEH_SOFT)
    {
        g_soft_caught = 1;
        /* 软件异常继续执行 = RaiseException 直接返回 */
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

/* 硬件异常：痕迹落盘后按异常码退出。不走「未处理异常」语义——wine 下会
 * 先尝试拉 winedbg，无头环境会卡住直至超时。 */
static LONG CALLBACK hw_handler(PEXCEPTION_POINTERS ptrs)
{
    FILE *trace;
    trace = fopen(g_trace_path, "w");
    if (trace)
    {
        fprintf(trace, "code=0x%08lX\n",
                (unsigned long)ptrs->ExceptionRecord->ExceptionCode);
        fclose(trace);
    }
    ExitProcess(ptrs->ExceptionRecord->ExceptionCode);
    return EXCEPTION_CONTINUE_SEARCH; /* 不可达 */
}

static int run_case(int which)
{
    switch (which)
    {
    case 1:
        g_soft_caught = 0;
        AddVectoredExceptionHandler(1, soft_handler);
        RaiseException(SEH_SOFT, 0, 0, NULL);
        return g_soft_caught ? 21 : 22;
    case 2:
        AddVectoredExceptionHandler(1, hw_handler);
        {
            volatile int zero = 0, one = 1;
            one = one / zero;
        }
        return 23; /* 不应到达 */
    case 3:
        AddVectoredExceptionHandler(1, hw_handler);
        {
            volatile int *p = (int *)(UINT_PTR)0x10;
            *p = 42;
        }
        return 24; /* 不应到达 */
    }
    return 25;
}

int main(int argc, char **argv)
{
    char temp[MAX_PATH];
    int cases[3] = {1, 2, 3};
    int i;

    t_begin("winehua_t_seh", argc, argv);

    if (argc > 3 && !lstrcmpA(argv[1], "--t-seh"))
    {
        winehua_smoke_copy_arg(g_trace_path, sizeof(g_trace_path), argv[3]);
        return run_case(atoi(argv[2]));
    }

    TCHECK("get-temp-path", (GetTempPathA(sizeof(temp), temp) > 0));

    for (i = 0; i < 3; ++i)
    {
        STARTUPINFOA si;
        PROCESS_INFORMATION pi;
        char cmdline[MAX_PATH + 64];
        char trace[MAX_PATH + 48];
        DWORD rc, exit_code = 0, expect;
        const char *label;

        snprintf(trace, sizeof(trace), "%s", temp);
        switch (cases[i])
        {
        case 1: label = "soft-continue"; expect = 21; break;
        case 2:
            label = "catch-divide";
            expect = 0xC0000094;
            lstrcatA(trace, "seh_trace_divide.txt");
            break;
        case 3:
            label = "catch-access";
            expect = 0xC0000005;
            lstrcatA(trace, "seh_trace_access.txt");
            break;
        }

        memset(&si, 0, sizeof(si));
        si.cb = sizeof(si);
        memset(&pi, 0, sizeof(pi));
        rc = GetModuleFileNameA(NULL, cmdline, (DWORD)(sizeof(cmdline) - 64));
        if (rc == 0 || rc >= sizeof(cmdline) - 64)
        {
            t_check("self-path", 0, "GetModuleFileName rc=%lu", rc);
            continue;
        }
        snprintf(cmdline + lstrlenA(cmdline), sizeof(cmdline) - lstrlenA(cmdline),
                 " --t-seh %d \"%s\"", cases[i], trace);
        rc = CreateProcessA(NULL, cmdline, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi);
        t_check(label, rc != 0, "spawn err=%lu", rc == 0 ? GetLastError() : 0);
        if (!rc)
            continue;
        WaitForSingleObject(pi.hProcess, 10000);
        GetExitCodeProcess(pi.hProcess, &exit_code);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        t_check(label, exit_code == expect, "exit=0x%08lX expect=0x%08lX",
                exit_code, expect);

        /* 硬件异常两例：handler 痕迹文件应存在且记录了异常码 */
        if (cases[i] != 1)
        {
            char buf[128];
            FILE *f = fopen(trace, "r");
            BOOL ok = FALSE;
            if (f)
            {
                memset(buf, 0, sizeof(buf));
                ok = fgets(buf, sizeof(buf), f) != NULL &&
                     strstr(buf, "code=0x") == buf;
                fclose(f);
                DeleteFileA(trace);
            }
            t_check(label, ok, "trace=%s content=%s", trace, buf);
        }
    }
    return t_finish();
}
