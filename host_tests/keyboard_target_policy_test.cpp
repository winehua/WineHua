#include "compositor/input/keyboard_target_policy.h"
#include <cassert>
#include <unordered_map>
#include <unordered_set>

int main()
{
    std::vector<uint32_t> order{1, 3, 2};
    std::unordered_set<uint32_t> alive{2, 3, 7};
    std::unordered_map<uint32_t,uint32_t> modals;
    auto choose = [&](uint32_t focus, uint32_t root = 1) {
        return winehua::ChooseDesktopKeyboardTarget(focus, root, 2, order,
            [&](uint32_t id) { return alive.count(id) != 0; },
            [&](uint32_t id) {
                auto it = modals.find(id);
                return it != modals.end() && alive.count(it->second) ? it->second : 0;
            });
    };
    assert(choose(0) == 3);
    assert(choose(1) == 3); // desktop focus must not swallow game controls
    assert(choose(7) == 7); // preserve established child focus over pointer hover
    assert(choose(99) == 3); // destroyed focus returns to visible application
    assert(choose(2) == 2); // an explicitly focused taskbar remains focused
    alive.insert(4); modals[3] = 4;
    assert(choose(3) == 4);
    assert(choose(1) == 4); // modal remains authoritative even if desktop got focus
    alive.insert(5); modals[4] = 5;
    assert(choose(3) == 5);
    alive.erase(5); modals.erase(4);
    assert(choose(5) == 4); // close nested modal, return to visible owner modal
    alive.erase(4); modals.erase(3);
    assert(choose(4) == 3); // close modal, return to application
    alive.erase(3);
    assert(choose(3) == 1); // no application, root is a deliberate fallback
    assert(choose(7, 0) == 0); // stopped session ignores stale focus entirely
}
