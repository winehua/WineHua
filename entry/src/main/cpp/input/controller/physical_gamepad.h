#pragma once

namespace winehua {
namespace controller {

// Feed Game Controller Kit events into ControllerHub (Physical source).
void PhysicalFeedButton(int ohButtonCode, bool pressed);
void PhysicalFeedAxis(int axisType, double x, double y);
void PhysicalFeedDevice(bool connected);

}  // namespace controller
}  // namespace winehua
