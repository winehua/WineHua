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
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!enabled_) return;
        uint32_t& owners = slots_[slot].buttonOwners[btn];
        const uint32_t bit = 1u << src;
        if (pressed) owners |= bit;
        else owners &= ~bit;
        RecomputeLocked(slot);
        cb = listener_;
        snap = slots_[slot].logical;
    }
    if (cb) cb(slot, snap);
}

void ControllerHub::SetAxis(ControllerSourceId source, uint32_t slot, LogicalAxis axis, float value)
{
    const uint32_t src = static_cast<uint32_t>(source);
    const uint32_t ax = static_cast<uint32_t>(axis);
    if (src >= kSourceCount || ax >= kAxisCount || slot >= kMaxControllerSlots) return;

    StateListener cb;
    LogicalGamepadState snap;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!enabled_) return;

        float v = value;
        if (axis == LogicalAxis::LT || axis == LogicalAxis::RT) {
            v = std::clamp(v, 0.f, 1.f);
            slots_[slot].axisValues[src][ax] = v;
            slots_[slot].axisActive[src][ax] = v > 0.02f;
        } else {
            v = std::clamp(v, -1.f, 1.f);
            slots_[slot].axisValues[src][ax] = v;
            const bool isLeft = (axis == LogicalAxis::LX || axis == LogicalAxis::LY);
            const uint32_t axX = static_cast<uint32_t>(isLeft ? LogicalAxis::LX : LogicalAxis::RX);
            const uint32_t axY = static_cast<uint32_t>(isLeft ? LogicalAxis::LY : LogicalAxis::RY);
            const float dx = slots_[slot].axisValues[src][axX];
            const float dy = slots_[slot].axisValues[src][axY];
            const float mag = std::sqrt(dx * dx + dy * dy);
            const bool active = mag > innerDeadzone_;
            slots_[slot].axisActive[src][axX] = active;
            slots_[slot].axisActive[src][axY] = active;
            if (active) {
                slots_[slot].axisOwner[axX] = source;
                slots_[slot].axisOwner[axY] = source;
            } else if (slots_[slot].axisOwner[axX] == source) {
                slots_[slot].axisOwner[axX] = ControllerSourceId::Touch;
                slots_[slot].axisOwner[axY] = ControllerSourceId::Touch;
                for (uint32_t s = 0; s < kSourceCount; ++s) {
                    if (slots_[slot].axisActive[s][axX] || slots_[slot].axisActive[s][axY]) {
                        slots_[slot].axisOwner[axX] = static_cast<ControllerSourceId>(s);
                        slots_[slot].axisOwner[axY] = static_cast<ControllerSourceId>(s);
                        break;
                    }
                }
            }
        }

        RecomputeLocked(slot);
        cb = listener_;
        snap = slots_[slot].logical;
    }
    if (cb) cb(slot, snap);
}

void ControllerHub::SetHat(ControllerSourceId source, uint32_t slot, int8_t x, int8_t y)
{
    const uint32_t src = static_cast<uint32_t>(source);
    if (src >= kSourceCount || slot >= kMaxControllerSlots) return;
    x = static_cast<int8_t>(std::clamp<int>(x, -1, 1));
    y = static_cast<int8_t>(std::clamp<int>(y, -1, 1));

    StateListener cb;
    LogicalGamepadState snap;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!enabled_) return;
        slots_[slot].hatXBySource[src] = x;
        slots_[slot].hatYBySource[src] = y;
        RecomputeLocked(slot);
        cb = listener_;
        snap = slots_[slot].logical;
    }
    if (cb) cb(slot, snap);
}

void ControllerHub::ResetSource(ControllerSourceId source)
{
    const uint32_t src = static_cast<uint32_t>(source);
    if (src >= kSourceCount) return;
    const uint32_t bit = 1u << src;

    StateListener cb;
    LogicalGamepadState snaps[kMaxControllerSlots];
    {
        std::lock_guard<std::mutex> lock(mutex_);
        cb = listener_;
        for (uint32_t slot = 0; slot < kMaxControllerSlots; ++slot) {
            auto& st = slots_[slot];
            for (uint32_t b = 0; b < kButtonCount; ++b) st.buttonOwners[b] &= ~bit;
            for (uint32_t a = 0; a < kAxisCount; ++a) {
                st.axisValues[src][a] = 0.f;
                st.axisActive[src][a] = false;
                if (st.axisOwner[a] == source) {
                    st.axisOwner[a] = ControllerSourceId::Touch;
                    for (uint32_t s = 0; s < kSourceCount; ++s) {
                        if (st.axisActive[s][a]) {
                            st.axisOwner[a] = static_cast<ControllerSourceId>(s);
                            break;
                        }
                    }
                }
            }
            st.hatXBySource[src] = 0;
            st.hatYBySource[src] = 0;
            RecomputeLocked(slot);
            snaps[slot] = st.logical;
        }
    }
    if (cb) {
        for (uint32_t slot = 0; slot < kMaxControllerSlots; ++slot) cb(slot, snaps[slot]);
    }
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

void ControllerHub::RecomputeLocked(uint32_t slot)
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

    auto emitStick = [&](LogicalAxis axX, LogicalAxis axY, int16_t* outX, int16_t* outY) {
        const uint32_t aX = static_cast<uint32_t>(axX);
        const uint32_t owner = static_cast<uint32_t>(st.axisOwner[aX]);
        float ox = 0.f, oy = 0.f;
        ApplyRadialDeadzone(st.axisValues[owner][aX],
                            st.axisValues[owner][static_cast<uint32_t>(axY)],
                            innerDeadzone_, &ox, &oy);
        *outX = FloatToStick(ox);
        *outY = FloatToStick(oy);
    };
    emitStick(LogicalAxis::LX, LogicalAxis::LY, &next.lx, &next.ly);
    emitStick(LogicalAxis::RX, LogicalAxis::RY, &next.rx, &next.ry);

    float lt = 0.f, rt = 0.f;
    for (uint32_t s = 0; s < kSourceCount; ++s) {
        lt = std::max(lt, st.axisValues[s][static_cast<uint32_t>(LogicalAxis::LT)]);
        rt = std::max(rt, st.axisValues[s][static_cast<uint32_t>(LogicalAxis::RT)]);
    }
    next.lt = FloatToTrigger(lt);
    next.rt = FloatToTrigger(rt);

    // DPad: merge hat sources OR button dpad ownership into hat.
    int hx = 0, hy = 0;
    for (uint32_t s = 0; s < kSourceCount; ++s) {
        if (st.hatXBySource[s] != 0) hx = st.hatXBySource[s];
        if (st.hatYBySource[s] != 0) hy = st.hatYBySource[s];
    }
    if (st.buttonOwners[static_cast<uint32_t>(LogicalButton::DpadLeft)]) hx = -1;
    if (st.buttonOwners[static_cast<uint32_t>(LogicalButton::DpadRight)]) hx = 1;
    if (st.buttonOwners[static_cast<uint32_t>(LogicalButton::DpadUp)]) hy = 1;
    if (st.buttonOwners[static_cast<uint32_t>(LogicalButton::DpadDown)]) hy = -1;
    // If both left+right, prefer last non-zero from hats already; clamp conflict to 0.
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
    }
}

}  // namespace controller
}  // namespace winehua
