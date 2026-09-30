/* winehua_t_filemap — 文件映射与跨进程共享内存（P1）。
 * 判定规格见 docs/engineering/testing-programs.md §3.17。
 * 失败特征：跨进程断 = shm/fd 继承链。
 * 协议：argv[1]=="--t-child" 子进程读共享内存校验后 exit(9)。
 */
#include "../common/winehua_t_check.h"

#define MAP_NAME "WineHuaT_FileMap"
#define MAP_SIZE 4096

static int run_child(void)
{
    HANDLE map = OpenFileMappingA(FILE_MAP_READ, FALSE, MAP_NAME);
    unsigned char *view;
    int i;

    if (!map)
        return 3;
    view = (unsigned char *)MapViewOfFile(map, FILE_MAP_READ, 0, 0, MAP_SIZE);
    if (!view)
        return 4;
    for (i = 0; i < 256; ++i)
    {
        if (view[i] != (unsigned char)(i * 7 + 3))
            return 5;
    }
    UnmapViewOfFile(view);
    CloseHandle(map);
    return 9;
}

int main(int argc, char **argv)
{
    HANDLE map, file, mapping2;
    unsigned char *view;
    char temp[MAX_PATH], path[MAX_PATH];
    DWORD rc;
    int i;

    t_begin("winehua_t_filemap", argc, argv);

    if (argc > 1 && !lstrcmpA(argv[1], "--t-child"))
        return run_child();

    TCHECK("get-temp-path", (GetTempPathA(sizeof(temp), temp) > 0));
    snprintf(path, sizeof(path), "%sfilemap.bin", temp);

    /* 页面文件 backed 共享内存 */
    map = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
                             0, MAP_SIZE, MAP_NAME);
    t_check("create-pagemap", map != NULL, "err=%lu", map ? 0 : GetLastError());
    if (!map)
        return t_finish();
    view = (unsigned char *)MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, MAP_SIZE);
    t_check("map-view", view != NULL, "err=%lu", view ? 0 : GetLastError());
    if (!view)
        return t_finish();
    for (i = 0; i < 256; ++i)
        view[i] = (unsigned char)(i * 7 + 3);
    t_check("write-pattern", view[255] == (unsigned char)(255 * 7 + 3), "pattern");

    /* 子进程读同一命名映射 */
    {
        STARTUPINFOA si;
        PROCESS_INFORMATION pi;
        char cmdline[MAX_PATH + 24];
        DWORD exit_code = 0;
        memset(&si, 0, sizeof(si));
        si.cb = sizeof(si);
        memset(&pi, 0, sizeof(pi));
        rc = GetModuleFileNameA(NULL, cmdline, (DWORD)(sizeof(cmdline) - 24));
        if (rc == 0 || rc >= sizeof(cmdline) - 24)
        {
            t_check("self-path", 0, "rc=%lu", rc);
            return t_finish();
        }
        lstrcatA(cmdline, " --t-child");
        rc = CreateProcessA(NULL, cmdline, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi);
        t_check("spawn-reader", rc != 0, "err=%lu", rc == 0 ? GetLastError() : 0);
        if (rc)
        {
            WaitForSingleObject(pi.hProcess, 10000);
            GetExitCodeProcess(pi.hProcess, &exit_code);
            t_check("cross-process-read", exit_code == 9,
                    "child exit=%lu (9=ok 3=open-failed 4=mapview 5=mismatch)",
                    exit_code);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
    }
    UnmapViewOfFile(view);
    CloseHandle(map);

    /* 文件 backed 映射：写穿到磁盘后按普通文件读回 */
    file = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                       FILE_ATTRIBUTE_NORMAL, NULL);
    t_check("create-file", file != INVALID_HANDLE_VALUE, "err=%lu",
            file == INVALID_HANDLE_VALUE ? GetLastError() : 0);
    if (file != INVALID_HANDLE_VALUE)
    {
        mapping2 = CreateFileMappingA(file, NULL, PAGE_READWRITE, 0, MAP_SIZE, NULL);
        t_check("create-filemap", mapping2 != NULL, "err=%lu",
                mapping2 ? 0 : GetLastError());
        if (mapping2)
        {
            view = (unsigned char *)MapViewOfFile(mapping2, FILE_MAP_ALL_ACCESS, 0, 0, MAP_SIZE);
            if (view)
            {
                memcpy(view, "MAPPED-CONTENT-42", 17);
                /* 写穿透读（同一映射内可见） */
                t_check("filemap-visible", memcmp(view, "MAPPED-CONTENT-42", 17) == 0,
                        "view=%s", view);
                UnmapViewOfFile(view);
            }
            CloseHandle(mapping2);
        }
        /* 普通文件语义读回（coherence） */
        SetFilePointer(file, 0, NULL, FILE_BEGIN);
        {
            unsigned char buf[17];
            DWORD read = 0;
            BOOL ok = ReadFile(file, buf, 17, &read, NULL) && read == 17 &&
                      memcmp(buf, "MAPPED-CONTENT-42", 17) == 0;
            t_check("file-coherence", ok, "read=%lu err=%lu", read,
                    ok ? 0 : GetLastError());
        }
        CloseHandle(file);
        DeleteFileA(path);
    }
    return t_finish();
}
