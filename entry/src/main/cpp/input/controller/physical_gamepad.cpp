#include "input/controller/physical_gamepad.h"

#include "input/controller/controller_hub.h"
#include "input/controller/controller_types.h"
#include "input/controller/ohos_gamepad_adapter.h"

namespace winehua {
namespace controller {
namespace {

enum class PhysicalControlAxis {
    LeftStick = 0,
    RightStick = 1,
    Dpad = 2,
    LeftTrigger = 3,
    RightTrigger = 4,
};

LogicalButton MapOhButton(int code)
{
    switch (code) {
        case 2301: return LogicalButton::A;
        case 2302: return LogicalButton::B;
        case 2304: return LogicalButton::X;
        case 2305: return LogicalButton::Y;
        case 2307: return LogicalButton::LB;
        case 2308: return LogicalButton::RB;
        case 2311: return LogicalButton::Back;
        case 2312: return LogicalButton::Start;
        case 2313: return LogicalButton::L3;
        case 2314: return LogicalButton::R3;
        case 2315: return LogicalButton::Guide;
        case 2012: return LogicalButton::DpadUp;
        case 2013: return LogicalButton::DpadDown;
        case 2014: return LogicalButton::DpadLeft;
        case 2015: return LogicalButton::DpadRight;
        default: return LogicalButton::Count;
    }
}

}  // namespace

void PhysicalFeedButton(int ohButtonCode, bool pressed)
{
    if (!ControllerHub::Instance().IsEnabled()) return;
    if (ohButtonCode == 2309) {
        ControllerHub::Instance().SetTrigger(ControllerSourceId::Physical, 0,
                                              LogicalTrigger::Left, pressed ? 1.f : 0.f);
        return;
    }
    if (ohButtonCode == 2310) {
        ControllerHub::Instance().SetTrigger(ControllerSourceId::Physical, 0,
                                              LogicalTrigger::Right, pressed ? 1.f : 0.f);
        return;
    }
    const LogicalButton btn = MapOhButton(ohButtonCode);
    if (btn == LogicalButton::Count) return;
    ControllerHub::Instance().SetButton(ControllerSourceId::Physical, 0, btn, pressed);
}

void PhysicalFeedAxis(int axisType, double x, double y)
{
    if (!ControllerHub::Instance().IsEnabled()) return;
    auto& hub = ControllerHub::Instance();
    switch (static_cast<PhysicalControlAxis>(axisType)) {
        case PhysicalControlAxis::LeftStick: {
            const Stick2D stick = NormalizeOhosThumb(x, y);
            hub.SetStick(ControllerSourceId::Physical, 0, LogicalStick::Left, stick.x, stick.y);
            break;
        }
        case PhysicalControlAxis::RightStick: {
            const Stick2D stick = NormalizeOhosThumb(x, y);
            hub.SetStick(ControllerSourceId::Physical, 0, LogicalStick::Right, stick.x, stick.y);
            break;
        }
        case PhysicalControlAxis::Dpad: {
            const CanonicalHat hat = NormalizeOhosHat(x, y);
            hub.SetHat(ControllerSourceId::Physical, 0, hat.x, hat.y);
            break;
        }
        case PhysicalControlAxis::LeftTrigger:
            hub.SetTrigger(ControllerSourceId::Physical, 0, LogicalTrigger::Left,
                           NormalizeOhosTrigger(x));
            break;
        case PhysicalControlAxis::RightTrigger:
            hub.SetTrigger(ControllerSourceId::Physical, 0, LogicalTrigger::Right,
                           NormalizeOhosTrigger(x));
            break;
        default:
            break;
    }
}

void PhysicalFeedDevice(bool connected)
{
    if (!connected) {
        ControllerHub::Instance().ResetSource(ControllerSourceId::Physical);
    }
}

}  // namespace controller
}  // namespace winehua
