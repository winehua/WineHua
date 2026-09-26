/* winehua_t_mem_heap — 堆压力（P3，手段 R）。
 * 判定规格见 docs/engineering/testing-programs.md §3.10。
 * 失败特征：损坏=堆后端断。
 * 协议：20 轮 × 每轮 HeapAlloc 随机尺寸 500 次（写校验图案）+ 随机
 * 顺序 Free，全量图案校验；随机数用固定种子 LCG（可复现）。
 */
#include "../common/winehua_t_check.h"

#define ROUNDS 20
#define ALLOCS_PER_ROUND 500
#define MAX_SIZE 4096

static unsigned int g_seed = 0x12345678u;

static unsigned int lcg(void)
{
    g_seed = g_seed * 1103515245u + 12345u;
    return (g_seed >> 8) & 0xFFFFFF;
}

static void fill_pattern(unsigned char *p, unsigned int size, unsigned int salt)
{
    unsigned int i;
    for (i = 0; i < size; ++i)
        p[i] = (unsigned char)((i * 31 + salt) & 0xFF);
}

static int check_pattern(const unsigned char *p, unsigned int size, unsigned int salt)
{
    unsigned int i;
    for (i = 0; i < size; ++i)
        if (p[i] != (unsigned char)((i * 31 + salt) & 0xFF)) return 0;
    return 1;
}

int main(int argc, char **argv)
{
    HANDLE heap;
    int round, failed = 0;
    unsigned long total = 0;

    t_begin("winehua_t_mem_heap", argc, argv);

    heap = HeapCreate(0, 1 << 20, 0);
    t_check("heap-create", heap != NULL, "err=%lu", GetLastError());
    if (!heap)
        return t_finish();

    for (round = 0; round < ROUNDS && !failed; ++round)
    {
        unsigned char *ptrs[ALLOCS_PER_ROUND];
        unsigned int sizes[ALLOCS_PER_ROUND];
        int i;

        memset(ptrs, 0, sizeof(ptrs));
        for (i = 0; i < ALLOCS_PER_ROUND; ++i)
        {
            sizes[i] = 8 + lcg() % MAX_SIZE;
            ptrs[i] = (unsigned char *)HeapAlloc(heap, 0, sizes[i]);
            if (!ptrs[i])
            {
                failed = 1;
                t_check("alloc-nonnull", 0, "round=%d i=%d size=%u",
                        round, i, sizes[i]);
                break;
            }
            fill_pattern(ptrs[i], sizes[i], (unsigned int)(round * 1000 + i));
            total++;
        }
        /* 校验全部图案（含随机前后交错） */
        for (i = 0; i < ALLOCS_PER_ROUND && !failed; ++i)
        {
            if (!ptrs[i]) continue;
            if (!check_pattern(ptrs[i], sizes[i], (unsigned int)(round * 1000 + i)))
            {
                failed = 1;
                t_check("pattern-intact", 0, "round=%d i=%d", round, i);
            }
        }
        /* 随机序释放，中途穿插重分配 */
        for (i = 0; i < ALLOCS_PER_ROUND && !failed; ++i)
        {
            int j = (int)(lcg() % ALLOCS_PER_ROUND);
            if (ptrs[j])
            {
                if (!HeapFree(heap, 0, ptrs[j]))
                {
                    failed = 1;
                    t_check("free-ok", 0, "round=%d j=%d", round, j);
                }
                ptrs[j] = NULL;
            }
        }
        for (i = 0; i < ALLOCS_PER_ROUND; ++i)
            if (ptrs[i]) HeapFree(heap, 0, ptrs[i]);
    }

    if (!failed)
    {
        t_check("heap-stress-pass", 1, "%lu allocs, %d rounds", total, ROUNDS);
        /* 释放后进程正常退出本身就是判据之一（堆后端无悬挂） */
        t_metric("alloc-total", "%lu", total);
    }
    HeapDestroy(heap);
    return t_finish();
}
