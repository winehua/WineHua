#ifndef WINEHUA_BENCHMARK_STATISTICS_H
#define WINEHUA_BENCHMARK_STATISTICS_H
#include <math.h>
#include <stddef.h>
#include <stdlib.h>

static int benchmark_double_compare(const void *left, const void *right) {
    const double a = *(const double *)left, b = *(const double *)right;
    return (a > b) - (a < b);
}

// Nearest-rank percentile of already sorted, finite, positive samples.
static double benchmark_percentile(const double *sorted, size_t count, double fraction) {
    size_t rank;
    if (!count) return 0;
    rank = (size_t)ceil(fraction * count);
    if (rank < 1) rank = 1;
    if (rank > count) rank = count;
    return sorted[rank - 1];
}
#endif
