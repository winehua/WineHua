#pragma once

#include <cstdint>

// Canonical Controller Space (platform-independent):
//   Stick X: right positive, range [-1, 1]
//   Stick Y: up positive, range [-1, 1]
//   Hat X:   right positive, values -1 / 0 / +1
//   Hat Y:   up positive, values -1 / 0 / +1
//   Trigger: 0..1
// LogicalGamepadState sticks use int16 [-32768, 32767] with the same signs.
// Source adapters normalize platform raw input into this space. Hub and WHGP
// never invert based on source. Wine bus_ohos maps canonical sticks to
// XInput without analog Y inversion.

namespace winehua {
namespace controller {

enum class LogicalButton : uint8_t {
    A = 0,
    B,
    X,
    Y,
    LB,
    RB,
    Back,
    Start,
    L3,
    R3,
    DpadUp,
    DpadRight,
    DpadDown,
    DpadLeft,
    Guide,
    Count
};

enum class LogicalStick : uint8_t {
    Left = 0,
    Right,
    Count
};

enum class LogicalTrigger : uint8_t {
    Left = 0,
    Right,
    Count
};

enum class ControllerSourceType : uint8_t {
    Touch = 0,
    Physical = 1,
    Keyboard = 2,
    Macro = 3,
};

enum class ControllerSourceId : uint8_t {
    Touch = 0,
    Physical = 1,
    Keyboard = 2,
    Count = 3,
};

struct Stick2D {
    float x = 0.0f;
    float y = 0.0f;
};

static constexpr uint32_t kMaxControllerSlots = 4;
static constexpr uint32_t kButtonCount = static_cast<uint32_t>(LogicalButton::Count);
static constexpr uint32_t kStickCount = static_cast<uint32_t>(LogicalStick::Count);
static constexpr uint32_t kTriggerCount = static_cast<uint32_t>(LogicalTrigger::Count);
static constexpr uint32_t kSourceCount = static_cast<uint32_t>(ControllerSourceId::Count);

struct LogicalGamepadState {
    uint32_t buttons = 0;  // bit i = LogicalButton(i) for i < 10 (A..R3); dpad via hat
    int16_t lx = 0;
    int16_t ly = 0;
    int16_t rx = 0;
    int16_t ry = 0;
    uint16_t lt = 0;
    uint16_t rt = 0;
    int8_t hatX = 0;
    int8_t hatY = 0;
    uint64_t sequence = 0;
};

inline uint32_t ButtonBit(LogicalButton b)
{
    return 1u << static_cast<uint32_t>(b);
}

inline int16_t FloatToStick(float v)
{
    if (v > 1.f) v = 1.f;
    if (v < -1.f) v = -1.f;
    if (v >= 0.f) return static_cast<int16_t>(v * 32767.f);
    return static_cast<int16_t>(v * 32768.f);
}

inline uint16_t FloatToTrigger(float v)
{
    if (v < 0.f) v = 0.f;
    if (v > 1.f) v = 1.f;
    return static_cast<uint16_t>(v * 32767.f);
}

}  // namespace controller
}  // namespace winehua
