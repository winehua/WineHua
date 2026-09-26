/* winehua_t_fs_watch — 目录变更通知（P3，手段 R）。
 * 判定规格见 docs/engineering/testing-programs.md §3.7。
 * 失败特征：无通知=watcher 后端断。
 * 协议：ReadDirectoryChangesW 阻塞在工作线程 → 主线程建/删文件 →
 * 解析 FILE_NOTIFY_INFORMATION 链，断言 ADDED/REMOVED 与文件名。
 */
#include "../common/winehua_t_check.h"

#define WATCH_DIR "C:\\smoke\\winehua_t_watch"
#define TARGET_FILE "watched_target.txt"

static volatile LONG g_watch_done;
static int g_got_added, g_got_removed;
static char g_added_name[256], g_removed_name[256];

static DWORD WINAPI watch_thread(LPVOID arg)
{
    char dir[MAX_PATH];
    HANDLE hdir;
    DWORD n;
    static char buf[4096];
    (void)arg;

    lstrcpynA(dir, WATCH_DIR, MAX_PATH);
    hdir = CreateFileA(dir, FILE_LIST_DIRECTORY,
                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (hdir == INVALID_HANDLE_VALUE)
    {
        InterlockedExchange(&g_watch_done, 2);
        return 0;
    }
    InterlockedExchange(&g_watch_done, 1); /* 目录已打开，后续调用阻塞等待 */
    /* 同步阻塞模式：每次调用挂起直到一批变更（工作线程专用，无 IOCP
     * 复杂度）；建/删两事件可能分批到达，循环挂起至收齐 */
    for (n = 0; n < 10 && !(g_got_added && g_got_removed); ++n)
    {
        DWORD got;
        FILE_NOTIFY_INFORMATION *fn;
        if (!ReadDirectoryChangesW(hdir, buf, sizeof(buf), FALSE,
                                   FILE_NOTIFY_CHANGE_FILE_NAME, &got, NULL, NULL))
            break;
        fn = (FILE_NOTIFY_INFORMATION *)buf;
        for (;;)
        {
            char name[256];
            int w = WideCharToMultiByte(CP_ACP, 0, fn->FileName,
                                        fn->FileNameLength / 2,
                                        name, sizeof(name) - 1, NULL, NULL);
            name[w] = '\0';
            if (fn->Action == FILE_ACTION_ADDED)
            {
                g_got_added = 1;
                lstrcpynA(g_added_name, name, sizeof(g_added_name));
            }
            if (fn->Action == FILE_ACTION_REMOVED)
            {
                g_got_removed = 1;
                lstrcpynA(g_removed_name, name, sizeof(g_removed_name));
            }
            if (!fn->NextEntryOffset) break;
            fn = (FILE_NOTIFY_INFORMATION *)((char *)fn + fn->NextEntryOffset);
        }
    }
    CloseHandle(hdir);
    return 0;
}

int main(int argc, char **argv)
{
    char path[MAX_PATH];
    HANDLE thread, h;
    int wait;

    t_begin("winehua_t_fs_watch", argc, argv);

    CreateDirectoryA(WATCH_DIR, NULL);
    snprintf(path, sizeof(path), "%s\\%s", WATCH_DIR, TARGET_FILE);
    DeleteFileA(path); /* 清残留 */

    thread = CreateThread(NULL, 0, watch_thread, NULL, 0, NULL);
    /* 等 watcher 挂上 */
    for (wait = 0; wait < 50 && !g_watch_done; ++wait) Sleep(20);
    t_check("watcher-armed", g_watch_done == 1, "state=%ld", (long)g_watch_done);
    if (g_watch_done != 1)
    {
        CloseHandle(thread);
        return t_finish();
    }

    h = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    t_check("create-file", h != INVALID_HANDLE_VALUE, "err=%lu", GetLastError());
    if (h != INVALID_HANDLE_VALUE)
    {
        DWORD w;
        WriteFile(h, "x", 1, &w, NULL);
        CloseHandle(h);
    }
    Sleep(300);
    t_check("remove-file", DeleteFileA(path), "err=%lu", GetLastError());

    /* 等 watch 线程收齐通知（最多 5s） */
    for (wait = 0; wait < 250 && !(g_got_added && g_got_removed); ++wait) Sleep(20);

    t_check("added-notified", g_got_added == 1, "added=%d", g_got_added);
    t_check("added-name", g_got_added && !lstrcmpiA(g_added_name, TARGET_FILE),
            "name='%s'", g_added_name);
    t_check("removed-notified", g_got_removed == 1, "removed=%d", g_got_removed);
    t_check("removed-name", g_got_removed && !lstrcmpiA(g_removed_name, TARGET_FILE),
            "name='%s'", g_removed_name);
    t_metric("watch-result", "added=%d removed=%d", g_got_added, g_got_removed);

    CloseHandle(thread);
    DeleteFileA(path);
    RemoveDirectoryA(WATCH_DIR);
    return t_finish();
}
