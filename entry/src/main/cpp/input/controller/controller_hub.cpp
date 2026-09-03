#include "input/controller/controller_hub.h"

#include <algorithm>
#include <cmath>

namespace winehua {
namespace controller {

ControllerHub& ControllerHub::Instance()
{
    static ControllerHub hub;
    return hub;
}

void ControllerHub::SetEnabled(bool enabled)
{
    StateListener cb;
    LogicalGamepadState snaps[kMaxControllerSlots];
    {
        std::lock_guard<std::mutex> lock(mutex_);
        enabled_ = enabled;
        cb = listener_;
        if (!enabled) {
            for (uint32_t s = 0; s < kMaxControllerSlots; ++s) {
                slots_[s] = SlotState{};
                snaps[s] = slots_[s].logical;
            }
        }
    }
    if (!enabled && cb) {
        for (uint32_t s = 0; s < kMaxControllerSlots; ++s) cb(s, snaps[s]);
    }
}

bool ControllerHub::IsEnabled() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return enabled_;
}

void ControllerHub::SetInnerDeadzone(float inner)
{
    std::lock_guard<std::mutex> lock(mutex_);
    innerDeadzone_ = std::clamp(inner, 0.f, 0.5f);
}

void ControllerHub::SetStateListener(StateListener listener)
{
    std::lock_guard<std::mutex> lock(mutex_);
    listener_ = std::move(listener);
}

LogicalGamepadState ControllerHub::GetState(uint32_t slot) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (slot >= kMaxControllerSlots) return {};
    return slots_[slot].logical;
}

void ControllerHub::SetButton(ControllerSourceId source, uint32_t slot, LogicalButton button, bool pressed)
{
    const uint32_t src = static_cast<uint32_t>(source);
    const uint32_t btn = static_cast<uint32_t>(button);
    if (src >= kSourceCount || btn >= kButtonCount || slot >= kMaxControllerSlots) return;

    StateListener cb;
    LogicalGamepadState snap;
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!enabled_) return;
        uint32_t& owners = slots_[slot].buttonOwners[btn];
        const uint32_t bit = 1u << src;
        if (pressed) owners |= bit;
        else owners &= ~bit;
        changed = RecomputeLocked(slot);
        cb = listener_;
        snap = slots_[slot].logical;
    }
    if (changed && cb) cb(slot, snap);
}

void ControllerHub::SetStick(ControllerSourceId source, uint32_t slot, LogicalStick stick, float x, float y)
{
    const uint32_t src = static_cast<uint32_t>(source);
    const uint32_t stIdx = static_cast<uint32_t>(stick);
    if (src >= kSourceCount || stIdx >= kStickCount || slot >= kMaxControllerSlots) return;
    if (!std::isfinite(x)) x = 0.f;
    if (!std::isfinite(y)) y = 0.f;
    x = std::clamp(x, -1.f, 1.f);
    y = std::clamp(y, -1.f, 1.f);

    StateListener cb;
    LogicalGamepadState snap;
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!enabled_) return;
        auto& st = slots_[slot];
        auto& srcStick = st.sticks[src][stIdx];
        srcStick.x = x;
        srcStick.y = y;
        const float mag = std::sqrt(x * x + y * y);
        const bool active = mag > innerDeadzone_;
        srcStick.active = active;
        if (active) {
            srcStick.activitySequence = st.nextActivitySequence++;
            st.stickOwner[stIdx] = source;
        } else if (st.stickOwner[stIdx] == source) {
            st.stickOwner[stIdx] = PickStickOwner(st, stIdx);
        }
        changed = RecomputeLocked(slot);
        cb = listener_;
        snap = st.logical;
    }
    if (changed && cb) cb(slot, snap);
}

void ControllerHub::SetTrigger(ControllerSourceId source, uint32_t slot, LogicalTrigger trigger, float value)
{
    const uint32_t src = static_cast<uint32_t>(source);
    const uint32_t tr = static_cast<uint32_t>(trigger);
    if (src >= kSourceCount || tr >= kTriggerCount || slot >= kMaxControllerSlots) return;
    if (!std::isfinite(value)) value = 0.f;
    value = std::clamp(value, 0.f, 1.f);

    StateListener cb;
    LogicalGamepadState snap;
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!enabled_) return;
        slots_[slot].triggers[src][tr] = value;
        changed = RecomputeLocked(slot);
        cb = listener_;
        snap = slots_[slot].logical;
    }
    if (changed && cb) cb(slot, snap);
}

void ControllerHub::SetHat(ControllerSourceId source, uint32_t slot, int8_t x, int8_t y)
{
    const uint32_t src = static_cast<uint32_t>(source);
    if (src >= kSourceCount || slot >= kMaxControllerSlots) return;
    x = static_cast<int8_t>(std::clamp<int>(x, -1, 1));
    y = static_cast<int8_t>(std::clamp<int>(y, -1, 1));

    StateListener cb;
    LogicalGamepadState snap;
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!enabled_) return;
        slots_[slot].hatXBySource[src] = x;
        slots_[slot].hatYBySource[src] = y;
        changed = RecomputeLocked(slot);
        cb = listener_;
        snap = slots_[slot].logical;
    }
    if (changed && cb) cb(slot, snap);
}

void ControllerHub::ResetSource(ControllerSourceId source)
{
    const uint32_t src = static_cast<uint32_t>(source);
    if (src >= kSourceCount) return;
    const uint32_t bit = 1u << src;

    StateListener cb;
    LogicalGamepadState snaps[kMaxControllerSlots];
    bool anyChanged = false;
    bool slotChanged[kMaxControllerSlots] = {};
    {
        std::lock_guard<std::mutex> lock(mutex_);
        cb = listener_;
        for (uint32_t slot = 0; slot < kMaxControllerSlots; ++slot) {
            auto& st = slots_[slot];
            for (uint32_t b = 0; b < kButtonCount; ++b) st.buttonOwners[b] &= ~bit;
            for (uint32_t stick = 0; stick < kStickCount; ++stick) {
                st.sticks[src][stick] = SourceStickState{};
                if (st.stickOwner[stick] == source) {
                    st.stickOwner[stick] = PickStickOwner(st, stick);
                }
            }
            for (uint32_t t = 0; t < kTriggerCount; ++t) st.triggers[src][t] = 0.f;
            st.hatXBySource[src] = 0;
            st.hatYBySource[src] = 0;
            slotChanged[slot] = RecomputeLocked(slot);
            anyChanged = anyChanged || slotChanged[slot];
            snaps[slot] = st.logical;
        }
    }
    if (cb && anyChanged) {
        for (uint32_t slot = 0; slot < kMaxControllerSlots; ++slot) {
            if (slotChanged[slot]) cb(slot, snaps[slot]);
        }
    }
}

ControllerSourceId ControllerHub::PickStickOwner(const SlotState& st, uint32_t stickIndex)
{
    ControllerSourceId owner = ControllerSourceId::Touch;
    uint64_t bestSeq = 0;
    bool found = false;
    for (uint32_t s = 0; s < kSourceCount; ++s) {
        const auto& srcStick = st.sticks[s][stickIndex];
        if (!srcStick.active) continue;
        if (!found || srcStick.activitySequence >= bestSeq) {
            found = true;
            bestSeq = srcStick.activitySequence;
            owner = static_cast<ControllerSourceId>(s);
        }
    }
    return owner;
}

float ControllerHub::ApplyRadialDeadzone(float x, float y, float inner, float* outX, float* outY)
{
    const float mag = std::sqrt(x * x + y * y);
    if (mag < inner || mag <= 0.f) {
        *outX = 0.f;
        *outY = 0.f;
        return 0.f;
    }
    constexpr float outer = 0.98f;
    float t = (mag - inner) / (outer - inner);
    if (t > 1.f) t = 1.f;
    const float scale = t / mag;
    *outX = x * scale;
    *outY = y * scale;
    return t;
}

bool ControllerHub::RecomputeLocked(uint32_t slot)
{
    auto& st = slots_[slot];
    LogicalGamepadState next = st.logical;
    next.buttons = 0;
    for (uint32_t b = 0; b < 10; ++b) {  // A..R3 → WHGP bits 0..9
        if (st.buttonOwners[b] != 0) next.buttons |= (1u << b);
    }
    if (st.buttonOwners[static_cast<uint32_t>(LogicalButton::Guide)] != 0) {
        next.buttons |= (1u << 10);
    }

    auto emitStick = [&](LogicalStick stick, int16_t* outX, int16_t* outY) {
        const uint32_t idx = static_cast<uint32_t>(stick);
        const uint32_t owner = static_cast<uint32_t>(st.stickOwner[idx]);
        float ox = 0.f, oy = 0.f;
        ApplyRadialDeadzone(st.sticks[owner][idx].x, st.sticks[owner][idx].y,
                            innerDeadzone_, &ox, &oy);
        *outX = FloatToStick(ox);
        *outY = FloatToStick(oy);
    };
    emitStick(LogicalStick::Left, &next.lx, &next.ly);
    emitStick(LogicalStick::Right, &next.rx, &next.ry);

    float lt = 0.f, rt = 0.f;
    for (uint32_t s = 0; s < kSourceCount; ++s) {
        lt = std::max(lt, st.triggers[s][static_cast<uint32_t>(LogicalTrigger::Left)]);
        rt = std::max(rt, st.triggers[s][static_cast<uint32_t>(LogicalTrigger::Right)]);
    }
    next.lt = FloatToTrigger(lt);
    next.rt = FloatToTrigger(rt);

    int hx = 0, hy = 0;
    for (uint32_t s = 0; s < kSourceCount; ++s) {
        if (st.hatXBySource[s] != 0) hx = st.hatXBySource[s];
        if (st.hatYBySource[s] != 0) hy = st.hatYBySource[s];
    }
    if (st.buttonOwners[static_cast<uint32_t>(LogicalButton::DpadLeft)]) hx = -1;
    if (st.buttonOwners[static_cast<uint32_t>(LogicalButton::DpadRight)]) hx = 1;
    if (st.buttonOwners[static_cast<uint32_t>(LogicalButton::DpadUp)]) hy = 1;
    if (st.buttonOwners[static_cast<uint32_t>(LogicalButton::DpadDown)]) hy = -1;
    if (st.buttonOwners[static_cast<uint32_t>(LogicalButton::DpadLeft)] &&
        st.buttonOwners[static_cast<uint32_t>(LogicalButton::DpadRight)]) {
        hx = 0;
    }
    if (st.buttonOwners[static_cast<uint32_t>(LogicalButton::DpadUp)] &&
        st.buttonOwners[static_cast<uint32_t>(LogicalButton::DpadDown)]) {
        hy = 0;
    }
    next.hatX = static_cast<int8_t>(hx);
    next.hatY = static_cast<int8_t>(hy);

    if (next.buttons != st.logical.buttons || next.lx != st.logical.lx || next.ly != st.logical.ly ||
        next.rx != st.logical.rx || next.ry != st.logical.ry || next.lt != st.logical.lt ||
        next.rt != st.logical.rt || next.hatX != st.logical.hatX || next.hatY != st.logical.hatY) {
        next.sequence = st.logical.sequence + 1;
        st.logical = next;
        return true;
    }
    return false;
}

}  // namespace controller
}  // namespace winehua
