"""Exercise Wine's TLS clear routine with live and unmapped thread entries."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / "thirdparty/wine-valve/dlls/ntdll/unix/virtual.c").read_text()
begin = source.index("NTSTATUS virtual_clear_tls_index(")
end = source.index("\n}\n", begin) + 3
helper = source[begin:end]
harness = r'''
#define _GNU_SOURCE
#define _WIN64
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include "wine/list.h"
typedef uint32_t ULONG;
typedef int NTSTATUS;
#define TLS_MINIMUM_AVAILABLE 64
#define STATUS_SUCCESS 0
#define STATUS_INVALID_PARAMETER ((int)0xc000000d)
#define CONTAINING_RECORD(p,t,f) ((t *)((char *)(p) - offsetof(t,f)))
#define ULongToPtr(p) ((void *)(uintptr_t)(p))
struct ntdll_thread_data { struct list entry; };
typedef struct { ULONG TlsSlots[64]; uintptr_t TlsExpansionSlots; } WOW_TEB;
typedef struct {
    struct ntdll_thread_data GdiTebBatch;
    uintptr_t TlsSlots[64];
    uintptr_t *TlsExpansionSlots;
    WOW_TEB *wow;
} TEB;
static WOW_TEB *get_wow_teb(TEB *teb) { return teb->wow; }
static struct { ULONG TlsExpansionBitmapBits[32]; } peb_store, *peb = &peb_store;
static struct list teb_list = LIST_INIT(teb_list);
static int process_exiting, virtual_mutex;
static void server_enter_uninterrupted_section(int *m, sigset_t *s) { ++*m; }
static void server_leave_uninterrupted_section(int *m, sigset_t *s) { --*m; }
static void ohos_teb_probe(const char *e, void *p, size_t s, ULONG v) {}
''' + helper + r'''
int main(void) {
    TEB *teb = mmap(NULL, 4096, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    assert(teb != MAP_FAILED);
    list_add_head(&teb_list, &teb->GdiTebBatch.entry);
    uintptr_t slots[1024] = {0};
    teb->TlsSlots[10] = 123;
    teb->TlsSlots[11] = 456;
    teb->TlsExpansionSlots = slots;
    slots[1023] = 789;
    assert(virtual_clear_tls_index(10) == 0 && teb->TlsSlots[10] == 0);
    assert(teb->TlsSlots[11] == 456);
    assert(virtual_clear_tls_index(1087) == 0 && slots[1023] == 0);
    WOW_TEB wow = {0};
    wow.TlsSlots[10] = 99;
    teb->wow = &wow;
    assert(virtual_clear_tls_index(10) == 0 && wow.TlsSlots[10] == 0);
    assert(virtual_mutex == 0);
    /* Match the observed process-detach failure: the list entry survives
       while the terminated thread's memory has become inaccessible. */
    assert(munmap(teb, 4096) == 0);
    process_exiting = 1;
    assert(virtual_clear_tls_index(10) == 0);
    assert(virtual_clear_tls_index(1087) == 0);
    assert(virtual_clear_tls_index(1088) == STATUS_INVALID_PARAMETER);
    assert(virtual_clear_tls_index(UINT32_MAX) == STATUS_INVALID_PARAMETER);
    assert(virtual_mutex == 0);
    puts("PASS: live native/WOW TLS, expansion slots, shutdown with unmapped TEB, invalid indices");
}
'''
with tempfile.TemporaryDirectory() as tmp:
    path = Path(tmp)
    (path / "test.c").write_text(harness)
    subprocess.run(["cc", "-O2", "-I", str(root / "thirdparty/wine-valve/include"),
                    str(path / "test.c"), "-o", str(path / "test")], check=True)
    subprocess.run([str(path / "test")], check=True)
