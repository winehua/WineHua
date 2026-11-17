#include "common/displayed_fps.h"
#include <cassert>
#include <fstream>
#include <limits>

int main() {
    char directory[] = "/tmp/winehua-route-metrics-XXXXXX";
    assert(mkdtemp(directory));
    const std::string base = std::string(directory) + "/display.txt";
    // X11 output IDs are zero. The legacy guest reader must see all five fields.
    assert(winehua::PublishDisplayRouteSample(base.c_str(), 0, 20, 59.5, "x11", 123456));
    unsigned long long seq, key; unsigned id; double fps; std::string route;
    std::ifstream(base) >> seq >> fps >> id >> route >> key;
    assert(seq == 20 && fps == 59.5 && id == 0 && route == "x11" && key == 123456);
    // A route sample may not overwrite another window's timestamped HUD sample.
    assert(winehua::PublishDisplayedFpsSample(base.c_str(), 7, 4, 48, 1000000));
    assert(winehua::PublishDisplayRouteSample(base.c_str(), 8, 21, 60, "wayland", 789));
    double hud = -1;
    assert(winehua::ReadDisplayedFpsSample(base.c_str(), 7, 1001000, hud) && hud == 48);
    assert(!winehua::ReadDisplayedFpsSample(base.c_str(), 8, 1001000, hud));
    assert(!winehua::ReadDisplayedFpsSample(base.c_str(), 7, 4000000, hud));
    assert(!winehua::PublishDisplayRouteSample(base.c_str(), 0, 22, std::numeric_limits<double>::quiet_NaN(), "x11", 0));
    assert(!winehua::PublishDisplayRouteSample(base.c_str(), 0, 22, 60, "invalid", 0));
    std::ifstream(base) >> seq >> fps >> id >> route >> key;
    assert(seq == 21 && route == "wayland" && key == 789);
    unlink(base.c_str()); unlink(winehua::DisplayedFpsPath(base.c_str(), 7).c_str()); rmdir(directory);
}
