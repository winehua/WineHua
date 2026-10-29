/* winehua_t_clip_cross — 跨进程剪贴板（P1，收敛项①验收载荷）。
 * 判定规格见 docs/engineering/testing-programs.md §3.6。
 * 数据路径：实例 1 写入 → 退出 → 实例 2 读回。wineserver 持有剪贴板数据，
 * 宿主 pasteboard 桥（OHOS 系统剪贴板互通）是另一层，本例测 wine 内跨进程。
 * 协议：argv[1]=="--t-writer <文本>" 子进程模式：写入后 exit(0)。
 */
#include "../common/winehua_t_check.h"

#define WRITER_TEXT "WineHuaT-cross-clip-42"

static int run_writer(void)
{
    HANDLE text;
    const char *payload = WRITER_TEXT;
    if (!OpenClipboard(NULL))
        return 2;
    if (!EmptyClipboard())
    {
        CloseClipboard();
        return 3;
    }
    text = GlobalAlloc(GMEM_MOVEABLE, lstrlenA(payload) + 1);
    if (!text)
    {
        CloseClipboard();
        return 4;
    }
    memcpy(GlobalLock(text), payload, lstrlenA(payload) + 1);
    GlobalUnlock(text);
    if (!SetClipboardData(CF_TEXT, text))
    {
        GlobalFree(text);
        CloseClipboard();
        return 5;
    }
    /* 所有权移交剪贴板后由系统释放，不再 GlobalFree */
    CloseClipboard();
    return 0;
}

int main(int argc, char **argv)
{
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    char cmdline[MAX_PATH + 64];
    DWORD rc, exit_code = 99;
    HANDLE data;
    const char *got;

    t_begin("winehua_t_clip_cross", argc, argv);

    if (argc > 2 && !lstrcmpA(argv[1], "--t-writer"))
        return run_writer();

    /* 先清掉既有剪贴板内容，保证读到的只能是本例写入 */
    if (OpenClipboard(NULL))
    {
        EmptyClipboard();
        CloseClipboard();
    }

    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    memset(&pi, 0, sizeof(pi));
    rc = GetModuleFileNameA(NULL, cmdline, (DWORD)(sizeof(cmdline) - 64));
    if (rc == 0 || rc >= sizeof(cmdline) - 64)
    {
        t_check("self-path", 0, "GetModuleFileName rc=%lu", rc);
        return t_finish();
    }
    lstrcatA(cmdline, " --t-writer " WRITER_TEXT);
    rc = CreateProcessA(NULL, cmdline, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi);
    t_check("spawn-writer", rc != 0, "err=%lu", rc == 0 ? GetLastError() : 0);
    if (!rc)
        return t_finish();

    WaitForSingleObject(pi.hProcess, 10000);
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    t_check("writer-exit-clean", exit_code == 0,
            "writer exit=%lu (2=open 3=empty 4=alloc 5=setdata)", exit_code);

    /* 写入进程已退出：数据由剪贴板服务持有，本进程读回应一致 */
    t_check("open-after-writer-exit", OpenClipboard(NULL), "err=%lu", GetLastError());
    if (!OpenClipboard(NULL))
        return t_finish();
    data = GetClipboardData(CF_TEXT);
    t_check("get-cross-data", data != NULL, "err=%lu", data ? 0 : GetLastError());
    if (data)
    {
        got = (const char *)GlobalLock(data);
        t_check("cross-text-match", got && !lstrcmpA(got, WRITER_TEXT),
                "got=%s", got ? got : "(null)");
        if (got)
            GlobalUnlock(data);
    }
    CloseClipboard();
    return t_finish();
}
