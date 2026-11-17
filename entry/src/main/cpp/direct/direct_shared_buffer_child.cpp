#define VK_USE_PLATFORM_OHOS 1
#include <vulkan/vulkan.h>
#include "native_buffer_socket.h"
#include "direct_native_read_watchdog.h"

#include <AbilityKit/native_child_process.h>
#include <native_buffer/native_buffer.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

namespace {
using namespace winehua::direct::shared;

class Writer {
public:
    explicit Writer(int socket) : socket_(socket) {}
    void Mark(const char* stage) {
        stage_ = stage;
        Packet packet{};
        packet.kind = Kind::Stage;
        packet.pid = getpid();
        packet.imports = imports_;
        packet.allocations = allocations_;
        packet.exports = exports_;
        packet.fenceImports = fenceImports_;
        packet.fenceExports = fenceExports_;
        packet.realFenceImports = realFenceImports_;
        packet.realFenceExports = realFenceExports_;
        snprintf(packet.stage, sizeof(packet.stage), "%s", stage);
        snprintf(packet.loaderPath, sizeof(packet.loaderPath), "%s", loader_);
        snprintf(packet.deviceName, sizeof(packet.deviceName), "%s", name_);
        Send(socket_, packet);
    }
    ~Writer() {
        if (device_) {
            vkDeviceWaitIdle(device_);
            if (fence_) vkDestroyFence(device_, fence_, nullptr);
            if (acquire_) vkDestroySemaphore(device_, acquire_, nullptr);
            if (release_) vkDestroySemaphore(device_, release_, nullptr);
            if (pool_) vkDestroyCommandPool(device_, pool_, nullptr);
            if (image_) vkDestroyImage(device_, image_, nullptr);
            if (memory_) vkFreeMemory(device_, memory_, nullptr);
            vkDestroyDevice(device_, nullptr);
        }
        if (instance_) vkDestroyInstance(instance_, nullptr);
    }
    bool Fail(const char* stage, VkResult error) { stage_ = stage; error_ = error; return false; }
    const char* stage_ = "writer_pending";
    VkResult error_ = VK_SUCCESS;
    char loader_[128]{};
    char name_[128]{};
    uint32_t imports_ = 0, allocations_ = 0, exports_ = 0, fenceImports_ = 0, fenceExports_ = 0;
    uint32_t realFenceImports_ = 0, realFenceExports_ = 0;

    bool InitializeDevice() {
        Dl_info loader{};
        if (!dladdr(reinterpret_cast<void*>(vkCreateInstance), &loader) || !loader.dli_fname)
            return Fail("writer_loader_identity", VK_ERROR_INITIALIZATION_FAILED);
        snprintf(loader_, sizeof(loader_), "%s", loader.dli_fname);
        if (!strstr(loader_, "/system/lib64/libvulkan.so"))
            return Fail("writer_not_system_vulkan", VK_ERROR_INITIALIZATION_FAILED);
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName = "WineHua phone shared-buffer P0";
        VkInstanceCreateInfo create{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        create.pApplicationInfo = &app;
        // Exercise the same OHOS WSI-capable loader path as Wine Direct,
        // even though this small capability image has no display surface.
        const char* instanceExtensions[]{VK_KHR_SURFACE_EXTENSION_NAME,
                                        VK_OHOS_SURFACE_EXTENSION_NAME};
        create.enabledExtensionCount = 2;
        create.ppEnabledExtensionNames = instanceExtensions;
        Mark("writer_instance");
        VkResult r = VK_ERROR_INCOMPATIBLE_DRIVER;
        for (uint32_t version : {VK_API_VERSION_1_3, VK_API_VERSION_1_2, VK_API_VERSION_1_1}) {
            app.apiVersion = version;
            r = vkCreateInstance(&create, nullptr, &instance_);
            if (r != VK_ERROR_INCOMPATIBLE_DRIVER) break;
        }
        if (r != VK_SUCCESS) return Fail("writer_instance", r);
        uint32_t count = 0;
        r = vkEnumeratePhysicalDevices(instance_, &count, nullptr);
        if (r != VK_SUCCESS || !count || count > 16) return Fail("writer_device_count", r);
        VkPhysicalDevice devices[16]{};
        r = vkEnumeratePhysicalDevices(instance_, &count, devices);
        if (r != VK_SUCCESS) return Fail("writer_devices", r);
        for (uint32_t i = 0; i < count; ++i) {
            VkPhysicalDeviceProperties properties{};
            vkGetPhysicalDeviceProperties(devices[i], &properties);
            if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) continue;
            physical_ = devices[i];
            snprintf(name_, sizeof(name_), "%s", properties.deviceName);
            break;
        }
        if (!physical_) return Fail("writer_native_gpu", VK_ERROR_INITIALIZATION_FAILED);
        uint32_t familiesCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(physical_, &familiesCount, nullptr);
        if (!familiesCount || familiesCount > 32) return Fail("writer_queue_count", VK_ERROR_INITIALIZATION_FAILED);
        VkQueueFamilyProperties families[32]{};
        vkGetPhysicalDeviceQueueFamilyProperties(physical_, &familiesCount, families);
        for (uint32_t i = 0; i < familiesCount; ++i)
            if (families[i].queueCount && (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) { family_ = i; break; }
        if (family_ == UINT32_MAX) return Fail("writer_queue_family", VK_ERROR_INITIALIZATION_FAILED);
        const float priority = 1;
        VkDeviceQueueCreateInfo queueCreate{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queueCreate.queueFamilyIndex = family_;
        queueCreate.queueCount = 1;
        queueCreate.pQueuePriorities = &priority;
        const char* extensions[]{VK_OHOS_EXTERNAL_MEMORY_EXTENSION_NAME, VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME};
        VkDeviceCreateInfo deviceCreate{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        deviceCreate.queueCreateInfoCount = 1;
        deviceCreate.pQueueCreateInfos = &queueCreate;
        deviceCreate.enabledExtensionCount = 2;
        deviceCreate.ppEnabledExtensionNames = extensions;
        Mark("writer_device");
        r = vkCreateDevice(physical_, &deviceCreate, nullptr, &device_);
        if (r != VK_SUCCESS) return Fail("writer_device", r);
        vkGetDeviceQueue(device_, family_, 0, &queue_);
        importFd_ = reinterpret_cast<PFN_vkImportSemaphoreFdKHR>(vkGetDeviceProcAddr(device_, "vkImportSemaphoreFdKHR"));
        exportFd_ = reinterpret_cast<PFN_vkGetSemaphoreFdKHR>(vkGetDeviceProcAddr(device_, "vkGetSemaphoreFdKHR"));
        if (!importFd_ || !exportFd_) return Fail("writer_sync_functions", VK_ERROR_EXTENSION_NOT_PRESENT);
        Mark("writer_device_ready");
        return true;
    }

    bool ImportBuffer(OH_NativeBuffer* buffer, const Packet& p) {
        VkResult r;
        VkNativeBufferFormatPropertiesOHOS format{VK_STRUCTURE_TYPE_NATIVE_BUFFER_FORMAT_PROPERTIES_OHOS};
        VkNativeBufferPropertiesOHOS properties{VK_STRUCTURE_TYPE_NATIVE_BUFFER_PROPERTIES_OHOS};
        properties.pNext = &format;
        Mark("writer_buffer_properties");
        r = vkGetNativeBufferPropertiesOHOS(device_, buffer, &properties);
        if (r != VK_SUCCESS) return Fail("writer_buffer_properties", r);
        if (format.format != VK_FORMAT_R8G8B8A8_UNORM || !properties.allocationSize || !properties.memoryTypeBits)
            return Fail("writer_buffer_format", VK_ERROR_FORMAT_NOT_SUPPORTED);
        VkExternalMemoryImageCreateInfo external{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
        external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OHOS_NATIVE_BUFFER_BIT_OHOS;
        VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        image.pNext = &external;
        image.imageType = VK_IMAGE_TYPE_2D;
        image.format = format.format;
        image.extent = {static_cast<uint32_t>(p.width), static_cast<uint32_t>(p.height), 1};
        image.mipLevels = image.arrayLayers = 1;
        image.samples = VK_SAMPLE_COUNT_1_BIT;
        image.tiling = VK_IMAGE_TILING_OPTIMAL;
        image.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        image.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        r = vkCreateImage(device_, &image, nullptr, &image_);
        if (r != VK_SUCCESS) return Fail("writer_import_image", r);
        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(device_, image_, &requirements);
        uint32_t types = properties.memoryTypeBits & requirements.memoryTypeBits;
        if (!types) return Fail("writer_memory_type", VK_ERROR_FEATURE_NOT_PRESENT);
        uint32_t type = 0;
        while (!(types & (1u << type))) ++type;
        VkImportNativeBufferInfoOHOS nativeImport{VK_STRUCTURE_TYPE_IMPORT_NATIVE_BUFFER_INFO_OHOS};
        nativeImport.buffer = buffer;
        VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
        dedicated.pNext = &nativeImport;
        dedicated.image = image_;
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.pNext = &dedicated;
        allocation.allocationSize = properties.allocationSize;
        allocation.memoryTypeIndex = type;
        Mark("writer_import_memory");
        r = vkAllocateMemory(device_, &allocation, nullptr, &memory_);
        if (r != VK_SUCCESS) return Fail("writer_import_memory", r);
        Mark("writer_import_bind");
        r = vkBindImageMemory(device_, image_, memory_, 0);
        if (r != VK_SUCCESS) return Fail("writer_import_bind", r);
        ++imports_;
        return InitializeCommands();
    }

    bool ExportBuffer(OH_NativeBuffer** buffer) {
        auto getBuffer = reinterpret_cast<PFN_vkGetMemoryNativeBufferOHOS>(
            vkGetDeviceProcAddr(device_, "vkGetMemoryNativeBufferOHOS"));
        if (!getBuffer) return Fail("writer_export_function", VK_ERROR_EXTENSION_NOT_PRESENT);
        constexpr auto handle = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OHOS_NATIVE_BUFFER_BIT_OHOS;
        constexpr VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        VkPhysicalDeviceExternalImageFormatInfo queryExternal{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO};
        queryExternal.handleType = handle;
        VkPhysicalDeviceImageFormatInfo2 query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2};
        query.pNext = &queryExternal;
        query.format = VK_FORMAT_R8G8B8A8_UNORM;
        query.type = VK_IMAGE_TYPE_2D;
        query.tiling = VK_IMAGE_TILING_OPTIMAL;
        query.usage = usage;
        VkExternalImageFormatProperties externalProperties{VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
        VkImageFormatProperties2 properties{VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2};
        properties.pNext = &externalProperties;
        Mark("writer_export_capabilities");
        VkResult r = vkGetPhysicalDeviceImageFormatProperties2(physical_, &query, &properties);
        if (r != VK_SUCCESS) return Fail("writer_export_capabilities", r);
        const auto& externalMemory = externalProperties.externalMemoryProperties;
        if (!(externalMemory.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT) ||
            !(externalMemory.compatibleHandleTypes & handle))
            return Fail("writer_export_unsupported", VK_ERROR_FEATURE_NOT_PRESENT);
        VkExternalMemoryImageCreateInfo external{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
        external.handleTypes = handle;
        VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        image.pNext = &external;
        image.imageType = VK_IMAGE_TYPE_2D;
        image.format = query.format;
        image.extent = {64, 64, 1};
        image.mipLevels = image.arrayLayers = 1;
        image.samples = VK_SAMPLE_COUNT_1_BIT;
        image.tiling = query.tiling;
        image.usage = usage;
        image.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        Mark("writer_export_image");
        r = vkCreateImage(device_, &image, nullptr, &image_);
        if (r != VK_SUCCESS) return Fail("writer_export_image", r);
        VkMemoryRequirements requirements{};
        Mark("writer_export_requirements");
        vkGetImageMemoryRequirements(device_, image_, &requirements);
        VkPhysicalDeviceMemoryProperties memoryProperties{};
        vkGetPhysicalDeviceMemoryProperties(physical_, &memoryProperties);
        fprintf(stderr, "[P0EXPORT] requirements size=%llu types=%x memoryTypes=%u\n",
            static_cast<unsigned long long>(requirements.size), requirements.memoryTypeBits,
            memoryProperties.memoryTypeCount);
        uint32_t type = UINT32_MAX;
        for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i)
            if ((!requirements.memoryTypeBits || (requirements.memoryTypeBits & (1u << i))) &&
                (memoryProperties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
                type = i; break;
            }
        // NativeBuffer allocation can derive the size from the dedicated image.
        // Maleoon does not expose ordinary pre-bind memory requirements here.
        // Pass its reported size (including zero) instead of inventing byte sizes.
        if (type == UINT32_MAX)
            return Fail("writer_export_memory_type", VK_ERROR_FEATURE_NOT_PRESENT);
        VkExportMemoryAllocateInfo exportInfo{VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO};
        exportInfo.handleTypes = handle;
        VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
        dedicated.pNext = &exportInfo;
        dedicated.image = image_;
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.pNext = &dedicated;
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = type;
        Mark("writer_export_allocate");
        r = vkAllocateMemory(device_, &allocation, nullptr, &memory_);
        if (r != VK_SUCCESS) return Fail("writer_export_allocate", r);
        ++allocations_;
        Mark("writer_export_bind");
        r = vkBindImageMemory(device_, image_, memory_, 0);
        if (r != VK_SUCCESS) return Fail("writer_export_bind", r);
        VkMemoryGetNativeBufferInfoOHOS getInfo{VK_STRUCTURE_TYPE_MEMORY_GET_NATIVE_BUFFER_INFO_OHOS};
        getInfo.memory = memory_;
        Mark("writer_export_native_buffer");
        r = getBuffer(device_, &getInfo, buffer);
        if (r != VK_SUCCESS || !*buffer)
            return Fail("writer_export_native_buffer", r == VK_SUCCESS ? VK_ERROR_INITIALIZATION_FAILED : r);
        ++exports_;
        exported_ = true;
        return InitializeCommands();
    }

    bool InitializeCommands() {
        VkResult r;
        VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pool.queueFamilyIndex = family_;
        r = vkCreateCommandPool(device_, &pool, nullptr, &pool_);
        if (r != VK_SUCCESS) return Fail("writer_command_pool", r);
        VkCommandBufferAllocateInfo command{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        command.commandPool = pool_;
        command.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        command.commandBufferCount = 1;
        r = vkAllocateCommandBuffers(device_, &command, &command_);
        if (r != VK_SUCCESS) return Fail("writer_command_allocate", r);
        VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        r = vkCreateFence(device_, &fence, nullptr, &fence_);
        if (r != VK_SUCCESS) return Fail("writer_fence", r);
        VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        r = vkCreateSemaphore(device_, &semaphore, nullptr, &acquire_);
        if (r != VK_SUCCESS) return Fail("writer_acquire_semaphore", r);
        VkExportSemaphoreCreateInfo exportInfo{VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO};
        exportInfo.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
        semaphore.pNext = &exportInfo;
        r = vkCreateSemaphore(device_, &semaphore, nullptr, &release_);
        return r == VK_SUCCESS || Fail("writer_release_semaphore", r);
    }

    bool Submit(uint32_t frame, bool drain, bool haveAcquire, int* acquireFd, int* doneFd) {
        VkResult r;
        if (submitted_) {
            r = vkWaitForFences(device_, 1, &fence_, VK_TRUE, 5'000'000'000ULL);
            if (r != VK_SUCCESS) return Fail("writer_reuse_wait", r);
        }
        r = vkResetFences(device_, 1, &fence_);
        if (r != VK_SUCCESS) return Fail("writer_reset_fence", r);
        r = vkResetCommandBuffer(command_, 0);
        if (r != VK_SUCCESS) return Fail("writer_reset_command", r);
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        r = vkBeginCommandBuffer(command_, &begin);
        if (r != VK_SUCCESS) return Fail("writer_begin", r);
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.oldLayout = submitted_ ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = drain ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        // A newly allocated image has no external owner until its first release.
        barrier.srcQueueFamilyIndex = exported_ && !submitted_ ? VK_QUEUE_FAMILY_IGNORED : VK_QUEUE_FAMILY_EXTERNAL;
        barrier.dstQueueFamilyIndex = exported_ && !submitted_ ? VK_QUEUE_FAMILY_IGNORED : family_;
        barrier.dstAccessMask = drain ? 0 : VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.image = image_;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(command_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                              0, 0, nullptr, 0, nullptr, 1, &barrier);
        if (!drain) {
            VkClearColorValue color{};
            color.float32[0] = ((frame % 8) * 31 + 7) / 255.0f;
            color.float32[1] = 90.0f / 255.0f;
            color.float32[2] = 165.0f / 255.0f;
            color.float32[3] = 1;
            vkCmdClearColorImage(command_, image_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                  &color, 1, &barrier.subresourceRange);
            barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            barrier.srcQueueFamilyIndex = family_;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.dstAccessMask = 0;
            vkCmdPipelineBarrier(command_, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                                  0, 0, nullptr, 0, nullptr, 1, &barrier);
        }
        r = vkEndCommandBuffer(command_);
        if (r != VK_SUCCESS) return Fail("writer_end", r);
        if (haveAcquire) {
            VkImportSemaphoreFdInfoKHR importInfo{VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR};
            importInfo.semaphore = acquire_;
            importInfo.flags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT;
            importInfo.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
            importInfo.fd = *acquireFd;
            r = importFd_(device_, &importInfo);
            if (r != VK_SUCCESS) return Fail("writer_release_import", r);
            if (*acquireFd >= 0) ++realFenceImports_;
            *acquireFd = -1;
            ++fenceImports_;
        }
        VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &command_;
        submit.waitSemaphoreCount = haveAcquire ? 1 : 0;
        submit.pWaitSemaphores = &acquire_;
        submit.pWaitDstStageMask = &waitStage;
        submit.signalSemaphoreCount = drain ? 0 : 1;
        submit.pSignalSemaphores = &release_;
        Mark("writer_submit");
        r = vkQueueSubmit(queue_, 1, &submit, fence_);
        if (r != VK_SUCCESS) return Fail("writer_submit", r);
        submitted_ = true;
        if (drain) {
            r = vkWaitForFences(device_, 1, &fence_, VK_TRUE, 5'000'000'000ULL);
            return r == VK_SUCCESS || Fail("writer_drain_wait", r);
        }
        VkSemaphoreGetFdInfoKHR exportInfo{VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR};
        exportInfo.semaphore = release_;
        exportInfo.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
        r = exportFd_(device_, &exportInfo, doneFd);
        if (r != VK_SUCCESS) return Fail("writer_done_export", r);
        ++fenceExports_;
        if (*doneFd >= 0) ++realFenceExports_;
        return true;
    }
private:
    int socket_;
    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    uint32_t family_ = UINT32_MAX;
    VkImage image_ = VK_NULL_HANDLE;
    VkDeviceMemory memory_ = VK_NULL_HANDLE;
    VkCommandPool pool_ = VK_NULL_HANDLE;
    VkCommandBuffer command_ = VK_NULL_HANDLE;
    VkFence fence_ = VK_NULL_HANDLE;
    VkSemaphore acquire_ = VK_NULL_HANDLE, release_ = VK_NULL_HANDLE;
    PFN_vkImportSemaphoreFdKHR importFd_ = nullptr;
    PFN_vkGetSemaphoreFdKHR exportFd_ = nullptr;
    bool submitted_ = false;
    bool exported_ = false;
};

void Report(int fd, Kind kind, const char* stage, int result = 0, uint32_t frame = 0, Writer* writer = nullptr,
            const std::vector<int>& fds = {}) {
    Packet packet{};
    packet.kind = kind;
    packet.pid = getpid();
    packet.result = result;
    packet.frame = frame;
    snprintf(packet.stage, sizeof(packet.stage), "%s", stage);
    if (writer) {
        packet.imports = writer->imports_;
        packet.allocations = writer->allocations_;
        packet.exports = writer->exports_;
        packet.fenceImports = writer->fenceImports_;
        packet.fenceExports = writer->fenceExports_;
        packet.realFenceImports = writer->realFenceImports_;
        packet.realFenceExports = writer->realFenceExports_;
        snprintf(packet.loaderPath, sizeof(packet.loaderPath), "%s", writer->loader_);
        snprintf(packet.deviceName, sizeof(packet.deviceName), "%s", writer->name_);
    }
    Send(fd, packet, fds);
}

bool Run(int fd) {
    Report(fd, Kind::Stage, "child_main");
    Message descriptor;
    if (!Receive(fd, descriptor)) return false;
    const bool reverse = descriptor.packet.kind == Kind::Allocate;
    if (!reverse && descriptor.packet.kind != Kind::Buffer) return false;
    if (reverse && !descriptor.fds.empty()) return false;
    // The imported Vulkan image/memory must be destroyed before the buffer.
    const auto unreference = [](OH_NativeBuffer* buffer) {
        if (buffer) OH_NativeBuffer_Unreference(buffer);
    };
    std::unique_ptr<OH_NativeBuffer, decltype(unreference)> bufferOwner(nullptr, unreference);
    OH_NativeBuffer* buffer = nullptr;
    const char* stage = "child_decode";
    NativeReadWatchdog watchdog;
    Writer writer(fd);
    watchdog.Arm();
    const bool deviceReady = writer.InitializeDevice();
    watchdog.Disarm();
    if (!deviceReady) {
        Report(fd, Kind::Error, writer.stage_, writer.error_, 0, &writer);
        return false;
    }
    bool imageReady = false;
    if (reverse) {
        watchdog.Arm();
        imageReady = writer.ExportBuffer(&buffer);
        bufferOwner.reset(buffer);
        if (imageReady) {
            writer.Mark("child_export_encode");
            imageReady = EncodeBuffer(buffer, descriptor, &stage);
            if (!imageReady) writer.Fail(stage, VK_ERROR_UNKNOWN);
        }
        watchdog.Disarm();
        if (imageReady) {
            descriptor.packet.pid = getpid();
            snprintf(descriptor.packet.stage, sizeof(descriptor.packet.stage), "child_buffer_exported");
            imageReady = Send(fd, descriptor.packet, descriptor.fds);
            if (!imageReady) writer.Fail("child_export_send", VK_ERROR_UNKNOWN);
        }
    } else {
        Report(fd, Kind::Stage, "child_native_buffer_decode");
        struct CodecContext { int fd; NativeReadWatchdog& watchdog; } codec{fd, watchdog};
        const auto codecProgress = [](const char* value, void* context) {
            auto& state = *static_cast<CodecContext*>(context);
            Report(state.fd, Kind::Stage, value);
            if (!strcmp(value, "codec_system_read_from_parcel")) state.watchdog.Arm();
            if (!strcmp(value, "codec_system_read_complete")) state.watchdog.Disarm();
        };
        const bool decoded = DecodeBuffer(descriptor, &buffer, &stage, codecProgress, &codec);
        bufferOwner.reset(buffer);
        watchdog.Disarm();
        if (!decoded) {
            Report(fd, Kind::Error, stage, -1);
            return false;
        }
        watchdog.Arm();
        imageReady = writer.ImportBuffer(buffer, descriptor.packet);
        watchdog.Disarm();
    }
    descriptor.CloseFds(); // NativeBuffer owns duplicates from the local parcel.
    bool passed = false;
    {
        if (imageReady) {
            Report(fd, Kind::Ready, reverse ? "child_image_exported" : "child_image_imported", 0, 0, &writer);
            int releaseFd = -1;
            bool haveRelease = false;
            uint32_t frame = 0;
            for (; frame < kFrameCount; ++frame) {
                Message command;
                if (!Receive(fd, command) || command.packet.kind != Kind::Render ||
                    command.packet.frame != frame || !command.fds.empty()) break;
                int doneFd = -1;
                if (!writer.Submit(frame, false, haveRelease, &releaseFd, &doneFd)) break;
                Report(fd, Kind::Frame, "child_gpu_submitted", 0, frame, &writer,
                       doneFd >= 0 ? std::vector<int>{doneFd} : std::vector<int>{});
                if (doneFd >= 0) close(doneFd);
                Message returned;
                if (!Receive(fd, returned) || returned.packet.kind != Kind::Release ||
                    returned.packet.frame != frame || returned.fds.size() > 1) break;
                releaseFd = returned.fds.empty() ? -1 : returned.fds.front();
                returned.fds.clear();
                haveRelease = true;
            }
            if (frame == kFrameCount) {
                Message finish;
                if (Receive(fd, finish) && finish.packet.kind == Kind::Finish && finish.fds.empty())
                    passed = writer.Submit(frame, true, haveRelease, &releaseFd, nullptr);
            }
            if (releaseFd >= 0) close(releaseFd);
        }
        Report(fd, passed ? Kind::Complete : Kind::Error,
               passed ? "child_complete" : writer.stage_, passed ? 0 : writer.error_, kFrameCount, &writer);
    }
    return passed;
}
}

extern "C" __attribute__((visibility("default"))) void Main(NativeChildProcess_Args args) {
    int fd = -1, diagnostic = -1;
    for (auto* item = args.fdList.head; item; item = item->next) {
        if (item->fdName && !strcmp(item->fdName, kSocketName)) fd = item->fd;
        if (item->fdName && !strcmp(item->fdName, kDiagnosticName)) diagnostic = item->fd;
    }
    if (fd < 0) _exit(2);
    if (diagnostic >= 0) {
        if (fd == STDERR_FILENO && diagnostic != STDERR_FILENO) {
            fd = fcntl(fd, F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
            if (fd < 0) _exit(3);
        }
        if (dup2(diagnostic, STDERR_FILENO) < 0) _exit(3);
        if (diagnostic != STDERR_FILENO) close(diagnostic);
    }
    timeval timeout{5, 0};
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    unsetenv("VK_ICD_FILENAMES");
    unsetenv("VK_DRIVER_FILES");
    const bool passed = Run(fd);
    // Run's local Vulkan objects have now been destroyed. A completion packet
    // alone precedes that cleanup and does not qualify a healthy child exit.
    if (passed) Report(fd, Kind::Stage, "child_cleanup_complete");
    close(fd);
    _exit(passed ? 0 : 1);
}
