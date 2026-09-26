/* winehua_t_mem_virtual — 虚拟内存执行页（RWX 链）与保护属性（P0）。
 * 判定规格见 docs/engineering/testing-programs.md §3.10。
 * 失败特征：执行崩 = noexec 匿名可执行映射链断（ohos_virtual 回归哨兵，
 * 加壳程序兼容的根基）。shellcode 两架构同字节：mov eax,imm32; ret。
 */
#include "../common/winehua_t_check.h"

static const unsigned char SHELLCODE[] = {
    0xB8, 0x78, 0x56, 0x34, 0x12, /* mov eax, 0x12345678 */
    0xC3                          /* ret */
};

typedef DWORD (*t_func)(void);

int main(int argc, char **argv)
{
    BYTE *page;
    MEMORY_BASIC_INFORMATION mbi;
    DWORD old_protect = 0;
    t_func fn;
    DWORD result;

    t_begin("winehua_t_mem_virtual", argc, argv);

    page = (BYTE *)VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE,
                                PAGE_READWRITE);
    t_check("alloc-rw", page != NULL, "err=%lu", page == NULL ? GetLastError() : 0);
    if (!page)
        return t_finish();

    memcpy(page, SHELLCODE, sizeof(SHELLCODE));
    t_check("write-plain", page[0] == SHELLCODE[0] && page[5] == SHELLCODE[5],
            "bytes after write");

    /* 修改为可执行：rw → rx，走 OHOS noexec 匿名映射链 */
    t_check("protect-execute", VirtualProtect(page, 4096, PAGE_EXECUTE_READ,
            &old_protect) != 0, "err=%lu", GetLastError());
    t_check("old-protect-rw", old_protect == PAGE_READWRITE,
            "old=0x%08lX", old_protect);

    /* 执行 */
    fn = (t_func)(void *)page;
    result = fn();
    t_check("execute-page", result == 0x12345678, "got 0x%08lX", result);

    /* VirtualQuery 核对属性与类型 */
    t_check("virtual-query", VirtualQuery(page, &mbi, sizeof(mbi)) == sizeof(mbi),
            "err=%lu", GetLastError());
    t_check("query-state-committed", mbi.State == MEM_COMMIT, "state=0x%08lX", mbi.State);
    t_check("query-protect-exec", mbi.Protect == PAGE_EXECUTE_READ,
            "protect=0x%08lX", mbi.Protect);
    t_check("query-type-private", mbi.Type == MEM_PRIVATE, "type=0x%08lX", mbi.Type);

    /* 换回 RW 后原写入仍可读 */
    t_check("protect-back", VirtualProtect(page, 4096, PAGE_READWRITE,
            &old_protect) != 0, "err=%lu", GetLastError());
    t_check("readback-after-roundtrip", page[1] == SHELLCODE[1], "bytes after protect roundtrip");

    TCHECK("virtual-free", (VirtualFree(page, 0, MEM_RELEASE)));
    return t_finish();
}
