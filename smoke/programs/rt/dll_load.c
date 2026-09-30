/* winehua_t_dll_load — 动态库加载/函数解析/卸载（P1）。
 * 判定规格见 docs/engineering/testing-programs.md §3.14。
 * 失败特征：加载失败 = PE 加载器/依赖解析断。
 */
#include "../common/winehua_t_check.h"

typedef void (*void_fn)(void);

int main(int argc, char **argv)
{
    HMODULE mod, mod2, self;
    void_fn fn;

    t_begin("winehua_t_dll_load", argc, argv);

    /* 系统 dll：加载 + 已知导出解析 + 调用 */
    mod = LoadLibraryA("comctl32.dll");
    t_check("load-comctl32", mod != NULL, "err=%lu", mod ? 0 : GetLastError());
    if (mod)
    {
        fn = (void_fn)GetProcAddress(mod, "InitCommonControls");
        t_check("getproc-known", fn != NULL, "err=%lu", fn ? 0 : GetLastError());
        if (fn)
            fn(); /* 无参无返回，调用即验证可达 */
        t_check("free-library", FreeLibrary(mod), "err=%lu", GetLastError());

        /* 重载应拿到同一模块（引用计数语义） */
        mod2 = LoadLibraryA("comctl32.dll");
        t_check("reload-same-module", mod2 == mod, "first=%p second=%p", mod, mod2);
        if (mod2)
            FreeLibrary(mod2);
    }

    /* 自身模块句柄与文件名往返 */
    self = GetModuleHandleA(NULL);
    t_check("self-module", self != NULL, "err=%lu", self ? 0 : GetLastError());
    if (self)
    {
        char self_path[MAX_PATH], exe_path[MAX_PATH];
        DWORD n1 = GetModuleFileNameA(self, self_path, sizeof(self_path));
        DWORD n2 = GetModuleFileNameA(NULL, exe_path, sizeof(exe_path));
        t_check("self-path-roundtrip", n1 > 0 && n1 == n2 && !lstrcmpA(self_path, exe_path),
                "self=%s exe=%s", self_path, exe_path);
    }

    /* 缺失 dll 的错误码语义 */
    SetLastError(0);
    mod = LoadLibraryA("winehua_t_no_such_dll_42.dll");
    t_check("missing-dll-error", mod == NULL && GetLastError() == ERROR_MOD_NOT_FOUND,
            "mod=%p err=%lu", mod, GetLastError());
    return t_finish();
}
