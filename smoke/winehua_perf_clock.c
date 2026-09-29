// Read-only device calibration for interpreting /proc process CPU ticks.
#include <stdio.h>
#include <time.h>
#include <unistd.h>
int main(void) {
    struct timespec time;
    const long ticks = sysconf(_SC_CLK_TCK);
    if (ticks <= 0 || clock_gettime(CLOCK_MONOTONIC, &time) != 0) return 1;
    printf("{\"clockTicksPerSecond\":%ld,\"monotonicNs\":%llu}\n", ticks,
           (unsigned long long)time.tv_sec * 1000000000ULL + time.tv_nsec);
    return 0;
}
