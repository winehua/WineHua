#pragma once

#include <cstdint>

namespace winehua::wineipc {

constexpr int32_t kVersion = 1;
constexpr uint32_t kBootstrap = 1;
constexpr int32_t kMaxFds = 16;
constexpr char kProbeParams[] = "__winehua_direct_ipc_probe__";
constexpr uint32_t kProbeToken = 0x57484950; // WHIP

struct ProbeResult {
    int32_t pid;
    int32_t status;
    uint32_t token;
    int32_t fdCount;
};

} // namespace winehua::wineipc
