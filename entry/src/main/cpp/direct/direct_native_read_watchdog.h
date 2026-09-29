#pragma once
#include <memory>

namespace winehua::direct::shared {
// Diagnostic only: use the documented signal-safe HiDebug FP unwinder, then
// symbolize on a separate thread. No remote debugger or system permission.
class NativeReadWatchdog {
public:
    NativeReadWatchdog();
    ~NativeReadWatchdog();
    void Arm();
    void Disarm();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
