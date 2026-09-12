/*
 * comprobe.exe — narrow down the rpcrt4 access violation seen with the official
 * x86-64 Steam client on WineHua/OHOS.
 *
 * Evidence being chased (2026-09-12):
 *   Steam's crash dump: exceptionCode 0xC0000005 at rpcrt4.dll+0x64aac,
 *   faulting access = write to 0x10, called from steamui/stechost path.
 *   Steam's stderr log also shows "Failed to load Steam Service (GLE 126)".
 *
 * Method: every step writes a marker and flushes BEFORE calling, so the last
 * marker in C:\windows\temp\comprobe.txt identifies the crashing call even if
 * the process dies inside it.
 *
 * Build:
 *   x86_64-w64-mingw32-gcc -O1 -municode -mwindows -o comprobe.exe comprobe.c \
 *       -lole32 -loleaut32 -lrpcrt4 -ladvapi32 -lshlwapi
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <shlobj.h>
#include <rpc.h>
#include <stdio.h>
#include <stdarg.h>

static FILE *g_out;

static void mark(const char *fmt, ...)
{
    va_list ap;
    if (!g_out) return;
    va_start(ap, fmt);
    vfprintf(g_out, fmt, ap);
    va_end(ap);
    fputc('\n', g_out);
    fflush(g_out);
}

static void hr_step(const char *name, HRESULT hr)
{
    mark("RESULT %-40s hr=0x%08lX", name, (unsigned long)hr);
}

static int run(void)
{
    HRESULT hr;
    DWORD err;
    UUID uuid;
    RPC_WSTR sb = NULL;
    RPC_BINDING_HANDLE binding = NULL;

    mark("comprobe build %s %s", __DATE__, __TIME__);

    mark("STEP 1 CoInitializeEx");
    hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    hr_step("CoInitializeEx", hr);

    mark("STEP 2 CoCreateInstance(CLSID_ShellLink)");
    {
        IShellLinkW *link = NULL;
        static const CLSID clsid_shelllink =
            { 0x00021401, 0x0000, 0x0000, { 0xC0,0x00,0x00,0x00,0x00,0x00,0x00,0x46 } };
        hr = CoCreateInstance(&clsid_shelllink, NULL, CLSCTX_INPROC_SERVER,
                              &IID_IShellLinkW, (void **)&link);
        hr_step("CoCreateInstance(ShellLink)", hr);
        if (link) { mark("  link=%p", (void *)link); link->lpVtbl->Release(link); }
    }

    mark("STEP 3 CoCreateInstance(CLSID_FileOpenDialog)");
    {
        IFileOpenDialog *dlg = NULL;
        static const CLSID clsid_fod =
            { 0xDC1C5A9C, 0xE88A, 0x4DDE, { 0xA5,0xA1,0x60,0xF8,0x2A,0x20,0xAE,0xF7 } };
        hr = CoCreateInstance(&clsid_fod, NULL, CLSCTX_INPROC_SERVER,
                              &IID_IFileOpenDialog, (void **)&dlg);
        hr_step("CoCreateInstance(FileOpenDialog)", hr);
        if (dlg) { mark("  dlg=%p", (void *)dlg); dlg->lpVtbl->Release(dlg); }
    }

    mark("STEP 4 CoCreateInstance(CLSID_StdGlobalInterfaceTable)");
    {
        IGlobalInterfaceTable *git = NULL;
        static const CLSID clsid_git =
            { 0x00000323, 0x0000, 0x0000, { 0xC0,0x00,0x00,0x00,0x00,0x00,0x00,0x46 } };
        static const IID iid_git =
            { 0x00000146, 0x0000, 0x0000, { 0xC0,0x00,0x00,0x00,0x00,0x00,0x00,0x46 } };
        hr = CoCreateInstance(&clsid_git, NULL, CLSCTX_INPROC_SERVER,
                              &iid_git, (void **)&git);
        hr_step("CoCreateInstance(StdGlobalInterfaceTable)", hr);
        if (git) { mark("  git=%p", (void *)git); ((IUnknown *)git)->lpVtbl->Release((IUnknown *)git); }
    }

    mark("STEP 5 UuidCreate");
    {
        RPC_STATUS rs = UuidCreate(&uuid);
        mark("RESULT %-40s rs=%ld", "UuidCreate", (long)rs);
    }

    mark("STEP 6 RpcStringBindingComposeW(ncacn_np \\pipe\\steam)");
    {
        RPC_STATUS rs = RpcStringBindingComposeW(NULL, (RPC_WSTR)L"ncacn_np", NULL,
                                                 (RPC_WSTR)L"\\pipe\\steam", NULL, &sb);
        mark("RESULT %-40s rs=%ld sb=%p", "RpcStringBindingComposeW", (long)rs, (void *)sb);
        if (sb) mark("  binding string: %ls", sb);
    }

    mark("STEP 7 RpcBindingFromStringBindingW");
    if (sb) {
        RPC_STATUS rs = RpcBindingFromStringBindingW(sb, &binding);
        mark("RESULT %-40s rs=%ld", "RpcBindingFromStringBindingW", (long)rs);
    }

    mark("STEP 8 RpcBindingFree");
    if (binding) {
        RPC_STATUS rs = RpcBindingFree(&binding);
        mark("RESULT %-40s rs=%ld", "RpcBindingFree", (long)rs);
    }
    if (sb) RpcStringFreeW(&sb);

    mark("STEP 9 OpenSCManagerW(L\"\", L\"\") - non-NULL strings");
    {
        SC_HANDLE scm = OpenSCManagerW(L"", L"", SC_MANAGER_CONNECT | SC_MANAGER_ENUMERATE_SERVICE);
        err = GetLastError();
        mark("RESULT %-40s handle=%p gle=%lu", "OpenSCManagerW(\"\",\"\")", (void *)scm, (unsigned long)err);
        if (scm) {
            mark("STEP 10 OpenServiceW(Steam Client Service)");
            {
                SC_HANDLE svc = OpenServiceW(scm, L"Steam Client Service", SERVICE_QUERY_STATUS);
                DWORD e2 = GetLastError();
                mark("RESULT %-40s handle=%p gle=%lu", "OpenServiceW(Steam)", (void *)svc, (unsigned long)e2);
                if (svc) CloseServiceHandle(svc);
            }
            mark("STEP 11 EnumServicesStatusExW");
            {
                DWORD needed = 0, returned = 0;
                BOOL ok = EnumServicesStatusExW(scm, SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_STATE_ALL,
                                                NULL, 0, &needed, &returned, NULL, NULL);
                mark("RESULT %-40s ok=%d needed=%lu gle=%lu", "EnumServicesStatusExW(probe)",
                     ok, (unsigned long)needed, (unsigned long)GetLastError());
            }
            CloseServiceHandle(scm);
        }
    }

    mark("STEP 12 CreateFileW(\\\\.\\pipe\\steamservice_sdk)");
    {
        HANDLE h = CreateFileW(L"\\\\.\\pipe\\steamservice_sdk", GENERIC_READ | GENERIC_WRITE,
                               0, NULL, OPEN_EXISTING, 0, NULL);
        mark("RESULT %-40s handle=%p gle=%lu", "CreateFileW(pipe)",
             (void *)h, (unsigned long)GetLastError());
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    }

    mark("STEP 13 CoMarshalInterface(ShellLink -> stream)");
    {
        IShellLinkW *link = NULL;
        IStream *stream = NULL;
        static const CLSID clsid_shelllink =
            { 0x00021401, 0x0000, 0x0000, { 0xC0,0x00,0x00,0x00,0x00,0x00,0x00,0x46 } };
        static const IID iid_iunk =
            { 0x00000000, 0x0000, 0x0000, { 0xC0,0x00,0x00,0x00,0x00,0x00,0x00,0x46 } };
        hr = CoCreateInstance(&clsid_shelllink, NULL, CLSCTX_INPROC_SERVER,
                              &IID_IShellLinkW, (void **)&link);
        if (SUCCEEDED(hr) && link) {
            hr = CreateStreamOnHGlobal(NULL, TRUE, &stream);
            mark("  CreateStreamOnHGlobal hr=0x%08lX", (unsigned long)hr);
            if (SUCCEEDED(hr)) {
                hr = CoMarshalInterface(stream, &iid_iunk, (IUnknown *)link,
                                        MSHCTX_INPROC, NULL, MSHLFLAGS_TABLESTRONG);
                hr_step("CoMarshalInterface", hr);
                stream->lpVtbl->Release(stream);
            }
            link->lpVtbl->Release(link);
        }
    }

    mark("STEP 14 LoadLibrary(SteamService.dll)");
    {
        HMODULE m = LoadLibraryW(L"C:\\Program Files (x86)\\Steam\\bin\\SteamService.dll");
        mark("RESULT %-40s module=%p gle=%lu", "LoadLibraryW(SteamService.dll)",
             (void *)m, (unsigned long)GetLastError());
    }

    mark("STEP 15 LoadLibrary(steamclient64.dll)");
    {
        HMODULE m = LoadLibraryW(L"C:\\Program Files (x86)\\Steam\\steamclient64.dll");
        mark("RESULT %-40s module=%p gle=%lu", "LoadLibraryW(steamclient64.dll)",
             (void *)m, (unsigned long)GetLastError());
    }

    mark("STEP 16 CoUninitialize");
    CoUninitialize();

    /* Deliberately LAST: this is the exact call that crashes (NULL, NULL).
     * If everything above succeeded and we only die here, the trigger is the
     * NULL [in,unique] LPCWSTR marshalling in rpcrt4's stubless client proxy. */
    mark("STEP 17 (last, expected crash) OpenSCManagerW(NULL, NULL, ...)");
    {
        SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
        mark("RESULT %-40s handle=%p gle=%lu", "OpenSCManagerW(NULL,NULL)",
             (void *)scm, (unsigned long)GetLastError());
        if (scm) CloseServiceHandle(scm);
    }

    mark("DONE - all steps completed");
    return 0;
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE hPrev, LPWSTR cmd, int show)
{
    (void)hInst; (void)hPrev; (void)cmd; (void)show;

    g_out = fopen("C:\\windows\\temp\\comprobe.txt", "w");
    if (!g_out) g_out = fopen("C:\\comprobe.txt", "w");
    if (!g_out) g_out = fopen("Z:\\comprobe.txt", "w");
    if (!g_out) return 1;

    run();

    fclose(g_out);
    {
        FILE *src = fopen("C:\\windows\\temp\\comprobe.txt", "rb");
        FILE *dst = fopen("Z:\\comprobe.txt", "wb");
        if (src && dst) {
            char buf[4096];
            size_t n;
            while ((n = fread(buf, 1, sizeof(buf), src)) > 0) fwrite(buf, 1, n, dst);
        }
        if (src) fclose(src);
        if (dst) fclose(dst);
    }
    return 0;
}
