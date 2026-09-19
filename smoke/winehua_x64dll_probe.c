/*
 * winehua_x64dll_probe.c — x64 用户态 DLL 加载分层探针
 *
 * 目的: 定位 "LoadLibrary(steamclient64.dll) 挂住" 到底是不是"任何 x64 DLL 都挂"。
 *   T0  基础 runtime (轻量)
 *   T2a LoadLibraryExA(bin\audio64.dll, DONT_RESOLVE_DLL_REFERENCES)   小 x64 DLL, 只映射
 *   T2b LoadLibraryA(bin\audio64.dll)                                   小 x64 DLL, 完整加载
 *   T2c LoadLibraryExA(steamclient64.dll, DONT_RESOLVE)                大 x64 DLL, 只映射
 *   T2d LoadLibraryA(steamclient64.dll)                                大 x64 DLL, 完整加载
 *   T3  GetProcAddress(steamclient64.dll, ...)
 *
 * 心跳线程每 2 秒打印一次当前 stage, 所以"卡在哪一步"在日志里直接可见。
 * 结果同时写 C:\smoke\results\x64dll-probe.json。
 */
#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

static FILE* g_log = NULL;
static volatile LONG g_stage = 0;
static volatile DWORD g_t0 = 0;

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

static DWORD WINAPI heartbeat_thread(void* param)
{
    (void)param;
    for (;;)
    {
        Sleep(2000);
        emit("[hb] stage=%ld elapsed=%lums\n", (long)InterlockedCompareExchange(&g_stage, 0, 0),
             (unsigned long)(GetTickCount() - g_t0));
    }
    return 0;
}

static void stage(int n, const char* what)
{
    InterlockedExchange(&g_stage, n);
    emit("[stage] %d %s t=%lums\n", n, what, (unsigned long)(GetTickCount() - g_t0));
}

static const char* err(void)
{
    static char buf[64];
    snprintf(buf, sizeof(buf), "%lu", (unsigned long)GetLastError());
    return buf;
}

int main(void)
{
    g_t0 = GetTickCount();
    CreateDirectoryA("C:\\smoke", NULL);
    CreateDirectoryA("C:\\smoke\\results", NULL);
    g_log = fopen("C:\\smoke\\results\\x64dll-probe.json", "w");
    if (g_log) { fputs("{\n", g_log); fflush(g_log); }

    CreateThread(NULL, 0, heartbeat_thread, NULL, 0, NULL);
    stage(0, "start");

    SYSTEM_INFO si;
    GetSystemInfo(&si);
    emit("[probe] arch=%u ptr=%u\n", (unsigned)si.wProcessorArchitecture, (unsigned)sizeof(void*));

    const char* small_dll  = "C:\\Program Files (x86)\\Steam\\bin\\audio64.dll";
    const char* small_dll2 = "C:\\Program Files (x86)\\Steam\\bin\\filesystem_stdio.dll";
    const char* big_dll    = "C:\\Program Files (x86)\\Steam\\steamclient64.dll";

    stage(2, "LoadLibraryEx(audio64, DONT_RESOLVE)");
    SetLastError(0);
    HMODULE h1 = LoadLibraryExA(small_dll, NULL, DONT_RESOLVE_DLL_REFERENCES);
    emit("[T2a] LoadLibraryEx(%.40s…, DONT_RESOLVE) = %p err=%s\n", small_dll, h1, err());
    if (h1) FreeLibrary(h1);
    stage(0, "idle after T2a");

    stage(3, "LoadLibrary(audio64, full)");
    SetLastError(0);
    HMODULE h2 = LoadLibraryA(small_dll);
    emit("[T2b] LoadLibrary(audio64, full) = %p err=%s\n", h2, err());
    if (h2) FreeLibrary(h2);
    stage(0, "idle after T2b");

    stage(4, "LoadLibraryEx(filesystem_stdio, DONT_RESOLVE)");
    SetLastError(0);
    HMODULE h3 = LoadLibraryExA(small_dll2, NULL, DONT_RESOLVE_DLL_REFERENCES);
    emit("[T2c] LoadLibraryEx(filesystem_stdio, DONT_RESOLVE) = %p err=%s\n", h3, err());
    if (h3) FreeLibrary(h3);
    stage(0, "idle after T2c");

    stage(5, "LoadLibraryEx(steamclient64, DONT_RESOLVE)");
    SetLastError(0);
    HMODULE h4 = LoadLibraryExA(big_dll, NULL, DONT_RESOLVE_DLL_REFERENCES);
    emit("[T2d] LoadLibraryEx(steamclient64, DONT_RESOLVE) = %p err=%s\n", h4, err());
    if (h4) FreeLibrary(h4);
    stage(0, "idle after T2d");

    stage(6, "LoadLibrary(steamclient64, full)");
    SetLastError(0);
    HMODULE h5 = LoadLibraryA(big_dll);
    emit("[T3] LoadLibrary(steamclient64, full) = %p err=%s\n", h5, err());
    if (h5)
    {
        const char* names[] = { "CreateInterface", "GetSteamClient" };
        for (int i = 0; i < 2; i++)
        {
            FARPROC p = GetProcAddress(h5, names[i]);
            emit("[T4] GetProcAddress(%s) = %p err=%s\n", names[i], (void*)p, err());
        }
    }
    stage(0, "done");
    emit("[probe] done\n");
    if (g_log) { fputs("\n}\n", g_log); fclose(g_log); }
    return 0;
}
