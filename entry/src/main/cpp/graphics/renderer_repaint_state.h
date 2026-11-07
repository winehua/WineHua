#pragma once
#include <atomic>
#include <cstdint>

namespace winehua {

// Visibility requests arrive on the UI thread; only the render thread records
// successful presentations. A request made during a swap belongs to the next
// frame and must not be acknowledged by that older swap.
class RendererRepaintState {
public:
    void RequestRedraw() {
        requested_.fetch_add(1, std::memory_order_release);
    }
    uint64_t CaptureRequest() const {
        return requested_.load(std::memory_order_acquire);
    }
    bool NeedsRedraw(uint64_t request) const { return request != presented_; }
    void DidPresent(uint64_t request) { presented_ = request; }

private:
    std::atomic<uint64_t> requested_{1};
    uint64_t presented_ = 0;
};

} // namespace winehua
