#pragma once

#include <cstdint>

namespace winehua::direct {

constexpr uint32_t kVulkanProbeMagic = 0x4430564b; // D0VK
constexpr uint32_t kVulkanProbeVersion = 2;
constexpr char kVulkanProbeFdName[] = "direct_probe_result";
constexpr uint32_t kVulkanProbeReadRequest = 1;
constexpr uint32_t kVulkanProbeFinishRequest = 2;

// One fixed-size write over the NCP result fd. No pointers cross the process boundary.
struct VulkanProbeResult {
    uint32_t magic = kVulkanProbeMagic;
    uint32_t version = kVulkanProbeVersion;
    uint32_t size = sizeof(VulkanProbeResult);
    int32_t status = -1;
    int32_t pid = -1;
    int32_t vkResult = 0;
    uint32_t loaderVersion = 0;
    uint32_t requestedApiVersion = 0;
    uint32_t instanceExtensionCount = 0;
    uint32_t apiVersion = 0;
    uint32_t pixelCheck = 0;
    uint64_t elapsedMs = 0;
    char stage[48] = {};
    char loaderPath[256] = {};
    char deviceName[256] = {};
    char icdEnvironment[256] = {};
};

} // namespace winehua::direct
