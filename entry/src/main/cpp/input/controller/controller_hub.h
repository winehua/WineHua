#pragma once

#include "input/controller/controller_types.h"

#include <functional>
#include <mutex>

namespace winehua {
namespace controller {

// Slot merge + logical canonical state. Thread-safe; Physical kit callbacks
// and NAPI (Touch) may call from different threads.
class ControllerHub {
public:
    using StateListener = std::function<void(uint32_t slot, const LogicalGamepadState&)>;

    static ControllerHub& Instance();

    void SetEnabled(bool enabled);
    bool IsEnabled() const;

    void SetButton(ControllerSourceId source, uint32_t slot, LogicalButton button, bool pressed);
    void SetStick(ControllerSourceId source, uint32_t slot, LogicalStick stick, float x, float y);
    void SetTrigger(ControllerSourceId source, uint32_t slot, LogicalTrigger trigger, float value);
    void SetHat(ControllerSourceId source, uint32_t slot, int8_t x, int8_t y);
    void ResetSource(ControllerSourceId source);

    LogicalGamepadState GetState(uint32_t slot) const;
    void SetStateListener(StateListener listener);

    // Deadzone applied to stick vectors before merge (radial).
    void SetInnerDeadzone(float inner);

private:
    ControllerHub() = default;

    struct SourceStickState {
        float x = 0.0f;
        float y = 0.0f;
        bool active = false;
        uint64_t activitySequence = 0;
    };

    struct SlotState {
        uint32_t buttonOwners[kButtonCount] = {};
        SourceStickState sticks[kSourceCount][kStickCount] = {};
        ControllerSourceId stickOwner[kStickCount] = {
            ControllerSourceId::Touch, ControllerSourceId::Touch};
        uint64_t nextActivitySequence = 1;
        float triggers[kSourceCount][kTriggerCount] = {};
        int8_t hatXBySource[kSourceCount] = {};
        int8_t hatYBySource[kSourceCount] = {};
        LogicalGamepadState logical;
    };

    bool RecomputeLocked(uint32_t slot);
    static float ApplyRadialDeadzone(float x, float y, float inner, float* outX, float* outY);
    static ControllerSourceId PickStickOwner(const SlotState& st, uint32_t stickIndex);

    mutable std::mutex mutex_;
    bool enabled_ = false;
    float innerDeadzone_ = 0.10f;
    SlotState slots_[kMaxControllerSlots];
    StateListener listener_;
};

}  // namespace controller
}  // namespace winehua
