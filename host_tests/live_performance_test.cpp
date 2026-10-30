#include "common/live_performance.h"
#include <cassert>
#include <chrono>
#include <cmath>
#include <thread>

int main()
{
    auto& stats = winehua::LivePerformance::Instance();
    stats.SetGpu("test-gpu");
    stats.Sample();
    for (int i = 0; i < 10; ++i) { stats.Commit(1); stats.Commit(2); stats.Present(); stats.Present(); }
    std::this_thread::sleep_for(std::chrono::milliseconds(550));
    auto sample = stats.Sample();
    assert(sample.clientFps > 0 && sample.gpu == "test-gpu");
    assert(std::abs(sample.displayFps - sample.clientFps * 2) < 0.1);
    std::this_thread::sleep_for(std::chrono::milliseconds(550));
    sample = stats.Sample();
    assert(sample.clientFps == 0 && sample.displayFps == 0);
    return 0;
}
