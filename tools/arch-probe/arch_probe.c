/* WineHua P0-2A — Windows architecture semantics probe (test only, no behaviour change).
 *
 * Builds as both a 32-bit (i386) and 64-bit (x86_64) Windows PE and prints the full
 * architecture surface Wine exposes: PE machine, GetSystemInfo/GetNativeSystemInfo,
 * WOW64 APIs, environment, system-directory views, registry values and the loaded
 * core DLLs.  Also writes a JSON copy next to the executable:
 *
 *   C:\smoke\results\arch-probe-<arch>.json
 *
 * Compile:
 *   i686-w64-mingw32-gcc   -O2 -o arch_probe_x86.exe arch_probe.c
 *   x86_64-w64-mingw32-gcc -O2 -o arch_probe_x64.exe arch_probe.c
 */

#include <windows.h>
#include <winternl.h>
#include <stdio.h>
#include <string.h>

#ifndef _WIN64
#define PROBE_ARCH "x86"
#else
#define PROBE_ARCH "x64"
#endif

/* ---- optional / newer APIs, resolved dynamically ---- */
typedef BOOL (WINAPI *pIsWow64Process2)(HANDLE, USHORT *, USHORT *);
typedef UINT (WINAPI *pGetSystemWow64Directory2W)(LPWSTR, UINT, USHORT);
typedef BOOL (WINAPI *pGetMachineTypeAttributes)(USHORT, int *);

#ifndef ProcessMachineTypeInfo
#define ProcessMachineTypeInfo 9
#endif

static FILE *g_json;

static const char *machine_name(USHORT machine)
{
    switch (machine) {
    case 0x0000: return "UNKNOWN";
    case 0x014c: return "I386";
    case 0x8664: return "AMD64";
    case 0xaa64: return "ARM64";
    case 0xa641: return "ARM64EC";
    case 0x01c4: return "ARMNT";
    default:     return "OTHER";
    }
}

/* SYSTEM_INFO.wProcessorArchitecture uses PROCESSOR_ARCHITECTURE_* (not IMAGE_FILE_MACHINE_*). */
static const char *proc_arch_name(WORD arch)
{
    switch (arch) {
    case 0:  return "INTEL/x86";
    case 5:  return "ARM";
    case 6:  return "IA64";
    case 9:  return "AMD64";
    case 12: return "ARM64";
    default: return "UNKNOWN";
    }
}

static void out(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    fflush(stdout);
}

static void jstr(const char *key, const char *value)
{
    if (g_json) fprintf(g_json, "  \"%s\": \"%s\",\n", key, value ? value : "");
}

static void jnum(const char *key, long long value)
{
    if (g_json) fprintf(g_json, "  \"%s\": %lld,\n", key, value);
}

static void wide_to_ascii(const WCHAR *w, char *out, size_t out_size)
{
    size_t i = 0;
    if (!w || !out || !out_size) return;
    for (; w[i] && i + 1 < out_size; i++)
        out[i] = (w[i] >= 32 && w[i] < 127) ? (char)w[i] : '?';
    out[i] = 0;
}

static USHORT module_machine(HMODULE mod)
{
    IMAGE_DOS_HEADER *dos;
    IMAGE_NT_HEADERS *nt;
    if (!mod) return 0;
    dos = (IMAGE_DOS_HEADER *)mod;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    nt = (IMAGE_NT_HEADERS *)((BYTE *)mod + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
    return nt->FileHeader.Machine;
}

static void dump_module(const char *name)
{
    HMODULE mod = GetModuleHandleA(name);
    char path[512] = "(not loaded)";
    USHORT machine = 0;
    unsigned long long base = 0, size = 0;
    IMAGE_DOS_HEADER *dos;

    if (mod) {
        GetModuleFileNameA(mod, path, sizeof(path));
        machine = module_machine(mod);
        base = (unsigned long long)(ULONG_PTR)mod;
        dos = (IMAGE_DOS_HEADER *)mod;
        if (dos->e_magic == IMAGE_DOS_SIGNATURE) {
            IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)((BYTE *)mod + dos->e_lfanew);
            size = nt->OptionalHeader.SizeOfImage;
        }
    }
    out("  %-14s machine=%-8s base=0x%llx size=0x%llx path=%s\n",
        name, mod ? machine_name(machine) : "-", base, size, path);
    if (g_json)
        fprintf(g_json,
                "  \"module_%s\": {\"loaded\": %d, \"machine\": \"%s\", \"base\": %llu, "
                "\"size\": %llu, \"path\": \"%s\"},\n",
                name, mod ? 1 : 0, mod ? machine_name(machine) : "NOT_LOADED",
                base, size, path);
}

static void dump_sysinfo(void)
{
    SYSTEM_INFO si, nsi;
    ZeroMemory(&si, sizeof(si));
    ZeroMemory(&nsi, sizeof(nsi));
    GetSystemInfo(&si);
    GetNativeSystemInfo(&nsi);

    out("GetSystemInfo:       arch=%s(%#x) type=%lu cores=%lu pagesize=%lu min=%p max=%p\n",
        proc_arch_name(si.wProcessorArchitecture), si.wProcessorArchitecture,
        si.dwProcessorType, si.dwNumberOfProcessors, si.dwPageSize,
        si.lpMinimumApplicationAddress, si.lpMaximumApplicationAddress);
    out("GetNativeSystemInfo: arch=%s(%#x) type=%lu cores=%lu pagesize=%lu min=%p max=%p\n",
        proc_arch_name(nsi.wProcessorArchitecture), nsi.wProcessorArchitecture,
        nsi.dwProcessorType, nsi.dwNumberOfProcessors, nsi.dwPageSize,
        nsi.lpMinimumApplicationAddress, nsi.lpMaximumApplicationAddress);

    if (g_json) {
        fprintf(g_json, "  \"GetSystemInfo\": {\"architecture\": \"%s\", \"raw\": %u, "
                        "\"cores\": %lu, \"page_size\": %lu},\n",
                proc_arch_name(si.wProcessorArchitecture), si.wProcessorArchitecture,
                si.dwNumberOfProcessors, si.dwPageSize);
        fprintf(g_json, "  \"GetNativeSystemInfo\": {\"architecture\": \"%s\", \"raw\": %u, "
                        "\"cores\": %lu, \"page_size\": %lu},\n",
                proc_arch_name(nsi.wProcessorArchitecture), nsi.wProcessorArchitecture,
                nsi.dwNumberOfProcessors, nsi.dwPageSize);
    }
}

static void dump_wow64(void)
{
    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    pIsWow64Process2 is2 = (pIsWow64Process2)(void *)GetProcAddress(k32, "IsWow64Process2");
    pGetSystemWow64Directory2W wow2 =
        (pGetSystemWow64Directory2W)(void *)GetProcAddress(k32, "GetSystemWow64Directory2W");
    HMODULE kb = GetModuleHandleA("kernelbase.dll");
    pGetMachineTypeAttributes gma =
        (pGetMachineTypeAttributes)(void *)(GetProcAddress(k32, "GetMachineTypeAttributes")
                                            ? GetProcAddress(k32, "GetMachineTypeAttributes")
                                            : GetProcAddress(kb, "GetMachineTypeAttributes"));
    BOOL wow = FALSE;
    USHORT process_machine = 0, native_machine = 0;
    BOOL is2_ok = FALSE;
    DWORD err = 0;
    WCHAR dir[512];

    if (IsWow64Process(GetCurrentProcess(), &wow)) {
        out("IsWow64Process:      %s\n", wow ? "TRUE" : "FALSE");
    } else {
        err = GetLastError();
        out("IsWow64Process:      FAILED err=%lu\n", err);
    }

    if (is2) {
        is2_ok = is2(GetCurrentProcess(), &process_machine, &native_machine);
        if (is2_ok)
            out("IsWow64Process2:     ProcessMachine=%s(%#x) NativeMachine=%s(%#x)\n",
                machine_name(process_machine), process_machine,
                machine_name(native_machine), native_machine);
        else
            out("IsWow64Process2:     FAILED err=%lu\n", GetLastError());
    } else {
        out("IsWow64Process2:     NOT_IMPLEMENTED\n");
    }

    if (wow2) {
        UINT n = wow2(dir, ARRAYSIZE(dir), IMAGE_FILE_MACHINE_AMD64);
        if (n && n < ARRAYSIZE(dir)) {
            char ascii[512];
            wide_to_ascii(dir, ascii, sizeof(ascii));
            out("GetSystemWow64Directory2W(AMD64): %s\n", ascii);
        } else {
            out("GetSystemWow64Directory2W(AMD64): FAILED err=%lu\n", GetLastError());
        }
    } else {
        out("GetSystemWow64Directory2W: NOT_IMPLEMENTED\n");
    }

    if (gma) {
        const USHORT machines[] = {IMAGE_FILE_MACHINE_I386, IMAGE_FILE_MACHINE_AMD64,
                                   IMAGE_FILE_MACHINE_ARM64, 0xa641 /* ARM64EC */};
        const char *names[] = {"I386", "AMD64", "ARM64", "ARM64EC"};
        int i;
        for (i = 0; i < 4; i++) {
            int attrs = 0;
            BOOL ok = gma(machines[i], &attrs);
            out("GetMachineTypeAttributes(%-7s): %s attrs=%#x\n", names[i],
                ok ? "OK" : "FAILED", attrs);
        }
    } else {
        out("GetMachineTypeAttributes: NOT_IMPLEMENTED\n");
    }

    if (g_json) {
        fprintf(g_json,
                "  \"IsWow64Process\": %s,\n"
                "  \"IsWow64Process2\": {\"available\": %d, \"ProcessMachine\": \"%s\", "
                "\"ProcessMachineRaw\": %u, \"NativeMachine\": \"%s\", \"NativeMachineRaw\": %u},\n"
                "  \"GetSystemWow64Directory2W_available\": %d,\n"
                "  \"GetMachineTypeAttributes_available\": %d,\n",
                wow ? "true" : "false", is2 ? 1 : 0,
                is2_ok ? machine_name(process_machine) : "UNAVAILABLE", process_machine,
                is2_ok ? machine_name(native_machine) : "UNAVAILABLE", native_machine,
                wow2 ? 1 : 0, gma ? 1 : 0);
    }
}

static void dump_env(void)
{
    static const char *names[] = {
        "PROCESSOR_ARCHITECTURE", "PROCESSOR_ARCHITEW6432", "PROCESSOR_IDENTIFIER",
        "ProgramFiles", "ProgramFiles(x86)", "ProgramW6432",
        "CommonProgramFiles", "CommonProgramFiles(x86)", "CommonProgramW6432",
        "SystemRoot", "windir",
    };
    size_t i;
    out("-- environment --\n");
    if (g_json) fprintf(g_json, "  \"environment\": {\n");
    for (i = 0; i < ARRAYSIZE(names); i++) {
        char value[1024];
        DWORD n = GetEnvironmentVariableA(names[i], value, sizeof(value));
        if (n == 0) {
            out("  %-24s (unset)\n", names[i]);
            if (g_json) fprintf(g_json, "    \"%s\": null,\n", names[i]);
        } else {
            out("  %-24s %s\n", names[i], value);
            if (g_json) fprintf(g_json, "    \"%s\": \"%s\",\n", names[i], value);
        }
    }
    if (g_json) fprintf(g_json, "  },\n");
}

static void dump_dirs(void)
{
    WCHAR buf[512];
    char ascii[512];
    struct { const char *name; UINT (WINAPI *fn)(LPWSTR, UINT); } dirs[] = {
        {"GetSystemDirectoryW", GetSystemDirectoryW},
        {"GetWindowsDirectoryW", GetWindowsDirectoryW},
        {"GetSystemWindowsDirectoryW", GetSystemWindowsDirectoryW},
        {"GetSystemWow64DirectoryW", GetSystemWow64DirectoryW},
    };
    size_t i;
    out("-- system directories --\n");
    if (g_json) fprintf(g_json, "  \"directories\": {\n");
    for (i = 0; i < ARRAYSIZE(dirs); i++) {
        if (!dirs[i].fn) continue;
        buf[0] = 0;
        UINT n = dirs[i].fn(buf, ARRAYSIZE(buf));
        if (n && n < ARRAYSIZE(buf)) {
            wide_to_ascii(buf, ascii, sizeof(ascii));
            out("  %-28s %s\n", dirs[i].name, ascii);
            if (g_json) fprintf(g_json, "    \"%s\": \"%s\",\n", dirs[i].name, ascii);
        } else {
            out("  %-28s FAILED err=%lu\n", dirs[i].name, GetLastError());
            if (g_json) fprintf(g_json, "    \"%s\": \"FAILED(%lu)\",\n", dirs[i].name, GetLastError());
        }
    }
    if (g_json) fprintf(g_json, "  },\n");
}

static void dump_registry(void)
{
    static const struct { const char *path, *value; } keys[] = {
        {"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", "ProcessorNameString"},
        {"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", "Identifier"},
        {"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment", "PROCESSOR_ARCHITECTURE"},
        {"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment", "ProgramFilesDir"},
        {"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment", "ProgramFilesDir (x86)"},
        {"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment", "ProgramW6432"},
    };
    size_t i;
    out("-- registry --\n");
    if (g_json) fprintf(g_json, "  \"registry\": {\n");
    for (i = 0; i < ARRAYSIZE(keys); i++) {
        HKEY key;
        char value[1024];
        DWORD size = sizeof(value), type = 0;
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, keys[i].path, 0, KEY_READ, &key) != ERROR_SUCCESS) {
            out("  %s\\%s (open failed)\n", keys[i].path, keys[i].value);
            if (g_json) fprintf(g_json, "    \"%s|%s\": \"OPEN_FAILED\",\n", keys[i].path, keys[i].value);
            continue;
        }
        if (RegQueryValueExA(key, keys[i].value, NULL, &type, (LPBYTE)value, &size) == ERROR_SUCCESS) {
            out("  %s\\%s = %s\n", keys[i].path, keys[i].value, value);
            if (g_json) fprintf(g_json, "    \"%s|%s\": \"%s\",\n", keys[i].path, keys[i].value, value);
        } else {
            out("  %s\\%s (missing)\n", keys[i].path, keys[i].value);
            if (g_json) fprintf(g_json, "    \"%s|%s\": null,\n", keys[i].path, keys[i].value);
        }
        RegCloseKey(key);
    }
    if (g_json) fprintf(g_json, "  },\n");
}

int main(void)
{
    char json_path[512];
    char exe_path[512] = "";
    HMODULE self = GetModuleHandleA(NULL);
    SYSTEM_INFO si;

    CreateDirectoryA("C:\\smoke", NULL);
    CreateDirectoryA("C:\\smoke\\results", NULL);
    snprintf(json_path, sizeof(json_path), "C:\\smoke\\results\\arch-probe-%s.json", PROBE_ARCH);

    out("=== WineHua arch probe (%s) ===\n", PROBE_ARCH);
    GetModuleFileNameA(self, exe_path, sizeof(exe_path));
    out("exe:                 %s\n", exe_path);
    out("PE machine (self):   %s(%#x)  sizeof(void*)=%u\n",
        machine_name(module_machine(self)), module_machine(self), (unsigned)sizeof(void *));

    ZeroMemory(&si, sizeof(si));
    GetSystemInfo(&si);

    g_json = fopen(json_path, "w");
    if (g_json) {
        fprintf(g_json, "{\n");
        fprintf(g_json, "  \"probe_arch\": \"%s\",\n", PROBE_ARCH);
        fprintf(g_json, "  \"exe\": \"%s\",\n", exe_path);
        fprintf(g_json, "  \"pe_machine\": \"%s\",\n", machine_name(module_machine(self)));
        fprintf(g_json, "  \"pe_machine_raw\": %u,\n", module_machine(self));
        fprintf(g_json, "  \"pointer_bits\": %u,\n", (unsigned)(sizeof(void *) * 8));
    }

    dump_sysinfo();
    dump_wow64();
    dump_env();
    dump_dirs();
    dump_registry();

    out("-- loaded core modules --\n");
    if (g_json) fprintf(g_json, "  \"modules\": [\n");
    dump_module("ntdll.dll");
    dump_module("kernel32.dll");
    dump_module("kernelbase.dll");
    dump_module("wow64.dll");
    dump_module("wow64win.dll");
    dump_module("wow64cpu.dll");
    dump_module("libarm64ecfex.dll");
    dump_module("libwow64fex.dll");
    dump_module("wowbox64.dll");
    if (g_json) fprintf(g_json, "    {}\n  ]\n}\n");

    out("JSON written to: %s\n", g_json ? json_path : "(open failed)");
    if (g_json) fclose(g_json);
    return 0;
}
