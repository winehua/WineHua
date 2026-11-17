#include "input/controller/physical_gamepad.h"

#include "input/controller/controller_hub.h"
#include "input/controller/controller_types.h"
#include "input/controller/ohos_gamepad_adapter.h"

#include <algorithm>
#include <array>
#include <mutex>
#include <unordered_set>

namespace winehua {
namespace controller {
namespace {

std::mutex deviceMutex;
std::array<std::string, kMaxControllerSlots> deviceSlots;
std::unordered_set<std::string> offlineDevices;

int FindSlotLocked(const std::string& deviceId, bool attach)
{
    if (deviceId.empty() || offlineDevices.count(deviceId)) return -1;
    for (uint32_t slot = 0; slot < kMaxControllerSlots; ++slot)
        if (deviceSlots[slot] == deviceId) return static_cast<int>(slot);
    if (attach) {
        for (uint32_t slot = 0; slot < kMaxControllerSlots; ++slot) {
            if (!deviceSlots[slot].empty()) continue;
            deviceSlots[slot] = deviceId;
            ControllerHub::Instance().SetSlotConnected(slot, true);
            return static_cast<int>(slot);
        }
    }
    return -1;
}

void DetachLocked(uint32_t slot)
{
    if (deviceSlots[slot].empty()) return;
    offlineDevices.insert(deviceSlots[slot]);
    ControllerHub::Instance().ResetSource(ControllerSourceId::Physical, slot);
    ControllerHub::Instance().SetSlotConnected(slot, false);
    deviceSlots[slot].clear();
}

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
        case 2309: return LogicalButton::LB; // L2 digital → LT via SetTrigger
        case 2310: return LogicalButton::RB;
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

void PhysicalFeedButton(const std::string& deviceId, int ohButtonCode, bool pressed)
{
    std::lock_guard<std::mutex> lock(deviceMutex);
    const int slot = FindSlotLocked(deviceId, true);
    if (slot < 0) return;
    if (!ControllerHub::Instance().IsEnabled()) return;
    if (ohButtonCode == 2309) {
        ControllerHub::Instance().SetTrigger(ControllerSourceId::Physical, slot,
                                              LogicalTrigger::Left, pressed ? 1.f : 0.f);
        return;
    }
    if (ohButtonCode == 2310) {
        ControllerHub::Instance().SetTrigger(ControllerSourceId::Physical, slot,
                                              LogicalTrigger::Right, pressed ? 1.f : 0.f);
        return;
    }
    const LogicalButton btn = MapOhButton(ohButtonCode);
    if (btn == LogicalButton::Count) return;
    ControllerHub::Instance().SetButton(ControllerSourceId::Physical, slot, btn, pressed);
}

void PhysicalFeedAxis(const std::string& deviceId, int axisType, double x, double y)
{
    std::lock_guard<std::mutex> lock(deviceMutex);
    const int slot = FindSlotLocked(deviceId, true);
    if (slot < 0) return;
    if (!ControllerHub::Instance().IsEnabled()) return;
    auto& hub = ControllerHub::Instance();
    switch (static_cast<PhysicalControlAxis>(axisType)) {
        case PhysicalControlAxis::LeftStick: {
            const Stick2D stick = NormalizeOhosThumb(x, y);
            hub.SetStick(ControllerSourceId::Physical, slot, LogicalStick::Left, stick.x, stick.y);
            break;
        }
        case PhysicalControlAxis::RightStick: {
            const Stick2D stick = NormalizeOhosThumb(x, y);
            hub.SetStick(ControllerSourceId::Physical, slot, LogicalStick::Right, stick.x, stick.y);
            break;
        }
        case PhysicalControlAxis::Dpad: {
            const CanonicalHat hat = NormalizeOhosHat(x, y);
            hub.SetHat(ControllerSourceId::Physical, slot, hat.x, hat.y);
            break;
        }
        case PhysicalControlAxis::LeftTrigger:
            hub.SetTrigger(ControllerSourceId::Physical, slot, LogicalTrigger::Left,
                           NormalizeOhosTrigger(x));
            break;
        case PhysicalControlAxis::RightTrigger:
            hub.SetTrigger(ControllerSourceId::Physical, slot, LogicalTrigger::Right,
                           NormalizeOhosTrigger(x));
            break;
        default:
            break;
    }
}

void PhysicalFeedDevice(const std::string& deviceId, bool connected)
{
    if (deviceId.empty()) return;
    std::lock_guard<std::mutex> lock(deviceMutex);
    if (connected) {
        offlineDevices.erase(deviceId);
        FindSlotLocked(deviceId, true);
    } else {
        const int slot = FindSlotLocked(deviceId, false);
        if (slot >= 0) DetachLocked(static_cast<uint32_t>(slot));
        offlineDevices.insert(deviceId);
    }
}

void PhysicalSyncDevices(const std::vector<std::string>& deviceIds)
{
    std::lock_guard<std::mutex> lock(deviceMutex);
    // Release missing devices first, then fill holes without renumbering survivors.
    for (uint32_t slot = 0; slot < kMaxControllerSlots; ++slot) {
        if (!deviceSlots[slot].empty() &&
            std::find(deviceIds.begin(), deviceIds.end(), deviceSlots[slot]) == deviceIds.end())
            DetachLocked(slot);
    }
    for (const auto& id : deviceIds) {
        offlineDevices.erase(id);
        FindSlotLocked(id, true);
    }
}

void PhysicalResetDevices()
{
    std::lock_guard<std::mutex> lock(deviceMutex);
    for (uint32_t slot = 0; slot < kMaxControllerSlots; ++slot) DetachLocked(slot);
    offlineDevices.clear();
}

std::string PhysicalDeviceForSlot(uint32_t slot)
{
    std::lock_guard<std::mutex> lock(deviceMutex);
    return slot < kMaxControllerSlots ? deviceSlots[slot] : std::string{};
}

}  // namespace controller
}  // namespace winehua
