// controller_merge_test.cpp — ControllerHub source ownership (make test)
#include "input/controller/controller_hub.h"
#include "input/controller/gamepad_ipc_protocol.h"

#include <cstdio>

using winehua::controller::ButtonBit;
using winehua::controller::ControllerHub;
using winehua::controller::ControllerSourceId;
using winehua::controller::LogicalAxis;
using winehua::controller::LogicalButton;

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond, msg) do { \
    ++g_checks; \
    if (!(cond)) { \
        ++g_failures; \
        std::printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
    } \
} while (0)

int main()
{
    static_assert(sizeof(whgp_header) == 16, "whgp_header packed size");
    static_assert(sizeof(whgp_state_v1) == 20, "whgp_state_v1 packed size");
    static_assert(sizeof(whgp_rumble_v1) == 8, "whgp_rumble_v1 packed size");

    auto& hub = ControllerHub::Instance();
    hub.SetEnabled(true);
    hub.SetInnerDeadzone(0.10f);

    // Button ownership: Touch A down, Physical A down, Touch A up → still down
    hub.SetButton(ControllerSourceId::Touch, 0, LogicalButton::A, true);
    CHECK(hub.GetState(0).buttons & ButtonBit(LogicalButton::A), "touch A down");
    hub.SetButton(ControllerSourceId::Physical, 0, LogicalButton::A, true);
    hub.SetButton(ControllerSourceId::Touch, 0, LogicalButton::A, false);
    CHECK(hub.GetState(0).buttons & ButtonBit(LogicalButton::A), "physical keeps A");
    hub.SetButton(ControllerSourceId::Physical, 0, LogicalButton::A, false);
    CHECK(!(hub.GetState(0).buttons & ButtonBit(LogicalButton::A)), "both released");

    // Trigger max
    hub.SetAxis(ControllerSourceId::Touch, 0, LogicalAxis::LT, 0.4f);
    hub.SetAxis(ControllerSourceId::Physical, 0, LogicalAxis::LT, 0.8f);
    CHECK(hub.GetState(0).lt > 20000, "trigger max physical");
    hub.SetAxis(ControllerSourceId::Physical, 0, LogicalAxis::LT, 0.f);
    CHECK(hub.GetState(0).lt > 10000, "trigger remains touch");

    // ResetSource clears only that source
    hub.SetButton(ControllerSourceId::Touch, 0, LogicalButton::B, true);
    hub.SetButton(ControllerSourceId::Physical, 0, LogicalButton::B, true);
    hub.ResetSource(ControllerSourceId::Touch);
    CHECK(hub.GetState(0).buttons & ButtonBit(LogicalButton::B), "physical B after touch reset");
    hub.ResetSource(ControllerSourceId::Physical);
    CHECK(!(hub.GetState(0).buttons & ButtonBit(LogicalButton::B)), "all clear");

    // Stick deadzone
    hub.SetAxis(ControllerSourceId::Physical, 0, LogicalAxis::LX, 0.05f);
    hub.SetAxis(ControllerSourceId::Physical, 0, LogicalAxis::LY, 0.05f);
    CHECK(hub.GetState(0).lx == 0 && hub.GetState(0).ly == 0, "inside deadzone");
    hub.SetAxis(ControllerSourceId::Physical, 0, LogicalAxis::LX, 1.0f);
    hub.SetAxis(ControllerSourceId::Physical, 0, LogicalAxis::LY, 0.0f);
    CHECK(hub.GetState(0).lx > 20000, "full stick X");

    // Last-active stick: Touch after Physical takes ownership; Touch release falls back.
    hub.SetAxis(ControllerSourceId::Physical, 0, LogicalAxis::LX, 1.0f);
    hub.SetAxis(ControllerSourceId::Physical, 0, LogicalAxis::LY, 0.0f);
    hub.SetAxis(ControllerSourceId::Touch, 0, LogicalAxis::LX, 0.5f);
    hub.SetAxis(ControllerSourceId::Touch, 0, LogicalAxis::LY, 0.0f);
    CHECK(hub.GetState(0).lx > 10000 && hub.GetState(0).lx < 20000, "touch last-active");
    hub.SetAxis(ControllerSourceId::Touch, 0, LogicalAxis::LX, 0.0f);
    hub.SetAxis(ControllerSourceId::Touch, 0, LogicalAxis::LY, 0.0f);
    CHECK(hub.GetState(0).lx > 20000, "physical fallback after touch");
    hub.ResetSource(ControllerSourceId::Physical);
    hub.ResetSource(ControllerSourceId::Touch);

    hub.SetEnabled(false);
    std::printf("controller_merge_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
