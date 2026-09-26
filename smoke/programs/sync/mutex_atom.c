/* winehua_t_mutex_atom — 互斥体与 Atom 表（P3，手段 R）。
 * 判定规格见 docs/engineering/testing-programs.md §3.17。
 * 失败特征：互斥失效=wineserver 同步对象断。
 * 协议：命名互斥体 + 文件映射计数器，4 子进程 + 主进程各 400 次
 * WaitForSingleObject/Release 互斥递增，断言无丢增；GlobalAddAtom/
 * FindAtom/GetAtomName 往返。
 * 子进程：同 exe + 参数 "-mutex-child <计数地址放映射名>"（同进程
 * 创建的命名映射，子进程 OpenFileMapping）。
 */
#include "../common/winehua_t_check.h"

#define MUTEX_NAME "WineHuaT_MutexAtom"
#define MAP_NAME "WineHuaT_MutexAtom_Map"
#define CHILD_ITERS 400
#define CHILDREN 4

int mutex_child_main(void)
{
    HANDLE mutex, map;
    LONG *counter;
    int i;

    mutex = OpenMutexA(SYNCHRONIZE, FALSE, MUTEX_NAME);
    if (!mutex) return 3;
    map = OpenFileMappingA(FILE_MAP_WRITE, FALSE, MAP_NAME);
    if (!map) return 4;
    counter = (LONG *)MapViewOfFile(map, FILE_MAP_WRITE, 0, 0, sizeof(LONG));
    if (!counter) return 5;
    for (i = 0; i < CHILD_ITERS; ++i)
    {
        if (WaitForSingleObject(mutex, 5000) != WAIT_OBJECT_0) break;
        (*counter)++;
        ReleaseMutex(mutex);
    }
    UnmapViewOfFile(counter);
    CloseHandle(map);
    CloseHandle(mutex);
    return 0;
}

int main(int argc, char **argv)
{
    HANDLE mutex, map;
    LONG *counter;
    char exe[MAX_PATH], cmd[MAX_PATH + 64], args[32];
    STARTUPINFOA si;
    PROCESS_INFORMATION pi[CHILDREN];
    int i, child_fail = 0;
    ATOM atom;
    char name[128], back[128];

    t_begin("winehua_t_mutex_atom", argc, argv);

    /* 子进程模式：只做互斥递增 */
    if (argc >= 2 && !lstrcmpiA(argv[1], "-mutex-child"))
        return mutex_child_main();

    mutex = CreateMutexA(NULL, FALSE, MUTEX_NAME);
    t_check("create-mutex", mutex != NULL, "err=%lu", GetLastError());
    map = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0,
                             sizeof(LONG), MAP_NAME);
    t_check("create-map", map != NULL, "err=%lu", GetLastError());
    if (!mutex || !map)
        return t_finish();
    counter = (LONG *)MapViewOfFile(map, FILE_MAP_WRITE, 0, 0, sizeof(LONG));
    if (!counter)
    {
        t_check("map-view", 0, "err=%lu", GetLastError());
        return t_finish();
    }
    *counter = 0;

    /* 4 子进程 + 主进程并发互斥递增 */
    t_check("self-path", GetModuleFileNameA(NULL, exe, MAX_PATH) > 0,
            "err=%lu", GetLastError());
    for (i = 0; i < CHILDREN; ++i)
    {
        ZeroMemory(&si, sizeof(si));
        si.cb = sizeof(si);
        snprintf(args, sizeof(args), "-mutex-child");
        snprintf(cmd, sizeof(cmd), "\"%s\" %s", exe, args);
        if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi[i]))
        {
            child_fail = 1;
            t_check("spawn-child", 0, "i=%d err=%lu", i, GetLastError());
        }
    }
    for (i = 0; !child_fail && i < CHILD_ITERS; ++i)
    {
        if (WaitForSingleObject(mutex, 5000) != WAIT_OBJECT_0) break;
        (*counter)++;
        ReleaseMutex(mutex);
    }
    for (i = 0; !child_fail && i < CHILDREN; ++i)
    {
        DWORD code = 99;
        WaitForSingleObject(pi[i].hProcess, 20000);
        GetExitCodeProcess(pi[i].hProcess, &code);
        if (code != 0)
        {
            child_fail = 1;
            t_check("child-exit", 0, "i=%d exit=%lu", i, (unsigned long)code);
        }
        CloseHandle(pi[i].hProcess);
        CloseHandle(pi[i].hThread);
    }

    if (!child_fail)
        t_check("mutex-no-lost-increment", *counter == (CHILDREN + 1) * CHILD_ITERS,
                "counter=%ld expect %ld", (long)*counter,
                (long)(CHILDREN + 1) * CHILD_ITERS);
    t_metric("mutex-counter", "%ld", (long)*counter);

    /* Atom 表往返 */
    atom = GlobalAddAtomA("WineHuaT_Atom_XYZ");
    t_check("add-atom", atom != 0, "atom=%u", atom);
    t_check("find-atom", GlobalFindAtomA("WineHuaT_Atom_XYZ") == atom,
            "found=%u", GlobalFindAtomA("WineHuaT_Atom_XYZ"));
    i = GlobalGetAtomNameA(atom, back, sizeof(back));
    t_check("atom-name-roundtrip", i > 0 && !lstrcmpiA(back, "WineHuaT_Atom_XYZ"),
            "got='%s'", back);
    GlobalDeleteAtom(atom);
    t_check("atom-deleted", GlobalFindAtomA("WineHuaT_Atom_XYZ") == 0,
            "still=%u", GlobalFindAtomA("WineHuaT_Atom_XYZ"));

    UnmapViewOfFile(counter);
    CloseHandle(map);
    CloseHandle(mutex);
    return t_finish();
}
