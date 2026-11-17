#include "../smoke/benchmark_statistics.h"
#include <cstdio>
int main() {
    double values[] = {40, 1, 10, 20, 5};
    qsort(values, 5, sizeof(double), benchmark_double_compare);
    if (benchmark_percentile(values, 5, .5) != 10 ||
        benchmark_percentile(values, 5, .95) != 40 ||
        benchmark_percentile(values, 5, .99) != 40 ||
        benchmark_percentile(values, 5, 0) != 1 ||
        benchmark_percentile(values, 5, 1) != 40 ||
        benchmark_percentile(values, 0, .5) != 0) return 1;
    std::puts("benchmark_statistics: nearest-rank and empty input checks PASS");
}
