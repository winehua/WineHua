/* CPU micro-benchmark for the 32-bit WineHua stack.
 *
 * Purpose: separate interpreter/dynarec cost from graphics cost when
 * investigating "the game runs, but several times slower than a normal
 * box64 + Wine stack". Each section exercises a different dynarec trait:
 *
 *   alu     long straight-line loop with a data dependent branch
 *   callret deep, noinline recursion (block merging / return prediction)
 *   memcpy  large buffer copies (REP MOVS style block moves)
 *   blit    256x256x4 DWORD row copies, mirroring the game's bitmap code
 *   qpc     QueryPerformanceCounter / GetTickCount64 call cost
 *
 * Results are appended to C:\windows\temp\winehua-x86-cpubench.txt so a run
 * can be collected without an interactive console.
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>

#define ALU_ITERATIONS   50000000u
#define CALLRET_STEPS    10000000u
#define MEMCPY_BYTES     (512u * 1024u * 1024u)
#define MEMCPY_CHUNK     (64u * 1024u)
#define BLIT_ROUNDS      512u
#define QPC_CALLS        2000000u

static unsigned g_sink;

static unsigned alu_loop(unsigned n)
{
    unsigned a = 1u, b = 0x9e3779b9u, c = 0x85ebca6bu, i;
    for (i = 0; i < n; i++)
    {
        a = a * 1664525u + 1013904223u;
        b ^= a >> 7;
        b = (b << 5) | (b >> 27);
        c += b ^ (a * 3u);
        if ((c & 0xffffu) == 0x1234u) c ^= 1u;
    }
    return a ^ b ^ c;
}

static __attribute__((noinline)) unsigned callret(unsigned depth, unsigned n)
{
    if (!n) return depth;
    if (depth >= 6) return callret(0, n - 1) + 1u;
    return callret(depth + 1, n - 1) + 2u;
}

static unsigned memcpy_loop(unsigned bytes, unsigned chunk)
{
    static unsigned char src[MEMCPY_CHUNK], dst[MEMCPY_CHUNK];
    unsigned done = 0;
    memset(src, 0x5a, chunk);
    while (done < bytes)
    {
        memcpy(dst, src, chunk);
        done += chunk;
    }
    return dst[0] + dst[chunk - 1];
}

static unsigned blit_loop(unsigned width, unsigned height, unsigned rounds)
{
    static unsigned src[256 * 256], dst[256 * 256];
    unsigned r, y, x;
    for (y = 0; y < height; y++)
        for (x = 0; x < width; x++)
            src[y * width + x] = y * 31u + x;
    for (r = 0; r < rounds; r++)
        for (y = 0; y < 256; y++)
            memcpy(dst + y * 256, src + y * 256, 256 * sizeof(unsigned));
    return dst[width] + dst[height];
}

static unsigned qpc_loop(unsigned n)
{
    LARGE_INTEGER freq, a, b;
    unsigned i, acc = 0;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&a);
    for (i = 0; i < n; i++)
    {
        QueryPerformanceCounter(&b);
        acc += (unsigned)(b.QuadPart ^ GetTickCount64());
    }
    QueryPerformanceCounter(&a);
    return acc ^ (unsigned)(b.QuadPart - a.QuadPart) ^ (unsigned)freq.QuadPart;
}

int main(void)
{
    LARGE_INTEGER freq, t0, t1;
    double alu_ms, callret_ms, memcpy_ms, blit_ms, qpc_ms;
    FILE *out = fopen("C:\\windows\\temp\\winehua-x86-cpubench.txt", "wb");

    if (!out) return 2;
    QueryPerformanceFrequency(&freq);
    fprintf(out, "freq=%lld\n", (long long)freq.QuadPart);

    QueryPerformanceCounter(&t0);
    g_sink = alu_loop(ALU_ITERATIONS);
    QueryPerformanceCounter(&t1);
    alu_ms = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)freq.QuadPart;

    QueryPerformanceCounter(&t0);
    g_sink += callret(0, CALLRET_STEPS);
    QueryPerformanceCounter(&t1);
    callret_ms = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)freq.QuadPart;

    QueryPerformanceCounter(&t0);
    g_sink += memcpy_loop(MEMCPY_BYTES, MEMCPY_CHUNK);
    QueryPerformanceCounter(&t1);
    memcpy_ms = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)freq.QuadPart;

    QueryPerformanceCounter(&t0);
    g_sink += blit_loop(256, 256, BLIT_ROUNDS);
    QueryPerformanceCounter(&t1);
    blit_ms = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)freq.QuadPart;

    QueryPerformanceCounter(&t0);
    g_sink += qpc_loop(QPC_CALLS);
    QueryPerformanceCounter(&t1);
    qpc_ms = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)freq.QuadPart;

    fprintf(out, "alu=%.1fms (%u iters)\n", alu_ms, ALU_ITERATIONS);
    fprintf(out, "callret=%.1fms (%u steps)\n", callret_ms, CALLRET_STEPS);
    fprintf(out, "memcpy=%.1fms (%u bytes)\n", memcpy_ms, MEMCPY_BYTES);
    fprintf(out, "blit=%.1fms (%u rounds)\n", blit_ms, BLIT_ROUNDS);
    fprintf(out, "qpc=%.1fms (%u calls)\n", qpc_ms, QPC_CALLS);
    fprintf(out, "total=%.1fms sink=%u\n",
            alu_ms + callret_ms + memcpy_ms + blit_ms + qpc_ms, g_sink);
    fflush(out);
    fclose(out);
    return 0;
}
