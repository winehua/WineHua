/*
 * varargprobe.exe — minimal check of the x64 -> ARM64EC variadic call convention.
 *
 * Hypothesis being tested (see ../../ohos-rpc-stubless-arm64ec-crash.md):
 *   FEX's ARM64EC dispatcher leaves x4 = x64 RSP + 8, while Wine's ARM64EC
 *   variadic entry code expects the x64 stack arguments to be found relative to
 *   x4 with the 32-byte shadow space skipped. If so, any variadic call made from
 *   x86-64 code with more than 2 stack-passed varargs should read garbage.
 *
 * x86-64 ABI: buf=rcx, fmt=rdx, then vararg1=r8, vararg2=r9, vararg3.. on stack.
 * So with 8 varargs, the first two come from registers and the rest from the stack.
 * If the harness is correct we must see exactly "11 22 33 44 55 66 77 88".
 *
 * Build:
 *   x86_64-w64-mingw32-gcc -O1 -municode -mwindows -o varargprobe.exe varargprobe.c -luser32
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
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

/* A variadic function implemented in this (x86-64) module, for comparison. */
static int local_sum(int count, ...)
{
    va_list ap;
    int i, sum = 0;
    va_start(ap, count);
    for (i = 0; i < count; i++) sum += va_arg(ap, int);
    va_end(ap);
    return sum;
}

static int run(void)
{
    char buf[512];
    WCHAR wbuf[512];
    int n;

    memset(buf, 0, sizeof(buf));
    memset(wbuf, 0, sizeof(wbuf));

    mark("varargprobe build %s %s", __DATE__, __TIME__);

    /* 0. sanity: our own variadic, same module (x86-64 code, no ARM64EC boundary) */
    n = local_sum(8, 11, 22, 33, 44, 55, 66, 77, 88);
    mark("STEP 0 local_sum(8, 11..88) = %d (expect 396)", n);

    /* 1. msvcrt sprintf — ARM64EC builtin, variadic */
    mark("STEP 1 sprintf(msvcrt, %d x8)");
    n = sprintf(buf, "%d %d %d %d %d %d %d %d", 11, 22, 33, 44, 55, 66, 77, 88);
    mark("  sprintf returned %d", n);
    mark("  sprintf out = [%s]", buf);
    mark("  EXPECT      = [11 22 33 44 55 66 77 88]");

    /* 2. user32 wsprintfW — ARM64EC builtin, variadic, wide */
    mark("STEP 2 wsprintfW(user32, %d x8)");
    n = wsprintfW(wbuf, L"%d %d %d %d %d %d %d %d", 11, 22, 33, 44, 55, 66, 77, 88);
    mark("  wsprintfW returned %d", n);
    mark("  wsprintfW out = [%ls]", wbuf);
    mark("  EXPECT        = [11 22 33 44 55 66 77 88]");

    /* 3. wsprintfA — narrow */
    memset(buf, 0, sizeof(buf));
    mark("STEP 3 wsprintfA(user32, %d x8)");
    n = wsprintfA(buf, "%d %d %d %d %d %d %d %d", 11, 22, 33, 44, 55, 66, 77, 88);
    mark("  wsprintfA returned %d", n);
    mark("  wsprintfA out = [%s]", buf);
    mark("  EXPECT        = [11 22 33 44 55 66 77 88]");

    /* 4. mixed types, to see whether only *stack* varargs are affected */
    memset(buf, 0, sizeof(buf));
    mark("STEP 4 sprintf mixed types");
    sprintf(buf, "a=%d b=%s c=%d d=%s e=%d", 1, "two", 3, "four", 5);
    mark("  out    = [%s]", buf);
    mark("  EXPECT = [a=1 b=two c=3 d=four e=5]");

    mark("DONE");
    return 0;
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE hPrev, LPWSTR cmd, int show)
{
    (void)hInst; (void)hPrev; (void)cmd; (void)show;

    g_out = fopen("C:\\windows\\temp\\varargprobe.txt", "w");
    if (!g_out) g_out = fopen("Z:\\varargprobe.txt", "w");
    if (!g_out) return 1;

    run();
    fclose(g_out);

    {
        FILE *src = fopen("C:\\windows\\temp\\varargprobe.txt", "rb");
        FILE *dst = fopen("Z:\\varargprobe.txt", "wb");
        if (src && dst) {
            char b[4096];
            size_t k;
            while ((k = fread(b, 1, sizeof(b), src)) > 0) fwrite(b, 1, k, dst);
        }
        if (src) fclose(src);
        if (dst) fclose(dst);
    }
    return 0;
}
