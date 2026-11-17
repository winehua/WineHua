#pragma once
#include <cstdint>
#include <unordered_map>

// Host boundary double: keep the real ZcBridge state machine under test while
// replacing only the OHOS IPC ready-marker publication.
namespace winehua {
class GraphicsBroker {
public:
    static GraphicsBroker& GetInstance() { static GraphicsBroker broker; return broker; }
    void SetZeroCopySurfaceReady(uint64_t key, bool value) { ready[key] = value; }
    std::unordered_map<uint64_t, bool> ready;
};
}
