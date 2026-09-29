#pragma once

#include <cstdint>

namespace winehua::direct {

constexpr uint32_t kVulkanProbeMagic = 0x4430564b; // D0VK
constexpr uint32_t kVulkanProbeVersion = 4;
constexpr char kVulkanProbeFdName[] = "direct_probe_result";
constexpr char kVulkanProbeStageFdName[] = "direct_probe_stage";
constexpr uint32_t kVulkanProbeReadRequest = 1;
constexpr uint32_t kVulkanProbeFinishRequest = 2;

enum VulkanCapability : uint32_t {
    kInstanceSurface = 1u << 0,
    kInstanceOhosSurface = 1u << 1,
    kDeviceSwapchain = 1u << 2,
    kDeviceOhosExternalMemory = 1u << 3,
    kDeviceExternalSemaphoreFd = 1u << 4,
    kDeviceExternalMemoryFd = 1u << 5,
    kDeviceForeignQueueFamily = 1u << 6,
    kSyncFdExportable = 1u << 7,
    kSyncFdImportable = 1u << 8,
    kOhosImageImportable = 1u << 9,
    kOpaqueFdImageImportable = 1u << 10,
    kOpaqueFdImageExportable = 1u << 11,
    kOpaqueFdImageDedicatedOnly = 1u << 12,
    kDeviceExternalMemoryDmaBuf = 1u << 13,
};

constexpr uint32_t kExternalImageProbeCount = 10;
struct ExternalImageProbeResult {
    uint32_t handleType = 0;
    uint32_t format = 0;
    uint32_t tiling = 0;
    uint32_t usage = 0;
    int32_t result = -7; // VK_ERROR_EXTENSION_NOT_PRESENT until queried
    uint32_t features = 0;
    uint32_t compatibleHandleTypes = 0;
    uint32_t exportFromImportedHandleTypes = 0;
};

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
    uint32_t nativeCapabilities = 0;
    uint32_t deviceExtensionCount = 0;
    uint32_t externalImageCount = 0;
    ExternalImageProbeResult externalImages[kExternalImageProbeCount]{};
    uint32_t opaqueFdBufferQueried = 0;
    uint32_t opaqueFdBufferFeatures = 0;
    uint64_t elapsedMs = 0;
    char stage[48] = {};
    char loaderPath[256] = {};
    char deviceName[256] = {};
    char icdEnvironment[256] = {};
};

} // namespace winehua::direct
