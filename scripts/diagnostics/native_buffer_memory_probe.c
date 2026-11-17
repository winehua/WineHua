/* OHOS capability probe, standalone or in an isolated diagnostic child.
 * Only two 64 KiB buffers are mapped; no rendered image is read back.
 * Run later on the selected device, outside an active game session. */
#define _GNU_SOURCE
#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_OHOS
#include <stdbool.h>
#include <vulkan/vulkan.h>
#include <native_buffer/native_buffer.h>
#include <dlfcn.h>
#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <signal.h>
#include <unistd.h>

static PFN_vkAllocateCommandBuffers vkAllocateCommandBuffers;
static PFN_vkAllocateMemory vkAllocateMemory;
static PFN_vkBeginCommandBuffer vkBeginCommandBuffer;
static PFN_vkBindBufferMemory vkBindBufferMemory;
static PFN_vkCmdCopyBuffer vkCmdCopyBuffer;
static PFN_vkCmdFillBuffer vkCmdFillBuffer;
static PFN_vkCmdPipelineBarrier vkCmdPipelineBarrier;
static PFN_vkCreateBuffer vkCreateBuffer;
static PFN_vkCreateCommandPool vkCreateCommandPool;
static PFN_vkCreateDevice vkCreateDevice;
static PFN_vkCreateFence vkCreateFence;
static PFN_vkCreateInstance vkCreateInstance;
static PFN_vkDestroyBuffer vkDestroyBuffer;
static PFN_vkDestroyCommandPool vkDestroyCommandPool;
static PFN_vkDestroyDevice vkDestroyDevice;
static PFN_vkDestroyFence vkDestroyFence;
static PFN_vkDestroyInstance vkDestroyInstance;
static PFN_vkEndCommandBuffer vkEndCommandBuffer;
static PFN_vkEnumerateDeviceExtensionProperties vkEnumerateDeviceExtensionProperties;
static PFN_vkEnumerateInstanceVersion vkEnumerateInstanceVersion;
static PFN_vkEnumeratePhysicalDevices vkEnumeratePhysicalDevices;
static PFN_vkFlushMappedMemoryRanges vkFlushMappedMemoryRanges;
static PFN_vkFreeMemory vkFreeMemory;
static PFN_vkGetBufferMemoryRequirements vkGetBufferMemoryRequirements;
static PFN_vkGetDeviceProcAddr vkGetDeviceProcAddr;
static PFN_vkGetDeviceQueue vkGetDeviceQueue;
static PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr;
static PFN_vkGetPhysicalDeviceExternalBufferProperties vkGetPhysicalDeviceExternalBufferProperties;
static PFN_vkGetPhysicalDeviceMemoryProperties vkGetPhysicalDeviceMemoryProperties;
static PFN_vkGetPhysicalDeviceProperties vkGetPhysicalDeviceProperties;
static PFN_vkGetPhysicalDeviceQueueFamilyProperties vkGetPhysicalDeviceQueueFamilyProperties;
static PFN_vkInvalidateMappedMemoryRanges vkInvalidateMappedMemoryRanges;
static PFN_vkMapMemory vkMapMemory;
static PFN_vkQueueSubmit vkQueueSubmit;
static PFN_vkUnmapMemory vkUnmapMemory;
static PFN_vkWaitForFences vkWaitForFences;

static PFN_vkGetMemoryNativeBufferOHOS get_native_buffer;
static PFN_vkGetPhysicalDeviceImageFormatProperties2 probe_image_properties;
enum { BYTES = 65536, WORDS = BYTES / 4 };
struct allocation {
    OH_NativeBuffer *native;
    OHNativeWindowBuffer *wrapper;
    VkBuffer buffer;
    VkDeviceMemory memory;
    void *vk_map;
    uint32_t *alias;
    size_t alias_size;
    VkMemoryPropertyFlags flags;
};
static VkResult make_allocation(VkPhysicalDevice physical, VkDevice device,
    PFN_vkGetNativeBufferPropertiesOHOS properties, VkExternalMemoryHandleTypeFlagBits handle,
    int usage, struct allocation *out)
{
    OH_NativeBuffer_Config config = {.width = BYTES, .height = 1,
        .format = NATIVEBUFFER_PIXEL_FMT_BLOB, .usage = usage};
    out->native = OH_NativeBuffer_Alloc(&config);
    if (!out->native) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    VkNativeBufferPropertiesOHOS native = {.sType = VK_STRUCTURE_TYPE_NATIVE_BUFFER_PROPERTIES_OHOS};
    VkResult result = properties(device, out->native, &native);
    if (result != VK_SUCCESS) return result;
    VkExternalMemoryBufferCreateInfo external = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO, .handleTypes = handle};
    VkBufferCreateInfo info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .pNext = &external,
        .size = BYTES, .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    result = vkCreateBuffer(device, &info, NULL, &out->buffer);
    if (result != VK_SUCCESS) return result;
    VkMemoryRequirements requirement;
    vkGetBufferMemoryRequirements(device, out->buffer, &requirement);
    VkPhysicalDeviceMemoryProperties memory;
    vkGetPhysicalDeviceMemoryProperties(physical, &memory);
    uint32_t bits = native.memoryTypeBits & requirement.memoryTypeBits;
    uint32_t type = memory.memoryTypeCount;
    for (unsigned pass = 0; pass < 2 && type == memory.memoryTypeCount; ++pass) {
        for (uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
            VkMemoryPropertyFlags flags = memory.memoryTypes[i].propertyFlags;
            if (!(bits & (1u << i)) || !(flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) continue;
            if (!pass && !(flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) continue;
            type = i; out->flags = flags; break;
        }
    }
    printf("NATIVE_BLOB stage=requirements handle=0x%x usage=0x%x nativeBytes=%" PRIu64
           " requiredBytes=%" PRIu64 " bits=0x%x type=%u flags=0x%x\n",
           handle, usage, native.allocationSize, requirement.size, bits, type, out->flags);
    if (type == memory.memoryTypeCount || native.allocationSize < requirement.size)
        return VK_ERROR_FEATURE_NOT_PRESENT;
    VkMemoryDedicatedAllocateInfo dedicated = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, .buffer = out->buffer};
    VkImportNativeBufferInfoOHOS import = {.sType = VK_STRUCTURE_TYPE_IMPORT_NATIVE_BUFFER_INFO_OHOS,
        .pNext = &dedicated, .buffer = out->native};
    VkMemoryAllocateInfo allocate = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &import,
        .allocationSize = native.allocationSize, .memoryTypeIndex = type};
    result = vkAllocateMemory(device, &allocate, NULL, &out->memory);
    if (result != VK_SUCCESS) return result;
    result = vkBindBufferMemory(device, out->buffer, out->memory, 0);
    if (result != VK_SUCCESS) return result;
    result = vkMapMemory(device, out->memory, 0, VK_WHOLE_SIZE, 0, &out->vk_map);
    if (result != VK_SUCCESS) return result;
    out->wrapper = OH_NativeWindow_CreateNativeWindowBufferFromNativeBuffer(out->native);
    BufferHandle *buffer = out->wrapper ? OH_NativeWindow_GetBufferHandleFromNative(out->wrapper) : NULL;
    if (!buffer || buffer->fd < 0 || buffer->size < BYTES) return VK_ERROR_INVALID_EXTERNAL_HANDLE;
    out->alias_size = (size_t)buffer->size;
    out->alias = mmap(NULL, out->alias_size, PROT_READ | PROT_WRITE, MAP_SHARED, buffer->fd, 0);
    if (out->alias == MAP_FAILED) { out->alias = NULL; return VK_ERROR_MEMORY_MAP_FAILED; }
    return VK_SUCCESS;
}
/* Control using ordinary driver memory: separates external import failures
 * from command execution or cache visibility in the same signed process. */
static VkResult make_regular_allocation(VkPhysicalDevice physical, VkDevice device,
    VkExternalMemoryHandleTypeFlagBits handle, struct allocation *out)
{
    VkExternalMemoryBufferCreateInfo external = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO, .handleTypes = handle};
    VkBufferCreateInfo info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .pNext = handle ? &external : NULL, .size = BYTES, .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VkResult result = vkCreateBuffer(device, &info, NULL, &out->buffer);
    if (result != VK_SUCCESS) return result;
    VkMemoryRequirements requirement;
    vkGetBufferMemoryRequirements(device, out->buffer, &requirement);
    VkPhysicalDeviceMemoryProperties memory;
    vkGetPhysicalDeviceMemoryProperties(physical, &memory);
    uint32_t type = memory.memoryTypeCount;
    for (unsigned pass = 0; pass < 2 && type == memory.memoryTypeCount; ++pass) {
        for (uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
            VkMemoryPropertyFlags flags = memory.memoryTypes[i].propertyFlags;
            if (!(requirement.memoryTypeBits & (1u << i)) || !(flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) continue;
            if (!pass && !(flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) continue;
            type = i; out->flags = flags; break;
        }
    }
    if (type == memory.memoryTypeCount) return VK_ERROR_FEATURE_NOT_PRESENT;
    printf("NATIVE_BLOB stage=regular-requirements type=%u flags=0x%x bytes=%" PRIu64 "\n",
        type, out->flags, requirement.size);
    VkExportMemoryAllocateInfo export = {.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
        .handleTypes = handle};
    VkMemoryAllocateInfo allocate = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = handle ? &export : NULL, .allocationSize = requirement.size, .memoryTypeIndex = type};
    result = vkAllocateMemory(device, &allocate, NULL, &out->memory);
    if (result != VK_SUCCESS) return result;
    result = vkBindBufferMemory(device, out->buffer, out->memory, 0);
    if (result != VK_SUCCESS) return result;
    result = vkMapMemory(device, out->memory, 0, VK_WHOLE_SIZE, 0, &out->vk_map);
    if (result != VK_SUCCESS) return result;
    if (!handle) { out->alias = out->vk_map; return VK_SUCCESS; }
    VkMemoryGetNativeBufferInfoOHOS get = {.sType = VK_STRUCTURE_TYPE_MEMORY_GET_NATIVE_BUFFER_INFO_OHOS,
        .memory = out->memory};
    result = get_native_buffer(device, &get, &out->native);
    printf("NATIVE_BLOB stage=export handle=0x%x flags=0x%x result=%d\n", handle, out->flags, result);
    if (result != VK_SUCCESS) return result;
    out->wrapper = OH_NativeWindow_CreateNativeWindowBufferFromNativeBuffer(out->native);
    BufferHandle *buffer = out->wrapper ? OH_NativeWindow_GetBufferHandleFromNative(out->wrapper) : NULL;
    if (!buffer || buffer->fd < 0 || buffer->size < BYTES) return VK_ERROR_INVALID_EXTERNAL_HANDLE;
    out->alias_size = (size_t)buffer->size;
    out->alias = mmap(NULL, out->alias_size, PROT_READ | PROT_WRITE, MAP_SHARED, buffer->fd, 0);
    if (out->alias == MAP_FAILED) { out->alias = NULL; return VK_ERROR_MEMORY_MAP_FAILED; }
    return VK_SUCCESS;
}
static void release_allocation(VkDevice device, struct allocation *a)
{
    if (a->alias && a->alias != a->vk_map) munmap(a->alias, a->alias_size);
    if (a->vk_map) vkUnmapMemory(device, a->memory);
    if (a->buffer) vkDestroyBuffer(device, a->buffer, NULL);
    if (a->memory) vkFreeMemory(device, a->memory, NULL);
    if (a->wrapper) OH_NativeWindow_DestroyNativeWindowBuffer(a->wrapper);
    if (a->native) OH_NativeBuffer_Unreference(a->native);
}
static unsigned wrong(const uint32_t *p, uint32_t expected)
{
    unsigned errors = 0;
    for (unsigned i = 0; i < WORDS; ++i) errors += p[i] != expected;
    return errors;
}
static int run_case(VkPhysicalDevice physical, VkDevice device, VkQueue queue, uint32_t family,
    PFN_vkGetNativeBufferPropertiesOHOS properties, VkExternalMemoryHandleTypeFlagBits handle,
    int usage)
{
    struct allocation a[2] = {0};
    VkCommandPool pool = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkResult result = VK_SUCCESS;
    int passed = 0;
    unsigned failed_phases = 0;
#define TRY(call) do { result = (call); if (result != VK_SUCCESS) { \
    printf("NATIVE_BLOB stage=error line=%d result=%d errno=%d\n", __LINE__, result, errno); goto done; } } while (0)
    for (unsigned i = 0; i < 2; ++i) {
        TRY(handle && usage >= 0 ? make_allocation(physical, device, properties, handle, usage, &a[i]) :
            make_regular_allocation(physical, device, handle, &a[i]));
    }
    /* Test a real shared fd alias, the same mechanism a separate GNU/FEX
     * process could map. The Vulkan mapping is only used for cache APIs and
     * an identity check; no memcpy transports the payload. */
    a[0].alias[0] = 0x12345678;
    if (*(volatile uint32_t *)a[0].vk_map != 0x12345678) {
        printf("NATIVE_BLOB stage=alias-identity failed=1\n"); goto done;
    }
    VkCommandPoolCreateInfo cp = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .queueFamilyIndex = family};
    TRY(vkCreateCommandPool(device, &cp, NULL, &pool));
    VkCommandBufferAllocateInfo ca = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 2};
    VkCommandBuffer command[2]; TRY(vkAllocateCommandBuffers(device, &ca, command));
    for (unsigned i = 0; i < 2; ++i) {
        VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        TRY(vkBeginCommandBuffer(command[i], &begin));
        VkBufferMemoryBarrier before = {.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_HOST_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = a[0].buffer, .size = VK_WHOLE_SIZE};
        vkCmdPipelineBarrier(command[i], VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, NULL, 1, &before, 0, NULL);
        VkBufferCopy copy = {.size = BYTES};
        if (!i) vkCmdCopyBuffer(command[i], a[0].buffer, a[1].buffer, 1, &copy);
        else vkCmdFillBuffer(command[i], a[1].buffer, 0, BYTES, 0xabcdef01);
        VkBufferMemoryBarrier after = {.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = a[1].buffer, .size = VK_WHOLE_SIZE};
        vkCmdPipelineBarrier(command[i], VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
            0, 0, NULL, 1, &after, 0, NULL);
        TRY(vkEndCommandBuffer(command[i]));
    }
    for (unsigned phase = 0; phase < 3; ++phase) {
        if (!phase && (!(a[0].flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) ||
                       !(a[1].flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))) {
            printf("NATIVE_BLOB phase=0 skipped=noncoherent\n"); continue;
        }
        uint32_t expected = phase == 2 ? 0xabcdef01 : 0x12345678 + phase;
        if (phase != 2) {
            for (unsigned i = 0; i < WORDS; ++i) a[0].alias[i] = expected;
            if (phase) {
                VkMappedMemoryRange r = {.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
                    .memory = a[0].memory, .size = VK_WHOLE_SIZE};
                TRY(vkFlushMappedMemoryRanges(device, 1, &r));
            }
        }
        VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        TRY(vkCreateFence(device, &fi, NULL, &fence));
        VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1,
            .pCommandBuffers = &command[phase == 2]};
        TRY(vkQueueSubmit(queue, 1, &submit, fence));
        result = vkWaitForFences(device, 1, &fence, VK_TRUE, 5000000000ull);
        if (result != VK_SUCCESS) {
            /* Do not free in-flight Vulkan objects or wait without a bound. */
            printf("NATIVE_BLOB stage=wait result=%d terminating=1\n", result);
            fflush(stdout); _Exit(2);
        }
        if (phase) {
            VkMappedMemoryRange r = {.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
                .memory = a[1].memory, .size = VK_WHOLE_SIZE};
            TRY(vkInvalidateMappedMemoryRanges(device, 1, &r));
        }
        unsigned alias_wrong = wrong(a[1].alias, expected);
        unsigned map_wrong = wrong(a[1].vk_map, expected);
        printf("NATIVE_BLOB phase=%u expected=%08x aliasFirst=%08x mapFirst=%08x aliasWrong=%u mapWrong=%u\n",
            phase, expected, a[1].alias[0], *(uint32_t *)a[1].vk_map, alias_wrong, map_wrong);
        vkDestroyFence(device, fence, NULL); fence = VK_NULL_HANDLE;
        failed_phases += alias_wrong != 0 || map_wrong != 0;
    }
    passed = failed_phases == 0;
done:
    if (result == VK_TIMEOUT) _Exit(3);
    if (fence) vkDestroyFence(device, fence, NULL);
    if (pool) vkDestroyCommandPool(device, pool, NULL);
    for (unsigned i = 0; i < 2; ++i) release_allocation(device, &a[i]);
    printf("NATIVE_BLOB stage=case-done handle=0x%x usage=0x%x passed=%d failedPhases=%u result=%d\n",
           handle, usage, passed, failed_phases, result);
    return passed;
#undef TRY
}
#include "native_image_memory_probe.h"

static VKAPI_ATTR VkBool32 VKAPI_CALL debug_message(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT type, const VkDebugUtilsMessengerCallbackDataEXT *data, void *user)
{
    (void)severity; (void)type; (void)user;
    printf("NATIVE_BLOB loader=%s\n", data->pMessage ? data->pMessage : "unknown");
    return VK_FALSE;
}
static void deadline(int signal_number)
{
    (void)signal_number;
    _Exit(3);
}
static void inspect_driver_directory(const char *path)
{
    DIR *dir = opendir(path);
    printf("NATIVE_BLOB stage=driver-directory path=%s opened=%d errno=%d\n", path, dir != NULL, dir ? 0 : errno);
    if (!dir) return;
    struct dirent *entry;
    unsigned count = 0;
    while ((entry = readdir(dir)) && count < 8) {
        if (entry->d_name[0] == '.') continue;
        printf("NATIVE_BLOB stage=driver-file name=%s\n", entry->d_name); ++count;
    }
    closedir(dir);
}
#ifdef WINEHUA_PROBE_EMBEDDED
int WinehuaNativeBufferMemoryMain(void)
#else
int main(void)
#endif
{
    setvbuf(stdout, NULL, _IONBF, 0);
    signal(SIGALRM, deadline); alarm(75);
    // The HAP also contains Wine's guest libvulkan.so. Bind every Vulkan call
    // through the explicit system loader so this control cannot test Venus.
    unsetenv("VK_ICD_FILENAMES"); unsetenv("VK_DRIVER_FILES");
    unsetenv("VK_ADD_DRIVER_FILES"); unsetenv("VK_LOADER_DRIVERS_SELECT"); unsetenv("VK_LOADER_DRIVERS_DISABLE");
    setenv("VK_LOADER_DEBUG", "error,warn,driver", 1);
    void *loader = dlopen("/system/lib64/libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    vkGetInstanceProcAddr = loader ? (PFN_vkGetInstanceProcAddr)dlsym(loader, "vkGetInstanceProcAddr") : NULL;
    if (!vkGetInstanceProcAddr) { printf("NATIVE_BLOB stage=system-loader failed=1\n"); return 2; }
    printf("NATIVE_BLOB stage=system-loader path=/system/lib64/libvulkan.so\n");
    unsetenv("VK_ICD_FILENAMES"); unsetenv("VK_DRIVER_FILES");
    vkCreateInstance = (PFN_vkCreateInstance)vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance");
    vkEnumerateInstanceVersion = (PFN_vkEnumerateInstanceVersion)vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkEnumerateInstanceVersion");
    Dl_info origin = {0};
    dladdr((void *)vkGetInstanceProcAddr, &origin);
    printf("NATIVE_BLOB stage=proc-origin name=vkGetInstanceProcAddr path=%s\n", origin.dli_fname ? origin.dli_fname : "unknown");
    dladdr((void *)vkCreateInstance, &origin);
    printf("NATIVE_BLOB stage=proc-origin name=vkCreateInstance path=%s\n", origin.dli_fname ? origin.dli_fname : "unknown");
    PFN_vkCreateInstance direct_create = (PFN_vkCreateInstance)dlsym(loader, "vkCreateInstance");
    printf("NATIVE_BLOB stage=proc-origin directCreateMatches=%d\n", direct_create == vkCreateInstance);
    if (direct_create) vkCreateInstance = direct_create;
    uint32_t api = VK_API_VERSION_1_0;
    if (vkEnumerateInstanceVersion) vkEnumerateInstanceVersion(&api);
    printf("NATIVE_BLOB stage=loader-api version=%u\n", api);
    if (!vkCreateInstance || api < VK_API_VERSION_1_1) return 2;
    PFN_vkEnumerateInstanceExtensionProperties enumerate_extensions = (PFN_vkEnumerateInstanceExtensionProperties)
        dlsym(loader, "vkEnumerateInstanceExtensionProperties");
    uint32_t instance_extensions = 0;
    VkResult extension_result = enumerate_extensions ? enumerate_extensions(NULL, &instance_extensions, NULL) : VK_ERROR_INITIALIZATION_FAILED;
    printf("NATIVE_BLOB stage=instance-extensions count=%u result=%d\n", instance_extensions, extension_result);
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "NativeBuffer memory contract probe", .apiVersion = VK_API_VERSION_1_1};
    inspect_driver_directory("/system/etc/vulkan/igraphics");
    inspect_driver_directory("/system/etc/vulkan/swapchain");
    // Use the same host WSI-enabled instance route as the existing Wine
    // receiver. No surface is created; OHOS selects its graphics driver here.
    const char *instance_names[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_OHOS_SURFACE_EXTENSION_NAME, VK_EXT_DEBUG_UTILS_EXTENSION_NAME};
    VkDebugUtilsMessengerCreateInfoEXT debug = {.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
        .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT,
        .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT,
        .pfnUserCallback = debug_message};
    VkInstanceCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app,
        .pNext = &debug, .enabledExtensionCount = 3, .ppEnabledExtensionNames = instance_names};
    VkInstance instance; VkResult result = vkCreateInstance(&ii, NULL, &instance);
    printf("NATIVE_BLOB stage=instance api=%u result=%d\n", app.apiVersion, result);
    if (result == VK_ERROR_INCOMPATIBLE_DRIVER) {
        app.apiVersion = VK_API_VERSION_1_0;
        result = vkCreateInstance(&ii, NULL, &instance);
        printf("NATIVE_BLOB stage=instance api=%u result=%d\n", app.apiVersion, result);
    }
    if (result != VK_SUCCESS) return 2;
#define LOAD_INSTANCE(name) do { name = (PFN_##name)vkGetInstanceProcAddr(instance, #name); \
    if (!name) { printf("NATIVE_BLOB stage=missing-function name=%s\n", #name); return 2; } } while (0)
    LOAD_INSTANCE(vkDestroyInstance);
    LOAD_INSTANCE(vkEnumeratePhysicalDevices);
    LOAD_INSTANCE(vkGetPhysicalDeviceProperties);
    LOAD_INSTANCE(vkEnumerateDeviceExtensionProperties);
    LOAD_INSTANCE(vkGetPhysicalDeviceQueueFamilyProperties);
    LOAD_INSTANCE(vkCreateDevice);
    LOAD_INSTANCE(vkGetDeviceProcAddr);
    LOAD_INSTANCE(vkGetPhysicalDeviceMemoryProperties);
    LOAD_INSTANCE(vkGetPhysicalDeviceExternalBufferProperties);
    probe_image_properties = (PFN_vkGetPhysicalDeviceImageFormatProperties2)
        vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceImageFormatProperties2");
#undef LOAD_INSTANCE
    VkPhysicalDevice physical; uint32_t count = 1;
    result = vkEnumeratePhysicalDevices(instance, &count, &physical);
    if ((result != VK_SUCCESS && result != VK_INCOMPLETE) || !count) return 2;
    VkPhysicalDeviceProperties props; vkGetPhysicalDeviceProperties(physical, &props);
    printf("NATIVE_BLOB stage=begin renderer=%s api=%u display_readbacks=0 venus=0 fex=0\n", props.deviceName, props.apiVersion);
    VkExtensionProperties extensions[256]; count = 256;
    result = vkEnumerateDeviceExtensionProperties(physical, NULL, &count, extensions);
    if (result != VK_SUCCESS) return 2;
    int external = 0, host_import = 0, foreign = 0;
    for (uint32_t i = 0; i < count; ++i) {
        external |= !strcmp(extensions[i].extensionName, VK_OHOS_EXTERNAL_MEMORY_EXTENSION_NAME);
        host_import |= !strcmp(extensions[i].extensionName, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME);
        foreign |= !strcmp(extensions[i].extensionName, VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME);
    }
    printf("NATIVE_BLOB stage=extensions ohosExternal=%d hostImport=%d foreign=%d\n", external, host_import, foreign);

    VkQueueFamilyProperties families[32]; count = 32;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families);
    uint32_t family = 0; while (family < count && !(families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT)) ++family;
    if (family == count) return 2;
    float priority = 1;
    VkDeviceQueueCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = family, .queueCount = 1, .pQueuePriorities = &priority};
    const char *names[2];
    uint32_t name_count = 0;
    if (external) names[name_count++] = VK_OHOS_EXTERNAL_MEMORY_EXTENSION_NAME;
    if (foreign) names[name_count++] = VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME;
    VkDeviceCreateInfo di = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &qi, .enabledExtensionCount = name_count, .ppEnabledExtensionNames = names};
    VkDevice device; result = vkCreateDevice(physical, &di, NULL, &device);
    if (result != VK_SUCCESS) { printf("NATIVE_BLOB stage=device result=%d\n", result); return 2; }
#define LOAD_DEVICE(name) do { name = (PFN_##name)vkGetDeviceProcAddr(device, #name); \
    if (!name) { printf("NATIVE_BLOB stage=missing-function name=%s\n", #name); return 2; } } while (0)
    LOAD_DEVICE(vkAllocateCommandBuffers);
    LOAD_DEVICE(vkAllocateMemory);
    LOAD_DEVICE(vkBeginCommandBuffer);
    LOAD_DEVICE(vkBindBufferMemory);
    LOAD_DEVICE(vkCmdCopyBuffer);
    LOAD_DEVICE(vkCmdFillBuffer);
    LOAD_DEVICE(vkCmdPipelineBarrier);
    LOAD_DEVICE(vkCreateBuffer);
    LOAD_DEVICE(vkCreateCommandPool);
    LOAD_DEVICE(vkCreateFence);
    LOAD_DEVICE(vkDestroyBuffer);
    LOAD_DEVICE(vkDestroyCommandPool);
    LOAD_DEVICE(vkDestroyDevice);
    LOAD_DEVICE(vkDestroyFence);
    LOAD_DEVICE(vkEndCommandBuffer);
    LOAD_DEVICE(vkFlushMappedMemoryRanges);
    LOAD_DEVICE(vkFreeMemory);
    LOAD_DEVICE(vkGetBufferMemoryRequirements);
    LOAD_DEVICE(vkGetDeviceQueue);
    LOAD_DEVICE(vkInvalidateMappedMemoryRanges);
    LOAD_DEVICE(vkMapMemory);
    LOAD_DEVICE(vkQueueSubmit);
    LOAD_DEVICE(vkUnmapMemory);
    LOAD_DEVICE(vkWaitForFences);
#undef LOAD_DEVICE
    VkQueue queue; vkGetDeviceQueue(device, family, 0, &queue);
    PFN_vkGetNativeBufferPropertiesOHOS properties = (PFN_vkGetNativeBufferPropertiesOHOS)
        vkGetDeviceProcAddr(device, "vkGetNativeBufferPropertiesOHOS");
    get_native_buffer = (PFN_vkGetMemoryNativeBufferOHOS)
        vkGetDeviceProcAddr(device, "vkGetMemoryNativeBufferOHOS");
    const int baseline = run_case(physical, device, queue, family, NULL, 0, 0);
    int passed = 0, attempted = 0, exports_passed = 0, exports_attempted = 0;
    /* Pre-API23 Huawei handle and the registered Khronos handle are different
     * bits. Query each; never substitute a new constant into an old driver. */
    const VkExternalMemoryHandleTypeFlagBits handles[] = {
        (VkExternalMemoryHandleTypeFlagBits)0x2000, (VkExternalMemoryHandleTypeFlagBits)0x8000};
    const int cpu = NATIVEBUFFER_USAGE_CPU_READ | NATIVEBUFFER_USAGE_CPU_WRITE | NATIVEBUFFER_USAGE_MEM_DMA;
    const int usages[] = {cpu, cpu | NATIVEBUFFER_USAGE_HW_RENDER | NATIVEBUFFER_USAGE_HW_TEXTURE,
        NATIVEBUFFER_USAGE_MEM_DMA,
        NATIVEBUFFER_USAGE_MEM_DMA | NATIVEBUFFER_USAGE_HW_RENDER | NATIVEBUFFER_USAGE_HW_TEXTURE};
    for (unsigned i = 0; properties && i < 2; ++i) {
        if (i == 1 && !foreign) {
            printf("NATIVE_BLOB stage=capability handle=0x8000 skipped=missing-foreign-extension\n"); continue;
        }
        VkPhysicalDeviceExternalBufferInfo eb = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO,
            .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, .handleType = handles[i]};
        VkExternalBufferProperties ep = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES};
        vkGetPhysicalDeviceExternalBufferProperties(physical, &eb, &ep);
        printf("NATIVE_BLOB stage=capability handle=0x%x features=0x%x compatible=0x%x\n",
            handles[i], ep.externalMemoryProperties.externalMemoryFeatures, ep.externalMemoryProperties.compatibleHandleTypes);
        if (get_native_buffer && (ep.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT)) {
            ++exports_attempted;
            exports_passed += run_case(physical, device, queue, family, properties, handles[i], -1);
        }
        if (!(ep.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT)) continue;
        for (unsigned u = 0; u < (i ? 2u : 4u); ++u) {
            ++attempted;
            passed += run_case(physical, device, queue, family, properties, handles[i], usages[u]);
        }
    }
    const int images_passed = run_image_memory_cases(physical, device, queue, family);
    vkDestroyDevice(device, NULL); vkDestroyInstance(instance, NULL);
    printf("NATIVE_BLOB stage=complete regularPassed=%d importedPassedCases=%d importedAttemptedCases=%d exportedPassedCases=%d exportedAttemptedCases=%d\n", baseline, passed, attempted, exports_passed, exports_attempted);
    alarm(0);
    return baseline && images_passed && attempted > 0 && passed == attempted && exports_attempted > 0 && exports_passed == exports_attempted ? 0 : 1;
}
