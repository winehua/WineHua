// controller_merge_test.cpp — Canonical Controller Space + Hub merge (make test)
#include "input/controller/controller_hub.h"
#include "input/controller/controller_types.h"
#include "input/controller/gamepad_ipc_protocol.h"
#include "input/controller/ohos_gamepad_adapter.h"

#include <cstdio>
#include <limits>

using winehua::controller::ButtonBit;
using winehua::controller::ControllerHub;
using winehua::controller::ControllerSourceId;
using winehua::controller::LogicalButton;
using winehua::controller::LogicalGamepadState;
using winehua::controller::LogicalStick;
using winehua::controller::LogicalTrigger;
using winehua::controller::NormalizeOhosHat;
using winehua::controller::NormalizeOhosThumb;
using winehua::controller::NormalizeOhosTrigger;
using winehua::controller::NormalizeScreenThumb;

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond, msg) do { \
    ++g_checks; \
    if (!(cond)) { \
        ++g_failures; \
        std::printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
    } \
} while (0)

static whgp_state_v2 SerializeCanonical(const LogicalGamepadState& state)
{
    whgp_state_v2 body{};
    body.buttons = state.buttons;
    body.lx = state.lx;
    body.ly = state.ly;
    body.rx = state.rx;
    body.ry = state.ry;
    body.lt = state.lt;
    body.rt = state.rt;
    body.hat_x = state.hatX;
    body.hat_y = state.hatY;
    return body;
}

int main()
{
    static_assert(sizeof(whgp_header) == 16, "whgp_header packed size");
    static_assert(sizeof(whgp_state_v2) == 20, "whgp_state_v2 packed size");
    static_assert(sizeof(whgp_rumble_v1) == 8, "whgp_rumble_v1 packed size");
    static_assert(WHGP_VERSION == 2, "WHGP v2");

    const float inf = std::numeric_limits<float>::infinity();
    const float nan = std::numeric_limits<float>::quiet_NaN();

    // OHOS adapter: Kit/SDL raw up is negative Y → canonical +Y.
    auto up = NormalizeOhosThumb(0.0, -1.0);
    CHECK(up.x == 0.f && up.y == 1.f, "OH raw up → canonical up");
    auto down = NormalizeOhosThumb(0.0, 1.0);
    CHECK(down.x == 0.f && down.y == -1.f, "OH raw down → canonical down");
    auto right = NormalizeOhosThumb(1.0, 0.0);
    CHECK(right.x == 1.f && right.y == 0.f, "OH raw right");
    auto left = NormalizeOhosThumb(-1.0, 0.0);
    CHECK(left.x == -1.f && left.y == 0.f, "OH raw left");
    auto diag = NormalizeOhosThumb(0.5, -0.5);
    CHECK(diag.x == 0.5f && diag.y == 0.5f, "OH raw diagonal up-right");
    auto clamped = NormalizeOhosThumb(2.0, -3.0);
    CHECK(clamped.x == 1.f && clamped.y == 1.f, "OH thumb clamp");
    auto nanThumb = NormalizeOhosThumb(nan, -1.0);
    CHECK(nanThumb.x == 0.f && nanThumb.y == 1.f, "OH thumb NaN X");
    auto infThumb = NormalizeOhosThumb(0.0, -inf);
    CHECK(infThumb.y == 0.f, "OH thumb Inf Y");

    auto hatUp = NormalizeOhosHat(0.0, -1.0);
    CHECK(hatUp.x == 0 && hatUp.y == 1, "OH hat raw up → canonical up");
    auto hatDown = NormalizeOhosHat(0.0, 1.0);
    CHECK(hatDown.x == 0 && hatDown.y == -1, "OH hat raw down → canonical down");
    auto hatIdle = NormalizeOhosHat(0.2, 0.2);
    CHECK(hatIdle.x == 0 && hatIdle.y == 0, "OH hat threshold");

    CHECK(NormalizeOhosTrigger(0.25) == 0.25f, "OH trigger pass");
    CHECK(NormalizeOhosTrigger(-0.2) == 0.f, "OH trigger clamp low");
    CHECK(NormalizeOhosTrigger(1.5) == 1.f, "OH trigger clamp high");
    CHECK(NormalizeOhosTrigger(nan) == 0.f, "OH trigger NaN");

    // Touch / screen adapter: finger up is negative screen dy → canonical +Y.
    auto touchUp = NormalizeScreenThumb(0.f, -40.f, 50.f);
    CHECK(touchUp.x == 0.f && touchUp.y > 0.f, "touch up → canonical up");
    auto touchDown = NormalizeScreenThumb(0.f, 40.f, 50.f);
    CHECK(touchDown.y < 0.f, "touch down → canonical down");
    auto touchCenter = NormalizeScreenThumb(0.f, 0.f, 50.f);
    CHECK(touchCenter.x == 0.f && touchCenter.y == 0.f, "touch center");
    auto touchClamp = NormalizeScreenThumb(100.f, 0.f, 50.f);
    CHECK(touchClamp.x == 1.f, "touch radius clamp");
    auto touchBad = NormalizeScreenThumb(1.f, 1.f, 0.f);
    CHECK(touchBad.x == 0.f && touchBad.y == 0.f, "touch zero radius");

    auto& hub = ControllerHub::Instance();
    hub.SetEnabled(false);
    hub.SetEnabled(true);
    hub.SetInnerDeadzone(0.10f);
    hub.SetStateListener(nullptr);

    // Button ownership: Touch A down, Physical A down, Touch A up → still down
    hub.SetButton(ControllerSourceId::Touch, 0, LogicalButton::A, true);
    CHECK(hub.GetState(0).buttons & ButtonBit(LogicalButton::A), "touch A down");
    hub.SetButton(ControllerSourceId::Physical, 0, LogicalButton::A, true);
    hub.SetButton(ControllerSourceId::Touch, 0, LogicalButton::A, false);
    CHECK(hub.GetState(0).buttons & ButtonBit(LogicalButton::A), "physical keeps A");
    hub.SetButton(ControllerSourceId::Physical, 0, LogicalButton::A, false);
    CHECK(!(hub.GetState(0).buttons & ButtonBit(LogicalButton::A)), "both released");

    // Trigger max
    hub.SetTrigger(ControllerSourceId::Touch, 0, LogicalTrigger::Left, 0.4f);
    hub.SetTrigger(ControllerSourceId::Physical, 0, LogicalTrigger::Left, 0.8f);
    CHECK(hub.GetState(0).lt > 20000, "trigger max physical");
    hub.SetTrigger(ControllerSourceId::Physical, 0, LogicalTrigger::Left, 0.f);
    CHECK(hub.GetState(0).lt > 10000, "trigger remains touch");
    hub.SetTrigger(ControllerSourceId::Touch, 0, LogicalTrigger::Left, 0.f);

    // ResetSource clears only that source
    hub.SetButton(ControllerSourceId::Touch, 0, LogicalButton::B, true);
    hub.SetButton(ControllerSourceId::Physical, 0, LogicalButton::B, true);
    hub.ResetSource(ControllerSourceId::Touch);
    CHECK(hub.GetState(0).buttons & ButtonBit(LogicalButton::B), "physical B after touch reset");
    hub.ResetSource(ControllerSourceId::Physical);
    CHECK(!(hub.GetState(0).buttons & ButtonBit(LogicalButton::B)), "all clear");

    // Stick deadzone on the 2D vector
    hub.SetStick(ControllerSourceId::Physical, 0, LogicalStick::Left, 0.05f, 0.05f);
    CHECK(hub.GetState(0).lx == 0 && hub.GetState(0).ly == 0, "inside deadzone");
    hub.SetStick(ControllerSourceId::Physical, 0, LogicalStick::Left, 1.0f, 0.0f);
    CHECK(hub.GetState(0).lx > 20000, "full stick X");
    hub.ResetSource(ControllerSourceId::Physical);

    // Touch Up and Physical normalized Up both yield ly > 0.
    hub.SetStick(ControllerSourceId::Touch, 0, LogicalStick::Left, touchUp.x, touchUp.y);
    CHECK(hub.GetState(0).ly > 0, "touch up → hub ly > 0");
    hub.ResetSource(ControllerSourceId::Touch);
    hub.SetStick(ControllerSourceId::Physical, 0, LogicalStick::Left, up.x, up.y);
    CHECK(hub.GetState(0).ly > 0, "physical normalized up → hub ly > 0");
    hub.SetStick(ControllerSourceId::Physical, 0, LogicalStick::Right, 0.f, 1.f);
    CHECK(hub.GetState(0).ry > 0, "right stick up → hub ry > 0");
    hub.ResetSource(ControllerSourceId::Physical);

    // SetStick is atomic: one call publishes a complete x/y pair once.
    int publishes = 0;
    LogicalGamepadState last{};
    hub.SetStateListener([&](uint32_t, const LogicalGamepadState& st) {
        ++publishes;
        last = st;
    });
    hub.SetStick(ControllerSourceId::Touch, 0, LogicalStick::Left, 0.6f, 0.8f);
    CHECK(publishes == 1, "SetStick publishes once");
    CHECK(last.lx != 0 && last.ly > 0, "atomic x and y");
    const uint64_t seq = last.sequence;
    hub.SetStick(ControllerSourceId::Touch, 0, LogicalStick::Left, 0.6f, 0.8f);
    CHECK(publishes == 1, "unchanged stick does not republish");
    CHECK(hub.GetState(0).sequence == seq, "sequence unchanged");

    // Last-active stick: Touch after Physical takes ownership; Touch release falls back.
    hub.SetStick(ControllerSourceId::Physical, 0, LogicalStick::Left, 1.0f, 0.0f);
    hub.SetStick(ControllerSourceId::Touch, 0, LogicalStick::Left, 0.5f, 0.0f);
    CHECK(hub.GetState(0).lx > 10000 && hub.GetState(0).lx < 20000, "touch last-active");
    hub.SetStick(ControllerSourceId::Touch, 0, LogicalStick::Left, 0.0f, 0.0f);
    CHECK(hub.GetState(0).lx > 20000, "physical fallback after touch");

    // Left and right stick owners are independent.
    hub.SetStick(ControllerSourceId::Physical, 0, LogicalStick::Left, 1.0f, 0.0f);
    hub.SetStick(ControllerSourceId::Touch, 0, LogicalStick::Right, 0.0f, 1.0f);
    CHECK(hub.GetState(0).lx > 20000, "left owner physical");
    CHECK(hub.GetState(0).ry > 20000, "right owner touch");
    hub.ResetSource(ControllerSourceId::Physical);
    hub.ResetSource(ControllerSourceId::Touch);
    CHECK(hub.GetState(0).lx == 0 && hub.GetState(0).ry == 0, "reset both sticks");

    // Hat up is canonical +1.
    hub.SetHat(ControllerSourceId::Touch, 0, 0, 1);
    CHECK(hub.GetState(0).hatY == 1, "hat up");
    hub.SetHat(ControllerSourceId::Physical, 0, hatUp.x, hatUp.y);
    CHECK(hub.GetState(0).hatY == 1, "physical hat up");
    hub.SetButton(ControllerSourceId::Touch, 0, LogicalButton::DpadUp, true);
    CHECK(hub.GetState(0).hatY == 1, "dpad up button → hatY +1");
    hub.SetButton(ControllerSourceId::Touch, 0, LogicalButton::DpadUp, false);
    hub.SetHat(ControllerSourceId::Touch, 0, 0, 0);
    hub.SetHat(ControllerSourceId::Physical, 0, 0, 0);

    // WHGP serialization copies canonical ly; no source-based invert.
    hub.SetStick(ControllerSourceId::Touch, 0, LogicalStick::Left, 0.f, 1.f);
    const LogicalGamepadState hubUp = hub.GetState(0);
    const whgp_state_v2 wire = SerializeCanonical(hubUp);
    CHECK(hubUp.ly > 0, "hub canonical up");
    CHECK(wire.ly == hubUp.ly && wire.ly > 0, "WHGP ly stays canonical up");
    CHECK(wire.lx == hubUp.lx && wire.ry == hubUp.ry, "WHGP copies remaining axes");

    // Wine bus_ohos mapping: analog Y passthrough; hat Y inverted for HID helper.
    CHECK(whgp_stick_y_to_hid(12345) == 12345, "wine analog Y passthrough +");
    CHECK(whgp_stick_y_to_hid(-12345) == -12345, "wine analog Y passthrough -");
    CHECK(whgp_stick_y_to_hid(wire.ly) == wire.ly, "wine HID stick Y == WHGP ly");
    CHECK(whgp_hat_y_to_hid(1) == -1, "wine HID hat up");
    CHECK(whgp_hat_y_to_hid(-1) == 1, "wine HID hat down");
    CHECK(whgp_version_matches(2), "accept WHGP v2");
    CHECK(!whgp_version_matches(1), "reject WHGP v1");
    CHECK(!whgp_version_matches(3), "reject unknown WHGP");

    hub.SetStateListener(nullptr);
    hub.SetEnabled(false);
    std::printf("controller_merge_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
