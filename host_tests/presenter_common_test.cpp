#include "graphics/presenter_common.h"

#include <cassert>
#include <iostream>

int main()
{
    assert(winehua::VirglQueuePacingPeriodNs(16666667) == 16666667);
    assert(winehua::VirglQueuePacingPeriodNs(0) ==
           winehua::kDefaultFramePeriodNs);
    assert(winehua::VirglQueuePacingPeriodNs(1000000) ==
           winehua::kMinFramePeriodNs);
    assert(winehua::VirglQueuePacingPeriodNs(100000000) ==
           winehua::kVirglMaxFramePeriodNs);
    assert(winehua::VenusPacingPeriodNs(16666667) == 16166667);
    std::cout << "presenter_common_test PASS\n";
}
