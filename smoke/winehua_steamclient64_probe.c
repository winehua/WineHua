/*
 * winehua_steamclient64_probe.c — Steam win64 启动失败的分层探针
 *
 * 目的 (对应专家方案 Task P0-X64-5 / Step 6):
 *   在同一个 HAP、同一个 prefix、同一个 HODLL64=libarm64ecfex.dll 下,
 *   用 x86_64 进程依次验证:
 *     T0  基础 64 位 runtime   : 系统信息 / 堆 / VirtualAlloc / VirtualProtect
 *     T1  SEH 是否可用          : 故意写只读页, __except 能否捕获
 *     T2  PE 映射 (Stage A)     : LoadLibraryExW(steamclient64.dll, DONT_RESOLVE_DLL_REFERENCES)
 *     T3  完整加载 (Stage B)    : LoadLibraryW(steamclient64.dll)
 *     T4  导出解析 (Stage C)    : GetProcAddress
 *
 * 结果写 C:\smoke\results\steamclient64-probe.json, 同时 printf 到 stderr。
 */
#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

static FILE* g_log = NULL;

static void emit(const char* fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0)
    {
        fputs(buf, stderr);
        fflush(stderr);
        if (g_log) { fputs(buf, g_log); fflush(g_log); }
    }
}

static const char* last_err(void)
{
    static char buf[64];
    snprintf(buf, sizeof(buf), "%lu", (unsigned long)GetLastError());
    return buf;
}

/* ---- T1: 异常分发探测 ---- */
static volatile LONG g_veh_hits = 0;
static volatile DWORD g_veh_code = 0;
static volatile void* g_veh_addr = NULL;

static LONG CALLBACK veh_handler(EXCEPTION_POINTERS* ep)
{
    InterlockedIncrement(&g_veh_hits);
    if (ep && ep->ExceptionRecord)
    {
        g_veh_code = ep->ExceptionRecord->ExceptionCode;
        g_veh_addr = ep->ExceptionRecord->ExceptionAddress;
    }
    /* 不修复现场, 交给默认/后续处理 (线程会因此结束) */
    return EXCEPTION_CONTINUE_SEARCH;
}

static DWORD WINAPI write_ro_thread(void* param)
{
    volatile char* p = (volatile char*)param;
    *p = 0x5A;              /* 只读页写 -> 访问违例 */
    return 0;               /* 不应到达 */
}

static void StartFaultProbe(void* roPage)
{
    PVOID veh = AddVectoredExceptionHandler(1, veh_handler);
    DWORD tid = 0;
    HANDLE th = CreateThread(NULL, 0, write_ro_thread, roPage, 0, &tid);
    emit("[probe-T1] fault thread created handle=%p tid=%lu err=%s\n",
         th, (unsigned long)tid, last_err());
    if (th)
    {
        DWORD wr = WaitForSingleObject(th, 5000);
        DWORD ec = 0;
        GetExitCodeThread(th, &ec);
        emit("[probe-T1] wait=%lu exitCode=0x%lx vehHits=%ld vehCode=0x%lx vehAddr=%p\n",
             (unsigned long)wr, (unsigned long)ec, (long)g_veh_hits,
             (unsigned long)g_veh_code, g_veh_addr);
        CloseHandle(th);
    }
    if (veh) RemoveVectoredExceptionHandler(veh);
}

int main(void)
{
    CreateDirectoryA("C:\\smoke", NULL);
    CreateDirectoryA("C:\\smoke\\results", NULL);
    g_log = fopen("C:\\smoke\\results\\steamclient64-probe.json", "w");
    if (g_log) { fputs("{\n", g_log); fflush(g_log); }

    emit("[probe] x86_64 probe start tid=%lu\n", (unsigned long)GetCurrentThreadId());

    /* ---- T0 基础 runtime ---- */
    SYSTEM_INFO si, nsi;
    GetSystemInfo(&si);
    GetNativeSystemInfo(&nsi);
    emit("[probe-T0] GetSystemInfo.arch=%u GetNativeSystemInfo.arch=%u\n",
         (unsigned)si.wProcessorArchitecture, (unsigned)nsi.wProcessorArchitecture);

    const char* pae = getenv("PROCESSOR_ARCHITE64");
    const char* pa = getenv("PROCESSOR_ARCHITECTURE");
    const char* paew = getenv("PROCESSOR_ARCHITEW6432");
    emit("[probe-T0] PROCESSOR_ARCHITECTURE=%s PROCESSOR_ARCHITEW6432=%s (pae=%s)\n",
         pa ? pa : "(null)", paew ? paew : "(null)", pae ? pae : "-");

    typedef BOOL (WINAPI *iswow2_t)(HANDLE, USHORT*, USHORT*);
    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    iswow2_t iswow2 = (iswow2_t)GetProcAddress(k32, "IsWow64Process2");
    if (iswow2)
    {
        USHORT proc = 0, native = 0;
        BOOL ok = iswow2(GetCurrentProcess(), &proc, &native);
        emit("[probe-T0] IsWow64Process2 ok=%d process=0x%04x native=0x%04x\n",
             (int)ok, (unsigned)proc, (unsigned)native);
    }
    else emit("[probe-T0] IsWow64Process2 missing\n");

    void* heap = HeapAlloc(GetProcessHeap(), 0, 1u << 20);
    emit("[probe-T0] HeapAlloc(1MB)=%p\n", heap);

    void* va = VirtualAlloc(NULL, 1u << 20, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    emit("[probe-T0] VirtualAlloc(1MB,RW)=%p err=%s\n", va, last_err());
    if (va)
    {
        memset(va, 0xA5, 1u << 20);
        DWORD old = 0;
        BOOL ok = VirtualProtect(va, 4096, PAGE_READONLY, &old);
        emit("[probe-T0] VirtualProtect(RW->RO) ok=%d old=0x%lx err=%s\n",
             (int)ok, (unsigned long)old, last_err());

        /* ---- T1 异常分发是否可用: 在独立线程里故意写只读页 ---- */
        StartFaultProbe(va);

        DWORD old2 = 0;
        VirtualProtect(va, 4096, PAGE_READWRITE, &old2);
    }

    /* ---- T2/T3/T4 steamclient64.dll 分层加载 ---- */
    const char* dll = "C:\\Program Files (x86)\\Steam\\steamclient64.dll";
    HMODULE h1 = LoadLibraryExA(dll, NULL, DONT_RESOLVE_DLL_REFERENCES);
    emit("[probe-T2] LoadLibraryEx(DONT_RESOLVE) = %p err=%s\n", h1, last_err());
    if (h1) FreeLibrary(h1);

    SetLastError(0);
    HMODULE h2 = LoadLibraryA(dll);
    emit("[probe-T3] LoadLibrary(full) = %p err=%s\n", h2, last_err());
    if (h2)
    {
        const char* names[] = { "CreateInterface", "GetSteamClient", "SteamClient" };
        for (int i = 0; i < 3; i++)
        {
            FARPROC p = GetProcAddress(h2, names[i]);
            emit("[probe-T4] GetProcAddress(%s) = %p err=%s\n", names[i], (void*)p, last_err());
        }
    }

    emit("[probe] done\n");
    if (g_log) { fputs("\n}\n", g_log); fclose(g_log); }
    return 0;
}
