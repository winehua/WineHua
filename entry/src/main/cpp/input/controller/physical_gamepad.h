#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace winehua {
namespace controller {

// Feed Game Controller Kit events into ControllerHub (Physical source).
void PhysicalFeedButton(const std::string& deviceId, int ohButtonCode, bool pressed);
void PhysicalFeedAxis(const std::string& deviceId, int axisType, double x, double y);
void PhysicalFeedDevice(const std::string& deviceId, bool connected);
void PhysicalSyncDevices(const std::vector<std::string>& deviceIds);
void PhysicalResetDevices();
std::string PhysicalDeviceForSlot(uint32_t slot);

}  // namespace controller
}  // namespace winehua
