#pragma once

#include "input/controller/controller_types.h"

#include <atomic>
#include <functional>
#include <mutex>

namespace winehua {
namespace controller {

// Slot merge + logical state. Thread-safe; Physical kit callbacks and NAPI
// (Touch) may call from different threads.
class ControllerHub {
public:
    using StateListener = std::function<void(uint32_t slot, const LogicalGamepadState&)>;

    static ControllerHub& Instance();

    void SetEnabled(bool enabled);
    bool IsEnabled() const;

    void SetButton(ControllerSourceId source, uint32_t slot, LogicalButton button, bool pressed);
    // value in [-1,1] for sticks, [0,1] for triggers. +Y = Up.
    void SetAxis(ControllerSourceId source, uint32_t slot, LogicalAxis axis, float value);
    void SetHat(ControllerSourceId source, uint32_t slot, int8_t x, int8_t y);
    void ResetSource(ControllerSourceId source);

    LogicalGamepadState GetState(uint32_t slot) const;
    void SetStateListener(StateListener listener);

    // Deadzone applied to physical/touch stick axes before merge (radial).
    void SetInnerDeadzone(float inner);

private:
    ControllerHub() = default;

    struct SlotState {
        uint32_t buttonOwners[kButtonCount] = {};
        float axisValues[kSourceCount][kAxisCount] = {};
        bool axisActive[kSourceCount][kAxisCount] = {};
        ControllerSourceId axisOwner[kAxisCount] = {
            ControllerSourceId::Touch, ControllerSourceId::Touch, ControllerSourceId::Touch,
            ControllerSourceId::Touch, ControllerSourceId::Touch, ControllerSourceId::Touch};
        int8_t hatXBySource[kSourceCount] = {};
        int8_t hatYBySource[kSourceCount] = {};
        LogicalGamepadState logical;
    };

    void RecomputeLocked(uint32_t slot);
    static float ApplyRadialDeadzone(float x, float y, float inner, float* outX, float* outY);

    mutable std::mutex mutex_;
    bool enabled_ = false;
    float innerDeadzone_ = 0.10f;
    SlotState slots_[kMaxControllerSlots];
    StateListener listener_;
};

}  // namespace controller
}  // namespace winehua
