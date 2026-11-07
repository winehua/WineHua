"""Exercise the patched Wine loader's real overlay search and backend policy."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent


class WineDxvkOverlayTests(unittest.TestCase):
    def test_wow64_legacy_chain_and_opt_in_policy(self):
        with tempfile.TemporaryDirectory(prefix='winehua-overlay-') as temp:
            work = Path(temp)
            loader = work/'dlls/ntdll/loader.c'
            loader.parent.mkdir(parents=True)
            loader.write_text((ROOT/'thirdparty/wine/dlls/ntdll/loader.c').read_text(encoding='utf-8'), encoding='utf-8')
            subprocess.run(['git','init','-q',str(work)], check=True)
            patch_file = str(ROOT/'scripts/patches/wine-dxvk-legacy-overlay.patch')
            applied = subprocess.run(['git','-C',str(work),'apply','--reverse','--check',patch_file],
                stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL).returncode == 0
            if not applied:
                subprocess.run(['git','-C',str(work),'apply',patch_file],check=True)
            source = loader.read_text(encoding='utf-8')
            start = source.index('static NTSTATUS search_winehua_dxvk_overlay(')
            end = source.index('\n/***********************************************************************', start)
            function = source[start:end]
            harness = r'''
#include <assert.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
typedef wchar_t WCHAR;
typedef const WCHAR *LPCWSTR;
typedef unsigned short USHORT;
typedef size_t SIZE_T;
typedef int BOOL;
typedef int NTSTATUS;
typedef void *HANDLE;
typedef struct { USHORT Length, MaximumLength; WCHAR *Buffer; } UNICODE_STRING;
typedef struct { int unused; } WINE_MODREF;
typedef struct { int unused; } SECTION_IMAGE_INFORMATION;
struct file_id { int unused; };
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define STATUS_SUCCESS 0
#define STATUS_DLL_NOT_FOUND 1
#define STATUS_NOT_SUPPORTED 2
#define STATUS_NO_MEMORY 3
static const WCHAR *backend;
static unsigned int opens;
static WCHAR loaded[512];
static void *GetProcessHeap(void) { return NULL; }
static void *RtlAllocateHeap(void *heap, unsigned int flags, SIZE_T size) {
    (void)heap; (void)flags; return malloc(size);
}
static void RtlFreeHeap(void *heap, unsigned int flags, void *ptr) {
    (void)heap; (void)flags; free(ptr);
}
static void RtlFreeUnicodeString(UNICODE_STRING *str) { free(str->Buffer); }
static int get_env_var(const WCHAR *name, unsigned int extra, UNICODE_STRING *out) {
    (void)extra;
    const WCHAR *value = !wcscmp(name,L"WINEHUA_D3D_BACKEND") ? backend :
        !wcscmp(name,L"WINEHUA_VKD3D_ROOT") ? L"/vkd3d" : L"/dxvk";
    out->Length = (USHORT)(wcslen(value)*sizeof(WCHAR));
    out->MaximumLength = out->Length+sizeof(WCHAR);
    out->Buffer = malloc(out->MaximumLength);
    assert(out->Buffer);
    wcscpy(out->Buffer,value);
    return 0;
}
static NTSTATUS open_dll_file(UNICODE_STRING *name, WINE_MODREF **pwm, HANDLE *mapping,
                             SECTION_IMAGE_INFORMATION *info, struct file_id *id) {
    (void)pwm; (void)mapping; (void)info; (void)id;
    ++opens;
    // Model a WoW64 process: skip the existing incompatible x64 DLL, then
    // accept the x86 DLL. No fallback to prefix copies is available.
    if (wcsstr(name->Buffer,L"\\x64\\")) return STATUS_NOT_SUPPORTED;
    assert(wcsstr(name->Buffer,L"\\x86\\"));
    wcscpy(loaded,name->Buffer);
    return STATUS_SUCCESS;
}
''' + function + r'''
static void check(const WCHAR *profile, const WCHAR *dll, int allowed, const WCHAR *root) {
    backend = profile;
    opens = 0;
    loaded[0] = 0;
    UNICODE_STRING name = {0};
    NTSTATUS result = search_winehua_dxvk_overlay(dll,&name,NULL,NULL,NULL,NULL);
    if (allowed) {
        assert(result == STATUS_SUCCESS && opens == 2);
        assert(wcsstr(loaded,root) && wcsstr(loaded,dll));
        free(name.Buffer);
    } else assert(result == STATUS_DLL_NOT_FOUND && opens == 0);
}
int main(void) {
    check(L"dxvk_legacy_d3d9",L"d3d9.dll",1,L"/dxvk");
    check(L"dxvk_legacy",L"d3d9.dll",0,L"/dxvk");
    check(L"dxvk_modern_2_6",L"d3d9.dll",0,L"/dxvk");
    check(L"wined3d",L"d3d9.dll",0,L"/dxvk");
    const WCHAR *chain[] = {L"d3d10.dll",L"d3d10_1.dll",L"d3d10core.dll"};
    for (unsigned int i=0;i<ARRAY_SIZE(chain);++i) {
        check(L"dxvk_legacy",chain[i],1,L"/dxvk");
        check(L"dxvk_legacy_d3d9",chain[i],1,L"/dxvk");
        check(L"dxvk_modern_2_6",chain[i],0,L"/dxvk");
        check(L"wined3d",chain[i],0,L"/dxvk");
    }
    check(L"dxvk_legacy",L"d3d11.dll",1,L"/dxvk");
    check(L"dxvk_modern_2_6",L"dxgi.dll",1,L"/dxvk");
    check(L"vkd3d_limited_500k",L"d3d12.dll",1,L"/vkd3d");
    check(L"dxvk_legacy_d3d9",L"d3d12.dll",0,L"/vkd3d");
}
'''
            c = work/'overlay.c'
            c.write_text(harness, encoding='utf-8')
            exe = work/('overlay.exe' if os.name == 'nt' else 'overlay')
            compiler = os.environ.get('CC') or shutil.which('cc') or shutil.which('gcc')
            self.assertIsNotNone(compiler, 'C compiler required')
            subprocess.run([compiler,'-std=c11','-Wall','-Wextra','-Werror',str(c),'-o',str(exe)],check=True)
            subprocess.run([str(exe)],check=True)


if __name__ == '__main__':
    unittest.main()
