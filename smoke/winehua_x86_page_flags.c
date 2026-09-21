/* A conditional branch in the next page must see DEC's zero flag. */
#include <windows.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    unsigned char *code;
    unsigned int (__cdecl *loop)(void);
    unsigned int result;
    DWORD old_protect;
    FILE *out = fopen("C:\\windows\\temp\\winehua-x86-page-flags.txt", "wb");
    static const unsigned char begin[] = {
        0xb9, 0x00, 0x01, 0x00, 0x00, /* mov ecx,256 */
        0x31, 0xc0,                   /* xor eax,eax */
        0x40,                         /* inc eax */
        0x49,                         /* dec ecx */
        0x89, 0xca                    /* mov edx,ecx */
    };
    if (!out) return 2;
    code = VirtualAlloc(NULL, 8192, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    if (!code) { fprintf(out, "FAIL allocation %lu\n", GetLastError()); fclose(out); return 3; }
    memset(code, 0x90, 8192);
    memcpy(code + 0xfe8, begin, sizeof(begin));
    code[0x1000] = 0x75; /* jnz 0xfef */
    code[0x1001] = 0xed;
    code[0x1002] = 0xc3;
    code[0x1100] = 0xc3;
    /* Build a block in page two, then invalidate it as self-modifying code.
     * WowBox64 retains readable page metadata but drops native EXEC here. */
    ((void (__cdecl *)(void))(code + 0x1100))();
    FlushInstructionCache(GetCurrentProcess(), code + 0x1000, 4096);
    /* Match legacy PE code pages executed by WowBox64 despite lacking NX
     * permission. The decoder stops when it reaches the next such page. */
    if (!VirtualProtect(code, 8192, PAGE_READONLY, &old_protect))
    {
        fprintf(out, "FAIL protection %lu\n", GetLastError());
        fclose(out);
        return 4;
    }
    FlushInstructionCache(GetCurrentProcess(), code, 8192);
    loop = (void *)(code + 0xfe8);
    fprintf(out, "START code=%p expected=256\n", code);
    fflush(out);
    result = loop();
    fprintf(out, "%s result=%u expected=256\n", result == 256 ? "PASS" : "FAIL", result);
    fclose(out);
    VirtualFree(code, 0, MEM_RELEASE);
    return result == 256 ? 0 : 1;
}
