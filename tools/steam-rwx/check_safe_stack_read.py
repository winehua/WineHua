"""Exercise the actual Wine diagnostic reader against real inaccessible mappings."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / "thirdparty/wine-valve/dlls/ntdll/unix/ohos_virtual.c").read_text()
begin = source.index("static size_t ohos_smc_read_stack(")
end = source.index("\n}\n", begin) + 3
helper = source[begin:end]
harness = r'''
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <unistd.h>
'''+helper+r'''
static uintptr_t guard_sp;
static volatile sig_atomic_t continued;
static void handler(int sig) {
    uintptr_t values[12];
    errno = EBUSY;
    assert(ohos_smc_read_stack(guard_sp, values, 12) == 0);
    assert(errno == EBUSY);
    continued = 1;
}
int main(void) {
    size_t page = sysconf(_SC_PAGESIZE);
    char *mem = mmap(0, page * 2, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    uintptr_t values[12];
    assert(mem != MAP_FAILED);
    *(uintptr_t *)mem = 0x12345678;
    *(uintptr_t *)(mem + page - sizeof(uintptr_t)) = 0x87654321;
    assert(mprotect(mem + page, page, PROT_NONE) == 0);
    errno = EDOM;
    assert(ohos_smc_read_stack((uintptr_t)mem, values, 12) == 12);
    assert(values[0] == 0x12345678 && errno == EDOM);
    assert(ohos_smc_read_stack((uintptr_t)(mem + page - sizeof(uintptr_t)), values, 12) == 1);
    assert(values[0] == 0x87654321);
    guard_sp = (uintptr_t)(mem + page);
    assert(signal(SIGUSR1, handler) != SIG_ERR);
    raise(SIGUSR1);
    assert(continued);
    assert(ohos_smc_read_stack(0, values, 12) == 0);
    assert(ohos_smc_read_stack(UINTPTR_MAX, values, 12) == 0);
    assert(munmap(mem, page * 2) == 0);
    assert(ohos_smc_read_stack((uintptr_t)mem, values, 12) == 0);
    puts("PASS: readable, partial, guard-page signal handler, null, invalid, unmapped, errno");
}
'''
with tempfile.TemporaryDirectory(prefix="winehua-safe-stack-") as tmp:
    src = Path(tmp) / "check.c"
    exe = Path(tmp) / "check"
    src.write_text(harness)
    subprocess.run(["cc", "-Wall", "-Wextra", "-Wno-unused-parameter", "-o", str(exe), str(src)], check=True)
    subprocess.run([str(exe)], check=True, timeout=10)
