#define VK_USE_PLATFORM_OHOS 1

#include "host_vulkan_probe.h"

#include <native_window/external_window.h>
#include <vulkan/vulkan.h>
#include <hilog/log.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <utility>
#include <unistd.h>
#include <vector>

#include "../../../../smoke/vkd3d_capability_audit.h"

#undef LOG_TAG
#define LOG_TAG "WL_VK_PROBE"

namespace {

using Clock = std::chrono::steady_clock;

std::atomic<bool> gRunning{false};
std::atomic<bool> gCancel{false};

std::string JsonEscape(const char* value)
{
    std::string out;
    for (const unsigned char c : std::string(value ? value : "")) {
        switch (c) {
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c >= 0x20) out += static_cast<char>(c);
            break;
        }
    }
    return out;
}

bool SafeId(const std::string& value)
{
    if (value.empty() || value.size() > 120) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return std::isalnum(c) || c == '-' || c == '_' || c == '.';
    });
}

bool EnsureDir(const std::string& path)
{
    struct stat st{};
    if (stat(path.c_str(), &st) == 0) return S_ISDIR(st.st_mode);
    return mkdir(path.c_str(), 0755) == 0;
}

bool WriteAtomic(const std::string& path, const std::string& text)
{
    std::string temporary = path + ".tmp." + std::to_string(getpid());
    int fd = open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) return false;
    const char* data = text.data();
    size_t remaining = text.size();
    bool ok = true;
    while (remaining) {
        ssize_t written = write(fd, data, remaining);
        if (written <= 0) { ok = false; break; }
        data += written;
        remaining -= static_cast<size_t>(written);
    }
    if (ok) ok = fsync(fd) == 0;
    close(fd);
    if (ok) ok = rename(temporary.c_str(), path.c_str()) == 0;
    if (!ok) unlink(temporary.c_str());
    return ok;
}

struct ProbeMetrics {
    uint64_t queueSubmitCount = 0;
    uint64_t gpuCopyCount = 0;
    uint64_t acquireWaitUs = 0;
    uint64_t renderWaitUs = 0;
    uint64_t presentWaitUs = 0;
    uint32_t swapchainRecreateCount = 0;
    std::vector<double> frameMs;
};

struct ProbeCaps {
    uint32_t loaderApiVersion = VK_API_VERSION_1_0;
    VkPhysicalDeviceProperties properties{};
    VkPhysicalDeviceVulkan12Properties vulkan12Properties{};
    VkPhysicalDeviceFeatures features{};
    VkSurfaceCapabilitiesKHR surface{};
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR;
    uint32_t graphicsQueueFamily = UINT32_MAX;
    uint32_t presentQueueFamily = UINT32_MAX;
    uint32_t surfaceFormatCount = 0;
    uint32_t presentModeCount = 0;
    bool bc1 = false;
    bool bc2 = false;
    bool bc3 = false;
    bool bc4 = false;
    bool bc5 = false;
    bool bc6 = false;
    bool bc7 = false;
    bool etc2 = false;
    bool astc4x4 = false;
    bool astc8x8 = false;
    bool descriptorIndexing = false;
    bool bufferDeviceAddress = false;
    bool scalarBlockLayout = false;
    bool robustness2 = false;
    bool transformFeedback = false;
    bool shaderInt8 = false;
    bool timelineSemaphore = false;
    bool timelineRoundTripAttempted = false;
    bool timelineRoundTripPassed = false;
    VkResult timelineCreateResult = VK_NOT_READY;
    VkResult timelineSubmitResult = VK_NOT_READY;
    VkResult timelineWaitResult = VK_NOT_READY;
    VkResult timelineCounterResult = VK_NOT_READY;
    uint64_t timelineObservedValue = 0;
    bool synchronization2 = false;
    bool dynamicRendering = false;
    bool maintenance4 = false;
    bool maintenance5 = false;
    bool maintenance6 = false;
    bool presentWait = false;
    bool swapchainMaintenance = false;
    bool customBorderColorExtension = false;
    bool customBorderColors = false;
    bool customBorderColorWithoutFormat = false;
    std::string capabilityAudit;
};

double Percentile(std::vector<double> values, double percentile)
{
    if (values.empty()) return -1.0;
    std::sort(values.begin(), values.end());
    size_t index = static_cast<size_t>(std::ceil(percentile * values.size())) - 1;
    return values[std::min(index, values.size() - 1)];
}

std::string MakeResult(const std::string& runId, const char* status, const char* message,
                       const ProbeCaps& caps, const ProbeMetrics& metrics)
{
    const double p50 = Percentile(metrics.frameMs, 0.50);
    const double p95 = Percentile(metrics.frameMs, 0.95);
    const double p99 = Percentile(metrics.frameMs, 0.99);
    std::ostringstream out;
    out.setf(std::ios::fixed);
    out.precision(3);
    out << "{\n"
        << "  \"schemaVersion\": 1,\n"
        << "  \"runId\": \"" << JsonEscape(runId.c_str()) << "\",\n"
        << "  \"testId\": \"host-vulkan\",\n"
        << "  \"status\": \"" << status << "\",\n"
        << "  \"stage\": \"host-vulkan\",\n"
        << "  \"message\": \"" << JsonEscape(message) << "\",\n"
        << "  \"architecture\": {\"peArchitecture\":\"not-applicable\","
           "\"wineUnixArchitecture\":\"not-applicable\","
           "\"vulkanLoaderArchitecture\":\"aarch64\","
           "\"venusIcdArchitecture\":\"not-applicable\","
           "\"hostArchitecture\":\"aarch64\","
           "\"wow64ThunkEnabled\":false,\"box64Enabled\":false},\n"
        << "  \"capabilities\": {\n"
        << "    \"loaderApiVersion\": \"" << VK_VERSION_MAJOR(caps.loaderApiVersion) << "."
        << VK_VERSION_MINOR(caps.loaderApiVersion) << "." << VK_VERSION_PATCH(caps.loaderApiVersion) << "\",\n"
        << "    \"deviceApiVersion\": \"" << VK_VERSION_MAJOR(caps.properties.apiVersion) << "."
        << VK_VERSION_MINOR(caps.properties.apiVersion) << "." << VK_VERSION_PATCH(caps.properties.apiVersion) << "\",\n"
        << "    \"deviceName\": \"" << JsonEscape(caps.properties.deviceName) << "\",\n"
        << "    \"vendorId\": " << caps.properties.vendorID << ",\n"
        << "    \"deviceId\": " << caps.properties.deviceID << ",\n"
        << "    \"driverVersion\": " << caps.properties.driverVersion << ",\n"
        << "    \"graphicsQueueFamily\": " << caps.graphicsQueueFamily << ",\n"
        << "    \"presentQueueFamily\": " << caps.presentQueueFamily << ",\n"
        << "    \"surfaceFormatCount\": " << caps.surfaceFormatCount << ",\n"
        << "    \"presentModeCount\": " << caps.presentModeCount << ",\n"
        << "    \"surfaceUsageFlags\": " << caps.surface.supportedUsageFlags << ",\n"
        << "    \"maxImageExtent\": [" << caps.surface.maxImageExtent.width << ","
        << caps.surface.maxImageExtent.height << "],\n"
        << "    \"pushConstantBytes\": " << caps.properties.limits.maxPushConstantsSize << ",\n"
        << "    \"geometryShader\": " << (caps.features.geometryShader ? "true" : "false") << ",\n"
        << "    \"tessellationShader\": " << (caps.features.tessellationShader ? "true" : "false") << ",\n"
        << "    \"multiDrawIndirect\": " << (caps.features.multiDrawIndirect ? "true" : "false") << ",\n"
        << "    \"descriptorIndexing\": " << (caps.descriptorIndexing ? "true" : "false") << ",\n"
        << "    \"bufferDeviceAddress\": " << (caps.bufferDeviceAddress ? "true" : "false") << ",\n"
        << "    \"updateAfterBindLimits\": {\"maxUpdateAfterBindDescriptorsInAllPools\":"
        << caps.vulkan12Properties.maxUpdateAfterBindDescriptorsInAllPools
        << ",\"maxDescriptorSetUpdateAfterBindSampledImages\":"
        << caps.vulkan12Properties.maxDescriptorSetUpdateAfterBindSampledImages
        << ",\"maxDescriptorSetUpdateAfterBindStorageImages\":"
        << caps.vulkan12Properties.maxDescriptorSetUpdateAfterBindStorageImages
        << ",\"maxDescriptorSetUpdateAfterBindStorageBuffers\":"
        << caps.vulkan12Properties.maxDescriptorSetUpdateAfterBindStorageBuffers << "},\n"
        << "    \"scalarBlockLayout\": " << (caps.scalarBlockLayout ? "true" : "false") << ",\n"
        << "    \"robustness2\": " << (caps.robustness2 ? "true" : "false") << ",\n"
        << "    \"transformFeedback\": " << (caps.transformFeedback ? "true" : "false") << ",\n"
        << "    \"shaderInt8\": " << (caps.shaderInt8 ? "true" : "false") << ",\n"
        << "    \"shaderInt16\": " << (caps.features.shaderInt16 ? "true" : "false") << ",\n"
        << "    \"shaderInt64\": " << (caps.features.shaderInt64 ? "true" : "false") << ",\n"
        << "    \"timelineSemaphore\": " << (caps.timelineSemaphore ? "true" : "false") << ",\n"
        << "    \"timelineRoundTrip\": {\"attempted\":"
        << (caps.timelineRoundTripAttempted ? "true" : "false")
        << ",\"passed\":" << (caps.timelineRoundTripPassed ? "true" : "false")
        << ",\"createResult\":" << caps.timelineCreateResult
        << ",\"submitResult\":" << caps.timelineSubmitResult
        << ",\"waitResult\":" << caps.timelineWaitResult
        << ",\"counterResult\":" << caps.timelineCounterResult
        << ",\"observedValue\":" << caps.timelineObservedValue << "},\n"
        << "    \"synchronization2\": " << (caps.synchronization2 ? "true" : "false") << ",\n"
        << "    \"dynamicRendering\": " << (caps.dynamicRendering ? "true" : "false") << ",\n"
        << "    \"maintenance4\": " << (caps.maintenance4 ? "true" : "false") << ",\n"
        << "    \"maintenance5\": " << (caps.maintenance5 ? "true" : "false") << ",\n"
        << "    \"maintenance6\": " << (caps.maintenance6 ? "true" : "false") << ",\n"
        << "    \"presentWait\": " << (caps.presentWait ? "true" : "false") << ",\n"
        << "    \"swapchainMaintenance\": " << (caps.swapchainMaintenance ? "true" : "false") << ",\n"
        << "    \"customBorderColorExtension\": " << (caps.customBorderColorExtension ? "true" : "false") << ",\n"
        << "    \"customBorderColors\": " << (caps.customBorderColors ? "true" : "false") << ",\n"
        << "    \"customBorderColorWithoutFormat\": " << (caps.customBorderColorWithoutFormat ? "true" : "false") << ",\n"
        << "    \"bc1\": " << (caps.bc1 ? "true" : "false") << ",\n"
        << "    \"bc2\": " << (caps.bc2 ? "true" : "false") << ",\n"
        << "    \"bc3\": " << (caps.bc3 ? "true" : "false") << ",\n"
        << "    \"bc4\": " << (caps.bc4 ? "true" : "false") << ",\n"
        << "    \"bc5\": " << (caps.bc5 ? "true" : "false") << ",\n"
        << "    \"bc6\": " << (caps.bc6 ? "true" : "false") << ",\n"
        << "    \"bc7\": " << (caps.bc7 ? "true" : "false") << ",\n"
        << "    \"etc2\": " << (caps.etc2 ? "true" : "false") << ",\n"
        << "    \"astc4x4\": " << (caps.astc4x4 ? "true" : "false") << ",\n"
        << "    \"astc8x8\": " << (caps.astc8x8 ? "true" : "false") << "\n"
        << "  },\n"
        << "  \"capabilityAudit\": "
        << (caps.capabilityAudit.empty() ? "{}" : caps.capabilityAudit) << ",\n"
        << "  \"metrics\": {"
        << "\"cpuReadBytes\":0,\"cpuUploadBytes\":0,"
        << "\"gpuCopyCount\":" << metrics.gpuCopyCount << ","
        << "\"queueSubmitCount\":" << metrics.queueSubmitCount << ","
        << "\"acquireWaitUs\":" << metrics.acquireWaitUs << ","
        << "\"renderWaitUs\":" << metrics.renderWaitUs << ","
        << "\"presentWaitUs\":" << metrics.presentWaitUs << ","
        << "\"frameLatency\":" << p50 << ","
        << "\"p50FrameMs\":" << p50 << ","
        << "\"p95FrameMs\":" << p95 << ","
        << "\"p99FrameMs\":" << p99 << ","
        << "\"swapchainRecreateCount\":" << metrics.swapchainRecreateCount << ","
        << "\"surfaceQueueBacklog\":0,\"fallbackDetected\":false,"
        << "\"perFrameDeviceWaitIdle\":0,\"fixedFrame\":\"rgba-quadrants-v1\"}\n"
        << "}\n";
    return out.str();
}

void PublishResult(const std::string& runId, const char* status, const char* message,
                   const ProbeCaps& caps, const ProbeMetrics& metrics, bool final)
{
    const std::string base = "/data/storage/el2/base/files/automation";
    const std::string root = base + "/results";
    const std::string dir = root + "/" + runId;
    EnsureDir(base);
    EnsureDir(root);
    EnsureDir(dir);
    const std::string result = MakeResult(runId, status, message, caps, metrics);
    WriteAtomic(dir + "/host-vulkan.json", result);
    if (!final) return;

    std::ostringstream summary;
    summary << "{\n  \"schemaVersion\":1,\n  \"runId\":\"" << JsonEscape(runId.c_str())
            << "\",\n  \"suite\":\"host-vulkan\",\n  \"prefixMode\":\"reuse\",\n"
            << "  \"status\":\"" << status << "\",\n  \"tests\":[" << result << "]\n}\n";
    WriteAtomic(dir + "/suite-summary.json", summary.str());
}

bool HasExtension(const std::vector<VkExtensionProperties>& extensions, const char* name)
{
    return std::any_of(extensions.begin(), extensions.end(), [name](const auto& ext) {
        return strcmp(ext.extensionName, name) == 0;
    });
}

class VulkanProbe {
public:
    VulkanProbe(OHNativeWindow* window, std::string runId)
        : window_(window), runId_(std::move(runId)) {}

    ~VulkanProbe() { Cleanup(); }

    void Run()
    {
        const char* failure = nullptr;
        bool unsupported = false;
        if (!InitInstance(failure, unsupported) || !InitDevice(failure, unsupported) ||
            !RunTimelineRoundTrip(failure, unsupported) ||
            !CreateSwapchain(failure, unsupported) || !CreateCommands(failure)) {
            PublishResult(runId_, unsupported ? "UNSUPPORTED" : "FAIL",
                          failure ? failure : "Host Vulkan initialization failed", caps_, metrics_, true);
            return;
        }

        bool fixedPublished = false;
        for (uint32_t frame = 0; frame < 120 && !gCancel.load(); ++frame) {
            if (frame == 60) {
                vkDeviceWaitIdle(device_);
                DestroySwapchain();
                if (!CreateSwapchain(failure, unsupported)) {
                    PublishResult(runId_, "FAIL", failure ? failure : "swapchain recreate failed",
                                  caps_, metrics_, true);
                    return;
                }
                metrics_.swapchainRecreateCount++;
            }
            if (!DrawFrame(failure)) {
                PublishResult(runId_, "FAIL", failure ? failure : "queue/present failed",
                              caps_, metrics_, true);
                return;
            }
            if (!fixedPublished && frame >= 5) {
                PublishResult(runId_, "started", "fixed-frame", caps_, metrics_, false);
                fixedPublished = true;
            }
        }

        if (gCancel.load()) {
            PublishResult(runId_, "FAIL", "surface destroyed during host Vulkan probe",
                          caps_, metrics_, true);
            return;
        }
        PublishResult(runId_, "PASS", "Host Vulkan swapchain rendered and presented",
                      caps_, metrics_, true);
        OH_LOG_INFO(LOG_APP, "[HostVulkan] PASS device=%{public}s frames=%{public}zu recreate=%{public}u",
                    caps_.properties.deviceName, metrics_.frameMs.size(), metrics_.swapchainRecreateCount);

        // Keep the deterministic final frame and swapchain alive long enough for
        // the host-side snapshot validator. The App is force-stopped afterwards.
        for (int i = 0; i < 100 && !gCancel.load(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

private:
    void QueryExtendedCapabilities(const std::vector<VkExtensionProperties>& extensions)
    {
        auto formatSupported = [this](VkFormat format) {
            VkFormatProperties properties{};
            vkGetPhysicalDeviceFormatProperties(physical_, format, &properties);
            return (properties.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0;
        };
        caps_.bc1 = formatSupported(VK_FORMAT_BC1_RGBA_UNORM_BLOCK);
        caps_.bc2 = formatSupported(VK_FORMAT_BC2_UNORM_BLOCK);
        caps_.bc3 = formatSupported(VK_FORMAT_BC3_UNORM_BLOCK);
        caps_.bc4 = formatSupported(VK_FORMAT_BC4_UNORM_BLOCK);
        caps_.bc5 = formatSupported(VK_FORMAT_BC5_UNORM_BLOCK);
        caps_.bc6 = formatSupported(VK_FORMAT_BC6H_UFLOAT_BLOCK);
        caps_.bc7 = formatSupported(VK_FORMAT_BC7_UNORM_BLOCK);
        caps_.etc2 = formatSupported(VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK);
        caps_.astc4x4 = formatSupported(VK_FORMAT_ASTC_4x4_UNORM_BLOCK);
        caps_.astc8x8 = formatSupported(VK_FORMAT_ASTC_8x8_UNORM_BLOCK);

        VkPhysicalDeviceFeatures2 features2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        VkPhysicalDeviceVulkan11Features vulkan11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
        VkPhysicalDeviceVulkan12Features vulkan12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
        VkPhysicalDeviceVulkan13Features vulkan13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        VkPhysicalDeviceRobustness2FeaturesEXT robustness2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT};
        VkPhysicalDeviceTransformFeedbackFeaturesEXT transformFeedback{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT};
        VkPhysicalDeviceSynchronization2Features synchronization2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES};
        VkPhysicalDeviceDynamicRenderingFeatures dynamicRendering{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES};
        VkPhysicalDeviceMaintenance4Features maintenance4{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_FEATURES};
        VkPhysicalDeviceMaintenance5Features maintenance5{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES};
        VkPhysicalDeviceMaintenance6Features maintenance6{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_6_FEATURES};
        VkPhysicalDeviceCustomBorderColorFeaturesEXT customBorderColor{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT};
        void** tail = &features2.pNext;
        auto append = [&tail](auto& feature, bool supported) {
            if (!supported) return;
            *tail = &feature;
            tail = &feature.pNext;
        };
        const bool api12 = caps_.properties.apiVersion >= VK_API_VERSION_1_2;
        const bool api13 = caps_.properties.apiVersion >= VK_API_VERSION_1_3;
        const bool hasRobustness2 = HasExtension(extensions, VK_EXT_ROBUSTNESS_2_EXTENSION_NAME);
        const bool hasTransformFeedback = HasExtension(extensions, VK_EXT_TRANSFORM_FEEDBACK_EXTENSION_NAME);
        const bool hasSynchronization2 = api13 || HasExtension(extensions, VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME);
        const bool hasDynamicRendering = api13 || HasExtension(extensions, VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME);
        const bool hasMaintenance4 = api13 || HasExtension(extensions, VK_KHR_MAINTENANCE_4_EXTENSION_NAME);
        const bool hasMaintenance5 = HasExtension(extensions, VK_KHR_MAINTENANCE_5_EXTENSION_NAME);
        const bool hasMaintenance6 = HasExtension(extensions, VK_KHR_MAINTENANCE_6_EXTENSION_NAME);
        const bool hasCustomBorderColor = HasExtension(
            extensions, VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME);
        append(vulkan11, caps_.properties.apiVersion >= VK_API_VERSION_1_1);
        append(vulkan12, api12);
        append(vulkan13, api13);
        append(robustness2, hasRobustness2);
        append(transformFeedback, hasTransformFeedback);
        append(synchronization2, !api13 && hasSynchronization2);
        append(dynamicRendering, !api13 && hasDynamicRendering);
        append(maintenance4, !api13 && hasMaintenance4);
        append(maintenance5, hasMaintenance5);
        append(maintenance6, hasMaintenance6);
        append(customBorderColor, hasCustomBorderColor);
        vkGetPhysicalDeviceFeatures2(physical_, &features2);
        VkPhysicalDeviceProperties2 properties2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        VkPhysicalDeviceIDProperties idProperties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
        caps_.vulkan12Properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES;
        properties2.pNext = &caps_.vulkan12Properties;
        caps_.vulkan12Properties.pNext = &idProperties;
        vkGetPhysicalDeviceProperties2(physical_, &properties2);

        caps_.descriptorIndexing = api12 && vulkan12.descriptorIndexing;
        caps_.bufferDeviceAddress = api12 && vulkan12.bufferDeviceAddress;
        caps_.scalarBlockLayout = api12 && vulkan12.scalarBlockLayout;
        caps_.shaderInt8 = api12 && vulkan12.shaderInt8;
        caps_.timelineSemaphore = api12 && vulkan12.timelineSemaphore;
        caps_.robustness2 = hasRobustness2 && robustness2.robustBufferAccess2;
        caps_.transformFeedback = hasTransformFeedback && transformFeedback.transformFeedback;
        caps_.synchronization2 = api13 ? vulkan13.synchronization2 :
            (hasSynchronization2 && synchronization2.synchronization2);
        caps_.dynamicRendering = api13 ? vulkan13.dynamicRendering :
            (hasDynamicRendering && dynamicRendering.dynamicRendering);
        caps_.maintenance4 = api13 ? vulkan13.maintenance4 :
            (hasMaintenance4 && maintenance4.maintenance4);
        caps_.maintenance5 = hasMaintenance5 && maintenance5.maintenance5;
        caps_.maintenance6 = hasMaintenance6 && maintenance6.maintenance6;
        caps_.presentWait = HasExtension(extensions, VK_KHR_PRESENT_WAIT_EXTENSION_NAME);
        caps_.swapchainMaintenance = HasExtension(extensions, VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME);
        caps_.customBorderColorExtension = hasCustomBorderColor;
        caps_.customBorderColors = hasCustomBorderColor &&
            customBorderColor.customBorderColors;
        caps_.customBorderColorWithoutFormat = hasCustomBorderColor &&
            customBorderColor.customBorderColorWithoutFormat;
        if (char *audit = winehua_vkd3d_capability_audit(
                physical_, extensions.data(), static_cast<uint32_t>(extensions.size()),
                &vulkan11, &vulkan12, &vulkan13, &caps_.vulkan12Properties, &idProperties)) {
            caps_.capabilityAudit.assign(audit);
            free(audit);
        }
        caps_.vulkan12Properties.pNext = nullptr;
    }

    bool InitInstance(const char*& failure, bool& unsupported)
    {
        auto enumerateVersion = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(
            vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkEnumerateInstanceVersion"));
        if (enumerateVersion) enumerateVersion(&caps_.loaderApiVersion);

        uint32_t count = 0;
        vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
        std::vector<VkExtensionProperties> extensions(count);
        vkEnumerateInstanceExtensionProperties(nullptr, &count, extensions.data());
        if (!HasExtension(extensions, VK_KHR_SURFACE_EXTENSION_NAME) ||
            !HasExtension(extensions, VK_OHOS_SURFACE_EXTENSION_NAME)) {
            failure = "VK_KHR_surface or VK_OHOS_surface is unavailable";
            unsupported = true;
            return false;
        }

        const char* enabled[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_OHOS_SURFACE_EXTENSION_NAME};
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName = "WineHua Host Vulkan Smoke";
        // Negotiate the same Vulkan generation as Venus so its advertised
        // timeline semaphore capability is exercised natively as well.
        app.apiVersion = std::min(caps_.loaderApiVersion, VK_API_VERSION_1_3);
        VkInstanceCreateInfo create{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        create.pApplicationInfo = &app;
        create.enabledExtensionCount = 2;
        create.ppEnabledExtensionNames = enabled;
        if (vkCreateInstance(&create, nullptr, &instance_) != VK_SUCCESS) {
            failure = "vkCreateInstance failed";
            return false;
        }
        VkSurfaceCreateInfoOHOS surfaceInfo{VK_STRUCTURE_TYPE_SURFACE_CREATE_INFO_OHOS};
        surfaceInfo.window = window_;
        if (vkCreateSurfaceOHOS(instance_, &surfaceInfo, nullptr, &surface_) != VK_SUCCESS) {
            failure = "vkCreateSurfaceOHOS failed";
            unsupported = true;
            return false;
        }
        return true;
    }

    bool InitDevice(const char*& failure, bool& unsupported)
    {
        uint32_t count = 0;
        if (vkEnumeratePhysicalDevices(instance_, &count, nullptr) != VK_SUCCESS || !count) {
            failure = "vkEnumeratePhysicalDevices returned no device";
            unsupported = true;
            return false;
        }
        std::vector<VkPhysicalDevice> devices(count);
        vkEnumeratePhysicalDevices(instance_, &count, devices.data());

        std::vector<VkExtensionProperties> selectedExtensions;
        for (VkPhysicalDevice candidate : devices) {
            uint32_t queueCount = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &queueCount, nullptr);
            std::vector<VkQueueFamilyProperties> queues(queueCount);
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &queueCount, queues.data());
            uint32_t graphics = UINT32_MAX, present = UINT32_MAX;
            for (uint32_t i = 0; i < queueCount; ++i) {
                VkBool32 supported = VK_FALSE;
                vkGetPhysicalDeviceSurfaceSupportKHR(candidate, i, surface_, &supported);
                if ((queues[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && graphics == UINT32_MAX) graphics = i;
                if (supported && present == UINT32_MAX) present = i;
            }
            uint32_t extCount = 0;
            vkEnumerateDeviceExtensionProperties(candidate, nullptr, &extCount, nullptr);
            std::vector<VkExtensionProperties> extensions(extCount);
            vkEnumerateDeviceExtensionProperties(candidate, nullptr, &extCount, extensions.data());
            if (graphics != UINT32_MAX && present != UINT32_MAX &&
                HasExtension(extensions, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) {
                physical_ = candidate;
                caps_.graphicsQueueFamily = graphics;
                caps_.presentQueueFamily = present;
                selectedExtensions = std::move(extensions);
                break;
            }
        }
        if (!physical_) {
            failure = "No graphics+present queue with VK_KHR_swapchain";
            unsupported = true;
            return false;
        }

        vkGetPhysicalDeviceProperties(physical_, &caps_.properties);
        vkGetPhysicalDeviceFeatures(physical_, &caps_.features);
        QueryExtendedCapabilities(selectedExtensions);

        float priority = 1.0f;
        std::vector<VkDeviceQueueCreateInfo> queues;
        for (uint32_t family : {caps_.graphicsQueueFamily, caps_.presentQueueFamily}) {
            if (!queues.empty() && queues.front().queueFamilyIndex == family) continue;
            VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
            queue.queueFamilyIndex = family;
            queue.queueCount = 1;
            queue.pQueuePriorities = &priority;
            queues.push_back(queue);
        }
        const char* extension = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
        VkPhysicalDeviceVulkan12Features vulkan12{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
        if (caps_.timelineSemaphore)
            vulkan12.timelineSemaphore = VK_TRUE;
        VkDeviceCreateInfo create{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        create.queueCreateInfoCount = static_cast<uint32_t>(queues.size());
        create.pQueueCreateInfos = queues.data();
        create.enabledExtensionCount = 1;
        create.ppEnabledExtensionNames = &extension;
        create.pNext = caps_.timelineSemaphore ? &vulkan12 : nullptr;
        if (vkCreateDevice(physical_, &create, nullptr, &device_) != VK_SUCCESS) {
            failure = "vkCreateDevice failed";
            return false;
        }
        vkGetDeviceQueue(device_, caps_.graphicsQueueFamily, 0, &graphicsQueue_);
        vkGetDeviceQueue(device_, caps_.presentQueueFamily, 0, &presentQueue_);
        return true;
    }

    bool RunTimelineRoundTrip(const char*& failure, bool& unsupported)
    {
        caps_.timelineRoundTripAttempted = true;
        if (!caps_.timelineSemaphore) {
            failure = "Host Vulkan does not expose timelineSemaphore";
            unsupported = true;
            return false;
        }

        const uint64_t expectedValue = 1;
        VkSemaphoreTypeCreateInfo timelineType{
            VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
        timelineType.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        VkSemaphoreCreateInfo semaphoreInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        semaphoreInfo.pNext = &timelineType;
        VkSemaphore semaphore = VK_NULL_HANDLE;
        caps_.timelineCreateResult = vkCreateSemaphore(device_, &semaphoreInfo,
                                                       nullptr, &semaphore);
        if (caps_.timelineCreateResult != VK_SUCCESS) {
            failure = "Host vkCreateSemaphore(timeline) failed";
            return false;
        }

        VkTimelineSemaphoreSubmitInfo timelineSubmit{
            VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
        timelineSubmit.signalSemaphoreValueCount = 1;
        timelineSubmit.pSignalSemaphoreValues = &expectedValue;
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.pNext = &timelineSubmit;
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = &semaphore;
        caps_.timelineSubmitResult = vkQueueSubmit(graphicsQueue_, 1, &submit,
                                                   VK_NULL_HANDLE);
        if (caps_.timelineSubmitResult != VK_SUCCESS) {
            vkDestroySemaphore(device_, semaphore, nullptr);
            failure = "Host vkQueueSubmit(timeline signal) failed";
            return false;
        }

        VkSemaphoreWaitInfo waitInfo{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
        waitInfo.semaphoreCount = 1;
        waitInfo.pSemaphores = &semaphore;
        waitInfo.pValues = &expectedValue;
        caps_.timelineWaitResult = vkWaitSemaphores(device_, &waitInfo,
                                                     UINT64_C(500000000));
        if (caps_.timelineWaitResult == VK_SUCCESS) {
            caps_.timelineCounterResult = vkGetSemaphoreCounterValue(
                device_, semaphore, &caps_.timelineObservedValue);
        }
        caps_.timelineRoundTripPassed =
            caps_.timelineWaitResult == VK_SUCCESS &&
            caps_.timelineCounterResult == VK_SUCCESS &&
            caps_.timelineObservedValue >= expectedValue;
        vkDestroySemaphore(device_, semaphore, nullptr);

        if (!caps_.timelineRoundTripPassed) {
            failure = "Host timeline semaphore signal/wait/counter round-trip failed";
            return false;
        }
        return true;
    }

    bool CreateSwapchain(const char*& failure, bool& unsupported)
    {
        if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_, surface_, &caps_.surface) != VK_SUCCESS) {
            failure = "vkGetPhysicalDeviceSurfaceCapabilitiesKHR failed";
            return false;
        }
        uint32_t formatCount = 0, modeCount = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(physical_, surface_, &formatCount, nullptr);
        vkGetPhysicalDeviceSurfacePresentModesKHR(physical_, surface_, &modeCount, nullptr);
        caps_.surfaceFormatCount = formatCount;
        caps_.presentModeCount = modeCount;
        if (!formatCount || !modeCount ||
            !(caps_.surface.supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)) {
            failure = "Surface lacks formats, present modes, or color-attachment usage";
            unsupported = true;
            return false;
        }
        std::vector<VkSurfaceFormatKHR> formats(formatCount);
        std::vector<VkPresentModeKHR> modes(modeCount);
        vkGetPhysicalDeviceSurfaceFormatsKHR(physical_, surface_, &formatCount, formats.data());
        vkGetPhysicalDeviceSurfacePresentModesKHR(physical_, surface_, &modeCount, modes.data());
        VkSurfaceFormatKHR chosen = formats[0];
        for (const auto& format : formats) {
            if (format.format == VK_FORMAT_B8G8R8A8_UNORM || format.format == VK_FORMAT_R8G8B8A8_UNORM) {
                chosen = format;
                break;
            }
        }
        caps_.format = chosen.format;
        caps_.presentMode = VK_PRESENT_MODE_FIFO_KHR;
        extent_ = caps_.surface.currentExtent;
        if (extent_.width == UINT32_MAX) {
            extent_.width = std::clamp(1280u, caps_.surface.minImageExtent.width, caps_.surface.maxImageExtent.width);
            extent_.height = std::clamp(800u, caps_.surface.minImageExtent.height, caps_.surface.maxImageExtent.height);
        }
        uint32_t imageCount = std::max(2u, caps_.surface.minImageCount);
        if (caps_.surface.maxImageCount) imageCount = std::min(imageCount, caps_.surface.maxImageCount);
        VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        if (!(caps_.surface.supportedCompositeAlpha & alpha)) {
            alpha = static_cast<VkCompositeAlphaFlagBitsKHR>(caps_.surface.supportedCompositeAlpha &
                    (~caps_.surface.supportedCompositeAlpha + 1));
        }
        uint32_t families[] = {caps_.graphicsQueueFamily, caps_.presentQueueFamily};
        VkSwapchainCreateInfoKHR create{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
        create.surface = surface_;
        create.minImageCount = imageCount;
        create.imageFormat = chosen.format;
        create.imageColorSpace = chosen.colorSpace;
        create.imageExtent = extent_;
        create.imageArrayLayers = 1;
        create.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        create.imageSharingMode = families[0] == families[1] ? VK_SHARING_MODE_EXCLUSIVE : VK_SHARING_MODE_CONCURRENT;
        create.queueFamilyIndexCount = families[0] == families[1] ? 0 : 2;
        create.pQueueFamilyIndices = families[0] == families[1] ? nullptr : families;
        create.preTransform = caps_.surface.currentTransform;
        create.compositeAlpha = alpha;
        create.presentMode = caps_.presentMode;
        create.clipped = VK_TRUE;
        if (vkCreateSwapchainKHR(device_, &create, nullptr, &swapchain_) != VK_SUCCESS) {
            failure = "vkCreateSwapchainKHR failed";
            return false;
        }

        vkGetSwapchainImagesKHR(device_, swapchain_, &imageCount, nullptr);
        images_.resize(imageCount);
        vkGetSwapchainImagesKHR(device_, swapchain_, &imageCount, images_.data());
        views_.resize(imageCount);
        for (uint32_t i = 0; i < imageCount; ++i) {
            VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            view.image = images_[i];
            view.viewType = VK_IMAGE_VIEW_TYPE_2D;
            view.format = chosen.format;
            view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            view.subresourceRange.levelCount = 1;
            view.subresourceRange.layerCount = 1;
            if (vkCreateImageView(device_, &view, nullptr, &views_[i]) != VK_SUCCESS) {
                failure = "vkCreateImageView failed";
                return false;
            }
        }
        VkAttachmentDescription attachment{};
        attachment.format = chosen.format;
        attachment.samples = VK_SAMPLE_COUNT_1_BIT;
        attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        VkAttachmentReference reference{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &reference;
        VkRenderPassCreateInfo render{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        render.attachmentCount = 1;
        render.pAttachments = &attachment;
        render.subpassCount = 1;
        render.pSubpasses = &subpass;
        if (vkCreateRenderPass(device_, &render, nullptr, &renderPass_) != VK_SUCCESS) {
            failure = "vkCreateRenderPass failed";
            return false;
        }
        framebuffers_.resize(imageCount);
        for (uint32_t i = 0; i < imageCount; ++i) {
            VkFramebufferCreateInfo framebuffer{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
            framebuffer.renderPass = renderPass_;
            framebuffer.attachmentCount = 1;
            framebuffer.pAttachments = &views_[i];
            framebuffer.width = extent_.width;
            framebuffer.height = extent_.height;
            framebuffer.layers = 1;
            if (vkCreateFramebuffer(device_, &framebuffer, nullptr, &framebuffers_[i]) != VK_SUCCESS) {
                failure = "vkCreateFramebuffer failed";
                return false;
            }
        }
        return true;
    }

    bool CreateCommands(const char*& failure)
    {
        VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pool.queueFamilyIndex = caps_.graphicsQueueFamily;
        if (vkCreateCommandPool(device_, &pool, nullptr, &commandPool_) != VK_SUCCESS) {
            failure = "vkCreateCommandPool failed";
            return false;
        }
        VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocate.commandPool = commandPool_;
        allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocate.commandBufferCount = 1;
        if (vkAllocateCommandBuffers(device_, &allocate, &command_) != VK_SUCCESS) {
            failure = "vkAllocateCommandBuffers failed";
            return false;
        }
        VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        if (vkCreateSemaphore(device_, &semaphore, nullptr, &acquire_) != VK_SUCCESS ||
            vkCreateSemaphore(device_, &semaphore, nullptr, &rendered_) != VK_SUCCESS ||
            vkCreateFence(device_, &fence, nullptr, &fence_) != VK_SUCCESS) {
            failure = "Vulkan synchronization object creation failed";
            return false;
        }
        return true;
    }

    bool DrawFrame(const char*& failure)
    {
        auto frameStart = Clock::now();
        auto renderStart = frameStart;
        if (vkWaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX) != VK_SUCCESS) {
            failure = "vkWaitForFences failed";
            return false;
        }
        metrics_.renderWaitUs += std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - renderStart).count();

        uint32_t imageIndex = 0;
        auto acquireStart = Clock::now();
        VkResult result = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX, acquire_, VK_NULL_HANDLE, &imageIndex);
        metrics_.acquireWaitUs += std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - acquireStart).count();
        if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
            failure = "vkAcquireNextImageKHR failed";
            return false;
        }
        vkResetFences(device_, 1, &fence_);
        vkResetCommandBuffer(command_, 0);
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        vkBeginCommandBuffer(command_, &begin);
        VkClearValue black{};
        black.color.float32[3] = 1.0f;
        VkRenderPassBeginInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        pass.renderPass = renderPass_;
        pass.framebuffer = framebuffers_[imageIndex];
        pass.renderArea.extent = extent_;
        pass.clearValueCount = 1;
        pass.pClearValues = &black;
        vkCmdBeginRenderPass(command_, &pass, VK_SUBPASS_CONTENTS_INLINE);
        const float colors[4][4] = {{1,0,0,1},{0,1,0,1},{0,0,1,1},{1,1,0,1}};
        const uint32_t halfW = extent_.width / 2;
        const uint32_t halfH = extent_.height / 2;
        for (uint32_t i = 0; i < 4; ++i) {
            VkClearAttachment clear{};
            clear.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            clear.colorAttachment = 0;
            memcpy(clear.clearValue.color.float32, colors[i], sizeof(colors[i]));
            VkClearRect rect{};
            rect.rect.offset = {static_cast<int32_t>((i & 1) ? halfW : 0),
                                static_cast<int32_t>((i & 2) ? halfH : 0)};
            rect.rect.extent = {(i & 1) ? extent_.width - halfW : halfW,
                                (i & 2) ? extent_.height - halfH : halfH};
            rect.layerCount = 1;
            vkCmdClearAttachments(command_, 1, &clear, 1, &rect);
        }
        vkCmdEndRenderPass(command_);
        if (vkEndCommandBuffer(command_) != VK_SUCCESS) {
            failure = "vkEndCommandBuffer failed";
            return false;
        }
        VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.waitSemaphoreCount = 1;
        submit.pWaitSemaphores = &acquire_;
        submit.pWaitDstStageMask = &waitStage;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &command_;
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = &rendered_;
        if (vkQueueSubmit(graphicsQueue_, 1, &submit, fence_) != VK_SUCCESS) {
            failure = "vkQueueSubmit failed";
            return false;
        }
        metrics_.queueSubmitCount++;
        VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        present.waitSemaphoreCount = 1;
        present.pWaitSemaphores = &rendered_;
        present.swapchainCount = 1;
        present.pSwapchains = &swapchain_;
        present.pImageIndices = &imageIndex;
        auto presentStart = Clock::now();
        result = vkQueuePresentKHR(presentQueue_, &present);
        metrics_.presentWaitUs += std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - presentStart).count();
        if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
            failure = "vkQueuePresentKHR failed";
            return false;
        }
        metrics_.frameMs.push_back(std::chrono::duration<double, std::milli>(Clock::now() - frameStart).count());
        return true;
    }

    void DestroySwapchain()
    {
        if (!device_) return;
        for (VkFramebuffer value : framebuffers_) if (value) vkDestroyFramebuffer(device_, value, nullptr);
        framebuffers_.clear();
        if (renderPass_) vkDestroyRenderPass(device_, renderPass_, nullptr);
        renderPass_ = VK_NULL_HANDLE;
        for (VkImageView value : views_) if (value) vkDestroyImageView(device_, value, nullptr);
        views_.clear();
        images_.clear();
        if (swapchain_) vkDestroySwapchainKHR(device_, swapchain_, nullptr);
        swapchain_ = VK_NULL_HANDLE;
    }

    void Cleanup()
    {
        if (device_) vkDeviceWaitIdle(device_);
        if (fence_) vkDestroyFence(device_, fence_, nullptr);
        if (rendered_) vkDestroySemaphore(device_, rendered_, nullptr);
        if (acquire_) vkDestroySemaphore(device_, acquire_, nullptr);
        if (commandPool_) vkDestroyCommandPool(device_, commandPool_, nullptr);
        DestroySwapchain();
        if (device_) vkDestroyDevice(device_, nullptr);
        if (surface_) vkDestroySurfaceKHR(instance_, surface_, nullptr);
        if (instance_) vkDestroyInstance(instance_, nullptr);
        if (window_) OH_NativeWindow_DestroyNativeWindow(window_);
        device_ = VK_NULL_HANDLE;
        instance_ = VK_NULL_HANDLE;
        window_ = nullptr;
    }

    OHNativeWindow* window_ = nullptr;
    std::string runId_;
    ProbeCaps caps_;
    ProbeMetrics metrics_;
    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue graphicsQueue_ = VK_NULL_HANDLE;
    VkQueue presentQueue_ = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkRenderPass renderPass_ = VK_NULL_HANDLE;
    VkCommandPool commandPool_ = VK_NULL_HANDLE;
    VkCommandBuffer command_ = VK_NULL_HANDLE;
    VkSemaphore acquire_ = VK_NULL_HANDLE;
    VkSemaphore rendered_ = VK_NULL_HANDLE;
    VkFence fence_ = VK_NULL_HANDLE;
    VkExtent2D extent_{};
    std::vector<VkImage> images_;
    std::vector<VkImageView> views_;
    std::vector<VkFramebuffer> framebuffers_;
};

} // namespace

bool StartHostVulkanProbe(uint64_t surfaceId, const std::string& runId)
{
    if (!SafeId(runId) || gRunning.exchange(true)) return false;
    gCancel.store(false);
    OHNativeWindow* window = nullptr;
    int result = OH_NativeWindow_CreateNativeWindowFromSurfaceId(surfaceId, &window);
    if (result != 0 || !window) {
        gRunning.store(false);
        return false;
    }
    std::thread([window, runId]() {
        VulkanProbe probe(window, runId);
        probe.Run();
        gRunning.store(false);
    }).detach();
    return true;
}

void StopHostVulkanProbe()
{
    gCancel.store(true);
}
