#pragma once
#include <cstdint>
#include <vector>

namespace winehua {
// Preserve a valid child focus. Desktop/expired focus falls back to the latest
// visible application rather than cursor hover or the pinned taskbar.
template<class Visible, class Modal>
uint32_t ChooseDesktopKeyboardTarget(uint32_t focused, uint32_t root, uint32_t taskbar,
    const std::vector<uint32_t>& order, Visible visible, Modal modal)
{
    if (!root) return 0;
    uint32_t target = focused && focused != root && visible(focused) ? focused : 0;
    if (!target) {
        for (auto it = order.rbegin(); it != order.rend(); ++it) {
            if (*it != root && *it != taskbar && visible(*it)) { target = *it; break; }
        }
    }
    if (!target) return root;
    for (unsigned depth = 0; depth < 16; ++depth) {
        const uint32_t next = modal(target);
        if (!next || next == target) break;
        target = next;
    }
    return target;
}
} // namespace winehua
