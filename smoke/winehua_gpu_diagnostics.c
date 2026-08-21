/* WineHua Vulkan and DXVK diagnostic application. */

#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <d3d11.h>
#include <dxgi.h>
#include <vulkan/vulkan.h>
#include <winver.h>

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif

struct diagnostics {
    BOOL automation;
    char run_id[96];
    char test_id[96];
    char result_path[MAX_PATH];
    char text_path[MAX_PATH];
    ULONGLONG started_ms;

    VkResult instance_result;
    VkResult physical_result;
    uint32_t loader_api;
    VkPhysicalDeviceProperties properties;
    VkPhysicalDeviceFeatures core;
    uint32_t queue_family;
    BOOL api12;
    BOOL api13;
    BOOL robustness2_extension;
    BOOL transform_feedback_extension;
    BOOL robust_buffer_access2;
    BOOL robust_image_access2;
    BOOL null_descriptor;
    BOOL timeline_semaphore;
    BOOL buffer_device_address;
    BOOL descriptor_indexing;
    BOOL scalar_block_layout;
    BOOL synchronization2;
    BOOL dynamic_rendering;
    BOOL maintenance4;
    BOOL transform_feedback;
    BOOL geometry_streams;
    BOOL bc[7];
    BOOL rgba8_snorm_color_attachment;
    BOOL d24s8_sampled;
    BOOL d24s8_depth_stencil_attachment;
    BOOL modern_eligible;

    char requested_backend[64];
    char configured_dxvk_version[64];
    char configured_dxvk_profile[64];
    char d3d11_path[MAX_PATH];
    char dxgi_path[MAX_PATH];
    char d3d11_file_version[64];
    char dxgi_file_version[64];
    BOOL d3d11_is_dxvk;
    BOOL dxgi_is_dxvk;
    HRESULT d3d11_create_result;
    D3D_FEATURE_LEVEL d3d_feature_level;
    char d3d_adapter[128];
};

static const char *bool_text(BOOL value) { return value ? "true" : "false"; }

static ULONGLONG now_ms(void)
{
    FILETIME file_time;
    ULARGE_INTEGER value;
    GetSystemTimeAsFileTime(&file_time);
    value.LowPart = file_time.dwLowDateTime;
    value.HighPart = file_time.dwHighDateTime;
    return value.QuadPart / 10000ULL - 11644473600000ULL;
}

static void append_text(char *text, size_t size, const char *format, ...)
{
    size_t used = strlen(text);
    va_list args;
    int written;
    if (used + 1 >= size) return;
    va_start(args, format);
    written = _vsnprintf(text + used, size - used - 1, format, args);
    va_end(args);
    if (written < 0 || (size_t)written >= size - used - 1) text[size - 1] = 0;
}

static void copy_text(char *output, size_t size, const char *input)
{
    if (size) lstrcpynA(output, input ? input : "", (int)size);
}

static void json_safe_copy(char *output, size_t output_size, const char *input)
{
    size_t written = 0;
    if (!output_size) return;
    while (input && *input && written + 1 < output_size) {
        unsigned char ch = (unsigned char)*input++;
        if (ch == '\\') ch = '/';
        if (ch == '"' || ch < 0x20 || ch > 0x7e) ch = '_';
        output[written++] = (char)ch;
    }
    output[written] = 0;
}

static void version_text(uint32_t version, char *buffer, size_t size)
{
    snprintf(buffer, size, "%u.%u.%u", VK_API_VERSION_MAJOR(version),
             VK_API_VERSION_MINOR(version), VK_API_VERSION_PATCH(version));
}

static const char *feature_level_text(D3D_FEATURE_LEVEL level)
{
    switch (level) {
    case D3D_FEATURE_LEVEL_11_1: return "11_1";
    case D3D_FEATURE_LEVEL_11_0: return "11_0";
    case D3D_FEATURE_LEVEL_10_1: return "10_1";
    case D3D_FEATURE_LEVEL_10_0: return "10_0";
    case D3D_FEATURE_LEVEL_9_3: return "9_3";
    default: return "unavailable";
    }
}

static BOOL has_argument(LPWSTR *argv, int argc, const WCHAR *name)
{
    int index;
    for (index = 1; index < argc; ++index)
        if (!lstrcmpiW(argv[index], name)) return TRUE;
    return FALSE;
}

static void argument_value(LPWSTR *argv, int argc, const WCHAR *name,
                           char *output, size_t output_size)
{
    int index;
    if (!output_size) return;
    output[0] = 0;
    for (index = 1; index + 1 < argc; ++index) {
        if (!lstrcmpiW(argv[index], name)) {
            WideCharToMultiByte(CP_UTF8, 0, argv[index + 1], -1, output,
                                (int)output_size, NULL, NULL);
            return;
        }
    }
}

static BOOL ensure_parent(const char *path)
{
    char copy[MAX_PATH];
    char *cursor;
    lstrcpynA(copy, path ? path : "", sizeof(copy));
    for (cursor = copy; *cursor; ++cursor) {
        if ((*cursor == '\\' || *cursor == '/') && cursor > copy + 2) {
            char saved = *cursor;
            *cursor = 0;
            if (!CreateDirectoryA(copy, NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
                return FALSE;
            *cursor = saved;
        }
    }
    return TRUE;
}

static void default_result_paths(struct diagnostics *state)
{
    if (state->result_path[0]) return;
    snprintf(state->result_path, sizeof(state->result_path),
             "C:\\smoke\\results\\diagnostics\\gpu-diagnostics-%lu.json",
             (unsigned long)GetCurrentProcessId());
    snprintf(state->text_path, sizeof(state->text_path),
             "C:\\smoke\\results\\diagnostics\\gpu-diagnostics-%lu.txt",
             (unsigned long)GetCurrentProcessId());
}

static void loaded_module_path(const char *module_name, char *path, size_t path_size)
{
    HMODULE module = GetModuleHandleA(module_name);
    DWORD length;
    if (!path_size) return;
    path[0] = 0;
    if (!module) return;
    length = GetModuleFileNameA(module, path, (DWORD)path_size);
    if (!length || length >= path_size) path[0] = 0;
}

static void module_file_version(const char *path, char *version, size_t size)
{
    DWORD ignored = 0;
    DWORD bytes;
    void *data;
    VS_FIXEDFILEINFO *fixed = NULL;
    UINT fixed_size = 0;
    if (!size) return;
    version[0] = 0;
    if (!path || !path[0]) return;
    bytes = GetFileVersionInfoSizeA(path, &ignored);
    if (!bytes) return;
    data = malloc(bytes);
    if (!data) return;
    if (GetFileVersionInfoA(path, 0, bytes, data) &&
        VerQueryValueA(data, "\\", (LPVOID *)&fixed, &fixed_size) &&
        fixed && fixed_size >= sizeof(*fixed)) {
        snprintf(version, size, "%u.%u.%u.%u",
                 HIWORD(fixed->dwFileVersionMS), LOWORD(fixed->dwFileVersionMS),
                 HIWORD(fixed->dwFileVersionLS), LOWORD(fixed->dwFileVersionLS));
    }
    free(data);
}

static BOOL path_is_dxvk(const char *path)
{
    char normalized[MAX_PATH];
    size_t index;
    lstrcpynA(normalized, path ? path : "", sizeof(normalized));
    for (index = 0; normalized[index]; ++index) {
        if (normalized[index] == '\\') normalized[index] = '/';
        if (normalized[index] >= 'A' && normalized[index] <= 'Z')
            normalized[index] = (char)(normalized[index] - 'A' + 'a');
    }
    return strstr(normalized, "/dxvk/") != NULL || strstr(normalized, "dxvk/") != NULL;
}

static BOOL has_extension(const VkExtensionProperties *extensions, uint32_t count,
                          const char *name)
{
    uint32_t index;
    for (index = 0; index < count; ++index)
        if (!strcmp(extensions[index].extensionName, name)) return TRUE;
    return FALSE;
}

static BOOL format_supports(VkPhysicalDevice physical, VkFormat format,
                            VkFormatFeatureFlags features)
{
    VkFormatProperties properties;
    vkGetPhysicalDeviceFormatProperties(physical, format, &properties);
    return (properties.optimalTilingFeatures & features) == features;
}

static void collect_vulkan(struct diagnostics *state)
{
    VkApplicationInfo application = { 0 };
    VkInstanceCreateInfo instance_info = { 0 };
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    uint32_t count = 0;
    uint32_t index;
    VkExtensionProperties *extensions = NULL;
    VkPhysicalDeviceFeatures2 features2 = { 0 };
    VkPhysicalDeviceVulkan12Features vk12 = { 0 };
    VkPhysicalDeviceVulkan13Features vk13 = { 0 };
    VkPhysicalDeviceRobustness2FeaturesEXT robustness2 = { 0 };
    VkPhysicalDeviceTransformFeedbackFeaturesEXT transform_feedback = { 0 };
    void **tail;
    PFN_vkEnumerateInstanceVersion enumerate_version;
    VkQueueFamilyProperties *queues = NULL;

    state->loader_api = VK_API_VERSION_1_0;
    state->queue_family = UINT32_MAX;
    enumerate_version = (PFN_vkEnumerateInstanceVersion)
        vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkEnumerateInstanceVersion");
    if (enumerate_version) enumerate_version(&state->loader_api);
    application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    application.pApplicationName = "WineHua GPU Diagnostics";
    application.pEngineName = "WineHua";
    application.apiVersion = VK_API_VERSION_1_0;
    instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instance_info.pApplicationInfo = &application;
    state->instance_result = vkCreateInstance(&instance_info, NULL, &instance);
    if (state->instance_result != VK_SUCCESS) return;
    state->physical_result = vkEnumeratePhysicalDevices(instance, &count, NULL);
    if (state->physical_result != VK_SUCCESS || !count) goto cleanup;
    {
        VkPhysicalDevice *devices = calloc(count, sizeof(*devices));
        if (!devices) goto cleanup;
        state->physical_result = vkEnumeratePhysicalDevices(instance, &count, devices);
        if (state->physical_result == VK_SUCCESS) physical = devices[0];
        free(devices);
    }
    if (!physical) goto cleanup;
    vkGetPhysicalDeviceProperties(physical, &state->properties);
    vkGetPhysicalDeviceFeatures(physical, &state->core);
    state->api12 = state->properties.apiVersion >= VK_API_VERSION_1_2;
    state->api13 = state->properties.apiVersion >= VK_API_VERSION_1_3;
    if (vkEnumerateDeviceExtensionProperties(physical, NULL, &count, NULL) != VK_SUCCESS)
        goto cleanup;
    extensions = calloc(count ? count : 1, sizeof(*extensions));
    if (!extensions) goto cleanup;
    if (count && vkEnumerateDeviceExtensionProperties(physical, NULL, &count, extensions) != VK_SUCCESS)
        goto cleanup;
    state->robustness2_extension = has_extension(extensions, count, VK_EXT_ROBUSTNESS_2_EXTENSION_NAME);
    state->transform_feedback_extension = has_extension(extensions, count, VK_EXT_TRANSFORM_FEEDBACK_EXTENSION_NAME);
    features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    vk12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    vk13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    robustness2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT;
    transform_feedback.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT;
    tail = &features2.pNext;
#define APPEND_FEATURE(feature, enabled) do { \
    if (enabled) { *tail = &(feature); tail = &(feature).pNext; } \
} while (0)
    APPEND_FEATURE(vk12, state->api12);
    APPEND_FEATURE(vk13, state->api13);
    APPEND_FEATURE(robustness2, state->robustness2_extension);
    APPEND_FEATURE(transform_feedback, state->transform_feedback_extension);
#undef APPEND_FEATURE
    vkGetPhysicalDeviceFeatures2(physical, &features2);
    state->timeline_semaphore = state->api12 && vk12.timelineSemaphore;
    state->buffer_device_address = state->api12 && vk12.bufferDeviceAddress;
    state->descriptor_indexing = state->api12 && vk12.descriptorIndexing;
    state->scalar_block_layout = state->api12 && vk12.scalarBlockLayout;
    state->synchronization2 = state->api13 && vk13.synchronization2;
    state->dynamic_rendering = state->api13 && vk13.dynamicRendering;
    state->maintenance4 = state->api13 && vk13.maintenance4;
    state->robust_buffer_access2 = state->robustness2_extension && robustness2.robustBufferAccess2;
    state->robust_image_access2 = state->robustness2_extension && robustness2.robustImageAccess2;
    state->null_descriptor = state->robustness2_extension && robustness2.nullDescriptor;
    state->transform_feedback = state->transform_feedback_extension && transform_feedback.transformFeedback;
    state->geometry_streams = state->transform_feedback_extension && transform_feedback.geometryStreams;
    state->bc[0] = format_supports(physical, VK_FORMAT_BC1_RGBA_UNORM_BLOCK, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT);
    state->bc[1] = format_supports(physical, VK_FORMAT_BC2_UNORM_BLOCK, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT);
    state->bc[2] = format_supports(physical, VK_FORMAT_BC3_UNORM_BLOCK, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT);
    state->bc[3] = format_supports(physical, VK_FORMAT_BC4_UNORM_BLOCK, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT);
    state->bc[4] = format_supports(physical, VK_FORMAT_BC5_UNORM_BLOCK, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT);
    state->bc[5] = format_supports(physical, VK_FORMAT_BC6H_UFLOAT_BLOCK, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT);
    state->bc[6] = format_supports(physical, VK_FORMAT_BC7_UNORM_BLOCK, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT);
    state->rgba8_snorm_color_attachment = format_supports(physical, VK_FORMAT_R8G8B8A8_SNORM,
        VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT);
    state->d24s8_sampled = format_supports(physical, VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT);
    state->d24s8_depth_stencil_attachment = format_supports(physical, VK_FORMAT_D24_UNORM_S8_UINT,
        VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT);
    state->modern_eligible = state->api13 && state->core.robustBufferAccess &&
        state->robust_buffer_access2 && state->robust_image_access2 && state->null_descriptor &&
        state->synchronization2 && state->dynamic_rendering && state->maintenance4 && state->timeline_semaphore;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, NULL);
    queues = calloc(count ? count : 1, sizeof(*queues));
    if (!queues) goto cleanup;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, queues);
    for (index = 0; index < count; ++index) {
        if (queues[index].queueCount && (queues[index].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
            state->queue_family = index;
            break;
        }
    }
cleanup:
    free(queues);
    free(extensions);
    if (instance != VK_NULL_HANDLE) vkDestroyInstance(instance, NULL);
}

static void collect_d3d11(struct diagnostics *state)
{
    static const D3D_FEATURE_LEVEL requested_levels[] = {
        /* Request 11.0 first. Older Wine/DXVK contracts can reject an
         * explicit 11.1 entry even though they correctly create FL 11.0. */
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0
    };
    ID3D11Device *device = NULL;
    ID3D11DeviceContext *context = NULL;
    IDXGIDevice *dxgi_device = NULL;
    IDXGIAdapter *adapter = NULL;
    DXGI_ADAPTER_DESC description;

    copy_text(state->requested_backend, sizeof(state->requested_backend), getenv("WINEHUA_D3D_BACKEND"));
    copy_text(state->configured_dxvk_version, sizeof(state->configured_dxvk_version), getenv("WINEHUA_DXVK_VERSION"));
    copy_text(state->configured_dxvk_profile, sizeof(state->configured_dxvk_profile), getenv("WINEHUA_DXVK_PROFILE"));
    loaded_module_path("d3d11.dll", state->d3d11_path, sizeof(state->d3d11_path));
    loaded_module_path("dxgi.dll", state->dxgi_path, sizeof(state->dxgi_path));
    module_file_version(state->d3d11_path, state->d3d11_file_version, sizeof(state->d3d11_file_version));
    module_file_version(state->dxgi_path, state->dxgi_file_version, sizeof(state->dxgi_file_version));
    state->d3d11_is_dxvk = path_is_dxvk(state->d3d11_path);
    state->dxgi_is_dxvk = path_is_dxvk(state->dxgi_path);
    state->d3d11_create_result = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE,
        NULL, 0, requested_levels, ARRAY_SIZE(requested_levels), D3D11_SDK_VERSION,
        &device, &state->d3d_feature_level, &context);
    if (FAILED(state->d3d11_create_result) || !device) goto cleanup;
    if (SUCCEEDED(ID3D11Device_QueryInterface(device, &IID_IDXGIDevice, (void **)&dxgi_device)) &&
        dxgi_device && SUCCEEDED(IDXGIDevice_GetAdapter(dxgi_device, &adapter)) && adapter &&
        SUCCEEDED(IDXGIAdapter_GetDesc(adapter, &description))) {
        WideCharToMultiByte(CP_UTF8, 0, description.Description, -1, state->d3d_adapter,
                            (int)sizeof(state->d3d_adapter), NULL, NULL);
    }
cleanup:
    if (adapter) IDXGIAdapter_Release(adapter);
    if (dxgi_device) IDXGIDevice_Release(dxgi_device);
    if (context) ID3D11DeviceContext_Release(context);
    if (device) ID3D11Device_Release(device);
}

static void build_modern_reason(const struct diagnostics *state, char *reason, size_t size)
{
    reason[0] = 0;
    if (state->modern_eligible) {
        copy_text(reason, size, "qualified");
        return;
    }
    if (!state->api13) append_text(reason, size, "Vulkan 1.3; ");
    if (!state->core.robustBufferAccess) append_text(reason, size, "robustBufferAccess; ");
    if (!state->robust_buffer_access2) append_text(reason, size, "robustBufferAccess2; ");
    if (!state->robust_image_access2) append_text(reason, size, "robustImageAccess2; ");
    if (!state->null_descriptor) append_text(reason, size, "nullDescriptor; ");
    if (!state->synchronization2) append_text(reason, size, "synchronization2; ");
    if (!state->dynamic_rendering) append_text(reason, size, "dynamicRendering; ");
    if (!state->maintenance4) append_text(reason, size, "maintenance4; ");
    if (!state->timeline_semaphore) append_text(reason, size, "timelineSemaphore; ");
}

static void build_report(const struct diagnostics *state, char *report, size_t size)
{
    char loader_version[32];
    char device_version[32];
    char reason[256];

    report[0] = 0;
    version_text(state->loader_api, loader_version, sizeof(loader_version));
    version_text(state->properties.apiVersion, device_version, sizeof(device_version));
    build_modern_reason(state, reason, sizeof(reason));
    append_text(report, size, "WineHua Vulkan / DXVK Diagnostics\r\n");
    append_text(report, size, "\r\nVulkan transport (Windows -> winevulkan -> Venus)\r\n");
    append_text(report, size, "  Loader API: %s\r\n", loader_version);
    append_text(report, size, "  Device API: %s\r\n", device_version);
    append_text(report, size, "  Device: %s\r\n", state->properties.deviceName[0] ? state->properties.deviceName : "unavailable");
    append_text(report, size, "  Vendor/device/driver: 0x%04x / 0x%08x / %u\r\n",
                state->properties.vendorID, state->properties.deviceID, state->properties.driverVersion);
    append_text(report, size, "  Graphics queue: %s\r\n", state->queue_family == UINT32_MAX ? "unavailable" : "available");
    append_text(report, size, "\r\nD3D11-relevant Vulkan features\r\n");
    append_text(report, size, "  Geometry=%s Tessellation=%s MultiDrawIndirect=%s\r\n",
                bool_text(state->core.geometryShader), bool_text(state->core.tessellationShader), bool_text(state->core.multiDrawIndirect));
    append_text(report, size, "  DualSrcBlend=%s MultiViewport=%s RobustBufferAccess=%s\r\n",
                bool_text(state->core.dualSrcBlend), bool_text(state->core.multiViewport), bool_text(state->core.robustBufferAccess));
    append_text(report, size, "  Vulkan 1.2: timeline=%s BDA=%s descriptorIndexing=%s scalarBlockLayout=%s\r\n",
                bool_text(state->timeline_semaphore), bool_text(state->buffer_device_address),
                bool_text(state->descriptor_indexing), bool_text(state->scalar_block_layout));
    append_text(report, size, "  Vulkan 1.3: synchronization2=%s dynamicRendering=%s maintenance4=%s\r\n",
                bool_text(state->synchronization2), bool_text(state->dynamic_rendering), bool_text(state->maintenance4));
    append_text(report, size, "  Robustness2: buffer=%s image=%s nullDescriptor=%s\r\n",
                bool_text(state->robust_buffer_access2), bool_text(state->robust_image_access2), bool_text(state->null_descriptor));
    append_text(report, size, "  Transform feedback: extension=%s transformFeedback=%s geometryStreams=%s\r\n",
                bool_text(state->transform_feedback_extension), bool_text(state->transform_feedback), bool_text(state->geometry_streams));
    append_text(report, size, "\r\nNative Guest Vulkan formats\r\n");
    append_text(report, size, "  BC1=%s BC2=%s BC3=%s BC4=%s BC5=%s BC6=%s BC7=%s\r\n",
                bool_text(state->bc[0]), bool_text(state->bc[1]), bool_text(state->bc[2]), bool_text(state->bc[3]),
                bool_text(state->bc[4]), bool_text(state->bc[5]), bool_text(state->bc[6]));
    append_text(report, size, "  RGBA8_SNORM color attachment=%s D24S8 sampled=%s depth/stencil=%s\r\n",
                bool_text(state->rgba8_snorm_color_attachment), bool_text(state->d24s8_sampled),
                bool_text(state->d24s8_depth_stencil_attachment));
    append_text(report, size, "\r\nDXVK Modern 2.6 eligibility: %s\r\n",
                state->modern_eligible ? "SUPPORTED" : "NOT SUPPORTED");
    append_text(report, size, "  Missing/qualification result: %s\r\n", reason);
    append_text(report, size, "\r\nCurrent D3D11 process\r\n");
    append_text(report, size, "  Requested backend: %s\r\n", state->requested_backend[0] ? state->requested_backend : "not set");
    append_text(report, size, "  Configured DXVK: profile=%s version=%s\r\n",
                state->configured_dxvk_profile[0] ? state->configured_dxvk_profile : "not set",
                state->configured_dxvk_version[0] ? state->configured_dxvk_version : "not set");
    append_text(report, size, "  d3d11.dll: %s\r\n", state->d3d11_path[0] ? state->d3d11_path : "not loaded");
    append_text(report, size, "  dxgi.dll: %s\r\n", state->dxgi_path[0] ? state->dxgi_path : "not loaded");
    append_text(report, size, "  Actual DXVK module evidence: d3d11=%s dxgi=%s\r\n",
                bool_text(state->d3d11_is_dxvk), bool_text(state->dxgi_is_dxvk));
    append_text(report, size, "  D3D11CreateDevice: %s (0x%08lx), FL=%s, adapter=%s\r\n",
                SUCCEEDED(state->d3d11_create_result) ? "PASS" : "FAIL",
                (unsigned long)state->d3d11_create_result, feature_level_text(state->d3d_feature_level),
                state->d3d_adapter[0] ? state->d3d_adapter : "unavailable");
    append_text(report, size, "\r\nJSON report: %s\r\n", state->result_path);
}

static void write_result_json(const struct diagnostics *state)
{
    char temporary[MAX_PATH + 32];
    char loader_version[32];
    char device_version[32];
    char device_name[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE + 16];
    char d3d11_path[MAX_PATH];
    char dxgi_path[MAX_PATH];
    char adapter[sizeof(state->d3d_adapter)];
    char backend[sizeof(state->requested_backend)];
    char profile[sizeof(state->configured_dxvk_profile)];
    char configured_version[sizeof(state->configured_dxvk_version)];
    char reason[256];
    FILE *file;
    const char *status = state->instance_result == VK_SUCCESS && state->physical_result == VK_SUCCESS &&
        SUCCEEDED(state->d3d11_create_result) ? "PASS" : "FAIL";

    if (!state->result_path[0] || !ensure_parent(state->result_path)) return;
    snprintf(temporary, sizeof(temporary), "%s.tmp.%lu", state->result_path, (unsigned long)GetCurrentProcessId());
    file = fopen(temporary, "wb");
    if (!file) return;
    version_text(state->loader_api, loader_version, sizeof(loader_version));
    version_text(state->properties.apiVersion, device_version, sizeof(device_version));
    json_safe_copy(device_name, sizeof(device_name), state->properties.deviceName);
    json_safe_copy(d3d11_path, sizeof(d3d11_path), state->d3d11_path);
    json_safe_copy(dxgi_path, sizeof(dxgi_path), state->dxgi_path);
    json_safe_copy(adapter, sizeof(adapter), state->d3d_adapter);
    json_safe_copy(backend, sizeof(backend), state->requested_backend);
    json_safe_copy(profile, sizeof(profile), state->configured_dxvk_profile);
    json_safe_copy(configured_version, sizeof(configured_version), state->configured_dxvk_version);
    build_modern_reason(state, reason, sizeof(reason));
    fprintf(file,
        "{\n  \"schemaVersion\": 1,\n  \"runId\": \"%s\",\n  \"testId\": \"%s\",\n"
        "  \"status\": \"%s\",\n  \"stage\": \"gpu-diagnostics\",\n"
        "  \"message\": \"Vulkan and active D3D11 backend diagnostics completed\",\n"
        "  \"architecture\": {\"peArchitecture\": \"%s\",\"wineUnixArchitecture\": \"x86_64\","
        "\"vulkanLoaderArchitecture\": \"%s\",\"venusIcdArchitecture\": \"%s\","
        "\"hostArchitecture\": \"%s\",\"wow64ThunkEnabled\": %s,\"box64Enabled\": %s},\n"
        "  \"vulkan\": {\"instanceResult\": %d, \"physicalDeviceResult\": %d,"
        "\"loaderApiVersion\": \"%s\", \"deviceApiVersion\": \"%s\", \"deviceName\": \"%s\","
        "\"vendorId\": %u, \"deviceId\": %u, \"driverVersion\": %u, \"graphicsQueueFamily\": %u},\n"
        "  \"features\": {\"geometryShader\": %s, \"tessellationShader\": %s, \"multiDrawIndirect\": %s,"
        "\"dualSrcBlend\": %s, \"multiViewport\": %s, \"robustBufferAccess\": %s,"
        "\"timelineSemaphore\": %s, \"bufferDeviceAddress\": %s, \"descriptorIndexing\": %s,"
        "\"scalarBlockLayout\": %s, \"synchronization2\": %s, \"dynamicRendering\": %s,"
        "\"maintenance4\": %s, \"robustBufferAccess2\": %s, \"robustImageAccess2\": %s,"
        "\"nullDescriptor\": %s, \"transformFeedback\": %s, \"geometryStreams\": %s},\n"
        "  \"formats\": {\"bc1\": %s, \"bc2\": %s, \"bc3\": %s, \"bc4\": %s, \"bc5\": %s, \"bc6\": %s, \"bc7\": %s,"
        "\"rgba8SnormColorAttachment\": %s, \"d24s8Sampled\": %s, \"d24s8DepthStencilAttachment\": %s},\n"
        "  \"dxvkModern26\": {\"eligible\": %s, \"reason\": \"%s\"},\n"
        "  \"activeD3D11\": {\"requestedBackend\": \"%s\", \"configuredProfile\": \"%s\","
        "\"configuredVersion\": \"%s\", \"d3d11Dll\": \"%s\", \"dxgiDll\": \"%s\","
        "\"d3d11ModuleIsDxvk\": %s, \"dxgiModuleIsDxvk\": %s, \"createDeviceResult\": \"0x%08lx\","
        "\"featureLevel\": \"%s\", \"adapter\": \"%s\"},\n"
        "  \"metrics\": {\"cpuReadBytes\": 0, \"cpuUploadBytes\": 0, \"gpuCopyCount\": 0, \"queueSubmitCount\": 0, \"durationMs\": %llu}\n}\n",
        state->run_id, state->test_id, status,
#ifdef _WIN64
        "x86_64",
#else
        "x86",
#endif
        getenv("WINEHUA_VULKAN_LOADER_ARCH") ? getenv("WINEHUA_VULKAN_LOADER_ARCH") : "unknown",
        getenv("WINEHUA_VENUS_ICD_ARCH") ? getenv("WINEHUA_VENUS_ICD_ARCH") : "unknown",
        getenv("WINEHUA_HOST_ARCH") ? getenv("WINEHUA_HOST_ARCH") : "unknown",
#ifdef _WIN64
        "false",
#else
        "true",
#endif
        getenv("USE_LIBBOX64") && getenv("USE_LIBBOX64")[0] == '1' ? "true" : "false",
        (int)state->instance_result, (int)state->physical_result, loader_version, device_version, device_name,
        state->properties.vendorID, state->properties.deviceID, state->properties.driverVersion, state->queue_family,
        bool_text(state->core.geometryShader), bool_text(state->core.tessellationShader), bool_text(state->core.multiDrawIndirect),
        bool_text(state->core.dualSrcBlend), bool_text(state->core.multiViewport), bool_text(state->core.robustBufferAccess),
        bool_text(state->timeline_semaphore), bool_text(state->buffer_device_address), bool_text(state->descriptor_indexing),
        bool_text(state->scalar_block_layout), bool_text(state->synchronization2), bool_text(state->dynamic_rendering),
        bool_text(state->maintenance4), bool_text(state->robust_buffer_access2), bool_text(state->robust_image_access2),
        bool_text(state->null_descriptor), bool_text(state->transform_feedback), bool_text(state->geometry_streams),
        bool_text(state->bc[0]), bool_text(state->bc[1]), bool_text(state->bc[2]), bool_text(state->bc[3]),
        bool_text(state->bc[4]), bool_text(state->bc[5]), bool_text(state->bc[6]), bool_text(state->rgba8_snorm_color_attachment),
        bool_text(state->d24s8_sampled), bool_text(state->d24s8_depth_stencil_attachment),
        bool_text(state->modern_eligible), reason, backend, profile, configured_version, d3d11_path, dxgi_path,
        bool_text(state->d3d11_is_dxvk), bool_text(state->dxgi_is_dxvk), (unsigned long)state->d3d11_create_result,
        feature_level_text(state->d3d_feature_level), adapter, (unsigned long long)(now_ms() - state->started_ms));
    fclose(file);
    MoveFileExA(temporary, state->result_path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}

static void write_text_report(const struct diagnostics *state, const char *report)
{
    FILE *file;
    if (!state->text_path[0] || !ensure_parent(state->text_path)) return;
    file = fopen(state->text_path, "wb");
    if (!file) return;
    fputs(report, file);
    fclose(file);
}

static char g_report[16384];

static void copy_report_to_clipboard(HWND window)
{
    HGLOBAL memory;
    char *destination;
    size_t bytes = strlen(g_report) + 1;
    if (!OpenClipboard(window)) return;
    EmptyClipboard();
    memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (memory) {
        destination = GlobalLock(memory);
        if (destination) {
            memcpy(destination, g_report, bytes);
            GlobalUnlock(memory);
            SetClipboardData(CF_TEXT, memory);
            memory = NULL;
        }
    }
    if (memory) GlobalFree(memory);
    CloseClipboard();
}

static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    switch (message) {
    case WM_CREATE: {
        RECT rect;
        GetClientRect(window, &rect);
        CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", g_report,
                        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_LEFT | ES_MULTILINE |
                        ES_AUTOVSCROLL | ES_READONLY,
                        8, 8, rect.right - 16, rect.bottom - 56,
                        window, (HMENU)(INT_PTR)100, GetModuleHandleA(NULL), NULL);
        CreateWindowExA(0, "BUTTON", "Copy report", WS_CHILD | WS_VISIBLE,
                        rect.right - 220, rect.bottom - 40, 100, 28,
                        window, (HMENU)(INT_PTR)101, GetModuleHandleA(NULL), NULL);
        CreateWindowExA(0, "BUTTON", "Close", WS_CHILD | WS_VISIBLE,
                        rect.right - 110, rect.bottom - 40, 100, 28,
                        window, (HMENU)(INT_PTR)102, GetModuleHandleA(NULL), NULL);
        return 0;
    }
    case WM_SIZE: {
        RECT rect;
        GetClientRect(window, &rect);
        MoveWindow(GetDlgItem(window, 100), 8, 8, rect.right - 16, rect.bottom - 56, TRUE);
        MoveWindow(GetDlgItem(window, 101), rect.right - 220, rect.bottom - 40, 100, 28, TRUE);
        MoveWindow(GetDlgItem(window, 102), rect.right - 110, rect.bottom - 40, 100, 28, TRUE);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wparam) == 101) {
            copy_report_to_clipboard(window);
            return 0;
        }
        if (LOWORD(wparam) == 102) {
            DestroyWindow(window);
            return 0;
        }
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcA(window, message, wparam, lparam);
}

static void show_report_window(HINSTANCE instance)
{
    WNDCLASSEXA window_class;
    RECT rect = { 0, 0, 960, 700 };
    HWND window;
    MSG message;

    memset(&window_class, 0, sizeof(window_class));
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = window_proc;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursor(NULL, IDC_ARROW);
    window_class.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    window_class.lpszClassName = "WineHuaGpuDiagnostics";
    if (!RegisterClassExA(&window_class)) return;
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
    window = CreateWindowExA(0, window_class.lpszClassName,
                             "WineHua Vulkan / DXVK Diagnostics",
                             WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                             CW_USEDEFAULT, CW_USEDEFAULT,
                             rect.right - rect.left, rect.bottom - rect.top,
                             NULL, NULL, instance, NULL);
    if (!window) return;
    while (GetMessageA(&message, NULL, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageA(&message);
    }
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE previous, LPSTR command_line, int show_command)
{
    struct diagnostics state;
    LPWSTR *argv;
    int argc = 0;
    (void)previous;
    (void)command_line;
    (void)show_command;

    memset(&state, 0, sizeof(state));
    state.started_ms = now_ms();
    state.instance_result = VK_NOT_READY;
    state.physical_result = VK_NOT_READY;
    state.d3d11_create_result = E_FAIL;
    state.queue_family = UINT32_MAX;
    copy_text(state.run_id, sizeof(state.run_id), "manual");
    copy_text(state.test_id, sizeof(state.test_id), "gpu-diagnostics");
    argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv) {
        state.automation = has_argument(argv, argc, L"--automation");
        argument_value(argv, argc, L"--run-id", state.run_id, sizeof(state.run_id));
        argument_value(argv, argc, L"--test-id", state.test_id, sizeof(state.test_id));
        argument_value(argv, argc, L"--result", state.result_path, sizeof(state.result_path));
        LocalFree(argv);
    }
    default_result_paths(&state);
    collect_vulkan(&state);
    collect_d3d11(&state);
    build_report(&state, g_report, sizeof(g_report));
    write_result_json(&state);
    if (!state.automation) {
        write_text_report(&state, g_report);
        show_report_window(instance);
    }
    return state.instance_result == VK_SUCCESS && state.physical_result == VK_SUCCESS &&
        SUCCEEDED(state.d3d11_create_result) ? 0 : 1;
}
