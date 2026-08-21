/*
 * WineHua DXVK 2.6.2 Vulkan requirements probe.
 *
 * This is a Win32 program on purpose. It validates the exact path DXVK sees:
 * Windows Vulkan -> winevulkan -> x86_64 Vulkan loader -> Venus -> Host Vulkan.
 * It does not load DXVK and therefore separates transport qualification from
 * DXVK compatibility-layer work.
 *
 * When launched outside automation, it also presents the measured Windows PE
 * path as a VKD3D capability report. It never loads vkd3d-proton.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <vulkan/vulkan.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "vkd3d_capability_audit.h"

struct probe_state {
    const char *run_id;
    const char *test_id;
    const char *result_path;
    ULONGLONG started_ms;
    uint32_t loader_api;
    VkPhysicalDeviceProperties properties;
    VkPhysicalDeviceFeatures core;
    uint32_t max_update_after_bind_descriptors_in_all_pools;
    uint32_t max_descriptor_set_update_after_bind_sampled_images;
    uint32_t max_descriptor_set_update_after_bind_storage_images;
    uint32_t max_descriptor_set_update_after_bind_storage_buffers;
    uint32_t queue_family;
    BOOL fallback_detected;
    char *capability_audit;
    BOOL interactive;
    BOOL initial_vkd3d_tab;

    BOOL api11;
    BOOL api12;
    BOOL api13;
    VkPhysicalDeviceVulkan11Features vkd3d_features11;
    VkPhysicalDeviceVulkan12Features vkd3d_features12;
    VkPhysicalDeviceVulkan13Features vkd3d_features13;
    VkPhysicalDeviceVulkan12Properties vkd3d_properties12;
    BOOL extension_descriptor_indexing;
    BOOL extension_timeline_semaphore;
    BOOL extension_sampler_mirror_clamp_to_edge;
    BOOL extension_create_renderpass2;
    BOOL extension_separate_depth_stencil_layouts;
    BOOL extension_bind_memory2;
    BOOL extension_copy_commands2;
    BOOL extension_dynamic_rendering;
    BOOL extension_buffer_device_address;
    BOOL extension_push_descriptor;
    BOOL extension_extended_dynamic_state;
    BOOL extension_extended_dynamic_state2;
    BOOL extension_image_view_min_lod;
    BOOL extension_mutable_descriptor_type;
    BOOL sampler_mirror_clamp_to_edge;
    BOOL shader_draw_parameters;
    BOOL create_renderpass2;
    BOOL separate_depth_stencil_layouts;
    BOOL bind_memory2;
    BOOL copy_commands2;
    BOOL push_descriptor;
    BOOL extended_dynamic_state;
    BOOL extended_dynamic_state2;
    BOOL robust_buffer_access2;
    BOOL robust_image_access2;
    BOOL null_descriptor;
    BOOL synchronization2;
    BOOL dynamic_rendering;
    BOOL maintenance4;
    BOOL timeline_semaphore;
    BOOL buffer_device_address;
    BOOL descriptor_indexing;
    BOOL transform_feedback;
    BOOL geometry_streams;

    BOOL dual_src_blend;
    BOOL multi_viewport;
    BOOL texture_compression_bc;
    BOOL rgba8_snorm_color_attachment;
    BOOL d24s8_sampled;
    BOOL d24s8_depth_stencil_attachment;
    BOOL bc1;
    BOOL bc2;
    BOOL bc3;
    BOOL bc4;
    BOOL bc5;
    BOOL bc6;
    BOOL bc7;

    BOOL transport_features_ready;
    BOOL transport_device_create_ok;
    VkResult transport_device_create_result;

    BOOL vkd3d_bindless_device_create_attempted;
    BOOL vkd3d_bindless_device_create_ok;
    VkResult vkd3d_bindless_device_create_result;

    BOOL timeline_round_trip_ok;
    VkResult timeline_semaphore_create_result;
    VkResult timeline_submit_result;
    VkResult timeline_wait_result;
    VkResult timeline_counter_result;
    uint64_t timeline_observed_value;
};

static ULONGLONG now_ms(void)
{
    FILETIME file_time;
    ULARGE_INTEGER value;
    GetSystemTimeAsFileTime(&file_time);
    value.LowPart = file_time.dwLowDateTime;
    value.HighPart = file_time.dwHighDateTime;
    return value.QuadPart / 10000ULL - 11644473600000ULL;
}

static const char *argument_value(int argc, char **argv, const char *name, const char *fallback)
{
    int i;
    for (i = 1; i + 1 < argc; ++i)
        if (!lstrcmpiA(argv[i], name)) return argv[i + 1];
    return fallback;
}

static void json_safe_copy(char *output, size_t output_size, const char *input)
{
    size_t written = 0;
    if (!output_size) return;
    while (input && *input && written + 1 < output_size) {
        unsigned char ch = (unsigned char)*input++;
        if (ch == '"' || ch == '\\' || ch < 0x20 || ch > 0x7e) ch = '_';
        output[written++] = (char)ch;
    }
    output[written] = 0;
}

static void version_text(uint32_t version, char *buffer, size_t size)
{
    snprintf(buffer, size, "%u.%u.%u", VK_API_VERSION_MAJOR(version),
             VK_API_VERSION_MINOR(version), VK_API_VERSION_PATCH(version));
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

static const char *bool_text(BOOL value)
{
    return value ? "true" : "false";
}

static BOOL argument_present(int argc, char **argv, const char *name)
{
    int i;
    for (i = 1; i < argc; ++i)
        if (!lstrcmpiA(argv[i], name)) return TRUE;
    return FALSE;
}

static BOOL vkd3d_bindless_features_complete(const struct probe_state *state)
{
    const VkPhysicalDeviceVulkan12Features *features = &state->vkd3d_features12;

    return state->extension_descriptor_indexing &&
        features->shaderUniformBufferArrayNonUniformIndexing &&
        features->shaderSampledImageArrayNonUniformIndexing &&
        features->shaderStorageBufferArrayNonUniformIndexing &&
        features->shaderStorageImageArrayNonUniformIndexing &&
        features->shaderUniformTexelBufferArrayNonUniformIndexing &&
        features->shaderStorageTexelBufferArrayNonUniformIndexing &&
        features->descriptorBindingUniformBufferUpdateAfterBind &&
        features->descriptorBindingSampledImageUpdateAfterBind &&
        features->descriptorBindingStorageImageUpdateAfterBind &&
        features->descriptorBindingStorageBufferUpdateAfterBind &&
        features->descriptorBindingUniformTexelBufferUpdateAfterBind &&
        features->descriptorBindingStorageTexelBufferUpdateAfterBind &&
        features->descriptorBindingUpdateUnusedWhilePending &&
        features->descriptorBindingPartiallyBound &&
        features->descriptorBindingVariableDescriptorCount &&
        features->runtimeDescriptorArray;
}

static BOOL update_after_bind_limits_complete(const struct probe_state *state,
                                              uint32_t view_descriptor_minimum)
{
    const VkPhysicalDeviceVulkan12Properties *properties = &state->vkd3d_properties12;

    return properties->maxPerStageDescriptorUpdateAfterBindSamplers >= 2048u &&
        properties->maxDescriptorSetUpdateAfterBindSamplers >= 2048u &&
        properties->maxPerStageDescriptorUpdateAfterBindUniformBuffers >= view_descriptor_minimum &&
        properties->maxPerStageDescriptorUpdateAfterBindStorageBuffers >= view_descriptor_minimum &&
        properties->maxPerStageDescriptorUpdateAfterBindSampledImages >= view_descriptor_minimum &&
        properties->maxPerStageDescriptorUpdateAfterBindStorageImages >= view_descriptor_minimum &&
        properties->maxDescriptorSetUpdateAfterBindUniformBuffers >= view_descriptor_minimum &&
        properties->maxDescriptorSetUpdateAfterBindStorageBuffers >= view_descriptor_minimum &&
        properties->maxDescriptorSetUpdateAfterBindSampledImages >= view_descriptor_minimum &&
        properties->maxDescriptorSetUpdateAfterBindStorageImages >= view_descriptor_minimum;
}

static BOOL vkd3d26_supported(const struct probe_state *state)
{
    return state->api11 && vkd3d_bindless_features_complete(state) &&
        update_after_bind_limits_complete(state, 1000000u) && state->timeline_semaphore &&
        state->extension_sampler_mirror_clamp_to_edge && state->robust_buffer_access2 &&
        state->robust_image_access2 && state->null_descriptor &&
        state->separate_depth_stencil_layouts && state->bind_memory2 &&
        state->create_renderpass2 && state->copy_commands2 &&
        state->vkd3d_bindless_device_create_ok;
}

static BOOL vkd3d28_supported(const struct probe_state *state)
{
    return state->api11 && vkd3d_bindless_features_complete(state) &&
        update_after_bind_limits_complete(state, 1000000u) && state->timeline_semaphore &&
        state->extension_sampler_mirror_clamp_to_edge && state->robust_buffer_access2 &&
        state->robust_image_access2 && state->null_descriptor &&
        state->separate_depth_stencil_layouts && state->bind_memory2 &&
        state->copy_commands2 && state->dynamic_rendering &&
        state->extended_dynamic_state && state->extended_dynamic_state2 &&
        state->buffer_device_address && state->push_descriptor &&
        state->vkd3d_bindless_device_create_ok;
}

static BOOL vkd3d29_supported(const struct probe_state *state)
{
    return state->api13 && vkd3d_bindless_features_complete(state) &&
        update_after_bind_limits_complete(state, 1000000u) && state->sampler_mirror_clamp_to_edge &&
        state->shader_draw_parameters && state->robust_buffer_access2 &&
        state->robust_image_access2 && state->null_descriptor && state->push_descriptor &&
        state->vkd3d_bindless_device_create_ok;
}

static BOOL vkd3d26_limited_500k_candidate(const struct probe_state *state)
{
    return state->api11 && vkd3d_bindless_features_complete(state) &&
        update_after_bind_limits_complete(state, 500000u) && state->timeline_semaphore &&
        state->extension_sampler_mirror_clamp_to_edge && state->robust_buffer_access2 &&
        state->robust_image_access2 && state->null_descriptor &&
        state->separate_depth_stencil_layouts && state->bind_memory2 &&
        state->create_renderpass2 && state->copy_commands2 &&
        state->vkd3d_bindless_device_create_ok;
}

static BOOL dxvk_transport_supported(const struct probe_state *state)
{
    return state->transport_device_create_ok && state->timeline_round_trip_ok;
}

static void append_report_text(char *buffer, size_t buffer_size, const char *format, ...)
{
    size_t used = strlen(buffer);
    va_list arguments;

    if (used >= buffer_size - 1) return;
    va_start(arguments, format);
    vsnprintf(buffer + used, buffer_size - used, format, arguments);
    va_end(arguments);
}

static void append_feature_result(char *buffer, size_t buffer_size, const char *name,
                                  BOOL passed, const char *failure_reason)
{
    append_report_text(buffer, buffer_size, "[%s] %s%s%s\r\n",
                       passed ? "OK" : "FAIL", name,
                       passed ? "" : " - ", passed ? "" : failure_reason);
}

static void append_informational_feature_result(char *buffer, size_t buffer_size, const char *name,
                                                BOOL enabled, const char *note)
{
    append_report_text(buffer, buffer_size, "[INFO] %s: %s - %s\r\n",
                       name, enabled ? "enabled" : "disabled", note);
}

static void append_uab_limit_pair(char *buffer, size_t buffer_size, const char *name,
                                  uint32_t per_stage, uint32_t per_set, uint32_t minimum)
{
    BOOL passed = per_stage >= minimum && per_set >= minimum;
    append_report_text(buffer, buffer_size,
                       "[%s] %s (per-stage / per-set): %u / %u%s\r\n",
                       passed ? "OK" : "FAIL", name, per_stage, per_set,
                       passed ? "" : " - below required minimum");
}

static void append_descriptor_indexing_feature_results(char *buffer, size_t buffer_size,
                                                       const struct probe_state *state)
{
    const VkPhysicalDeviceVulkan12Features *features = &state->vkd3d_features12;

    append_informational_feature_result(buffer, buffer_size, "inputAttachment dynamic indexing",
                                        features->shaderInputAttachmentArrayDynamicIndexing,
                                        "not required by the vkd3d bindless view heap");
    append_feature_result(buffer, buffer_size, "uniform texel dynamic indexing",
                          features->shaderUniformTexelBufferArrayDynamicIndexing,
                          "descriptor indexing field is disabled");
    append_feature_result(buffer, buffer_size, "storage texel dynamic indexing",
                          features->shaderStorageTexelBufferArrayDynamicIndexing,
                          "descriptor indexing field is disabled");
    append_informational_feature_result(buffer, buffer_size, "uniform buffer non-uniform indexing",
                                        features->shaderUniformBufferArrayNonUniformIndexing,
                                        "storage-buffer CBV fallback is used when unavailable");
    append_feature_result(buffer, buffer_size, "sampled image non-uniform indexing",
                          features->shaderSampledImageArrayNonUniformIndexing,
                          "descriptor indexing field is disabled");
    append_feature_result(buffer, buffer_size, "storage buffer non-uniform indexing",
                          features->shaderStorageBufferArrayNonUniformIndexing,
                          "descriptor indexing field is disabled");
    append_feature_result(buffer, buffer_size, "storage image non-uniform indexing",
                          features->shaderStorageImageArrayNonUniformIndexing,
                          "descriptor indexing field is disabled");
    append_informational_feature_result(buffer, buffer_size, "input attachment non-uniform indexing",
                                        features->shaderInputAttachmentArrayNonUniformIndexing,
                                        "not required by the vkd3d bindless view heap");
    append_feature_result(buffer, buffer_size, "uniform texel non-uniform indexing",
                          features->shaderUniformTexelBufferArrayNonUniformIndexing,
                          "descriptor indexing field is disabled");
    append_feature_result(buffer, buffer_size, "storage texel non-uniform indexing",
                          features->shaderStorageTexelBufferArrayNonUniformIndexing,
                          "descriptor indexing field is disabled");
    append_informational_feature_result(buffer, buffer_size, "uniform buffer UpdateAfterBind",
                                        features->descriptorBindingUniformBufferUpdateAfterBind,
                                        "storage-buffer CBV fallback is used when unavailable");
    append_feature_result(buffer, buffer_size, "sampled image UpdateAfterBind",
                          features->descriptorBindingSampledImageUpdateAfterBind,
                          "descriptor indexing field is disabled");
    append_feature_result(buffer, buffer_size, "storage image UpdateAfterBind",
                          features->descriptorBindingStorageImageUpdateAfterBind,
                          "descriptor indexing field is disabled");
    append_feature_result(buffer, buffer_size, "storage buffer UpdateAfterBind",
                          features->descriptorBindingStorageBufferUpdateAfterBind,
                          "descriptor indexing field is disabled");
    append_feature_result(buffer, buffer_size, "uniform texel UpdateAfterBind",
                          features->descriptorBindingUniformTexelBufferUpdateAfterBind,
                          "descriptor indexing field is disabled");
    append_feature_result(buffer, buffer_size, "storage texel UpdateAfterBind",
                          features->descriptorBindingStorageTexelBufferUpdateAfterBind,
                          "descriptor indexing field is disabled");
    append_feature_result(buffer, buffer_size, "UpdateUnusedWhilePending",
                          features->descriptorBindingUpdateUnusedWhilePending,
                          "descriptor indexing field is disabled");
    append_feature_result(buffer, buffer_size, "PartiallyBound", features->descriptorBindingPartiallyBound,
                          "descriptor indexing field is disabled");
    append_feature_result(buffer, buffer_size, "VariableDescriptorCount",
                          features->descriptorBindingVariableDescriptorCount,
                          "descriptor indexing field is disabled");
    append_feature_result(buffer, buffer_size, "runtimeDescriptorArray", features->runtimeDescriptorArray,
                          "descriptor indexing field is disabled");
}

struct capability_report_window {
    HWND tabs;
    HWND dxvk_text;
    HWND vkd3d_text;
    HWND close_button;
    int selected_tab;
};

enum {
    REPORT_TABS = 1001,
    REPORT_CLOSE = 1002
};

static void set_control_font(HWND control)
{
    SendMessageA(control, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
}

static void update_report_layout(struct capability_report_window *report, HWND window)
{
    RECT client;
    int width;
    int height;

    GetClientRect(window, &client);
    width = client.right - client.left;
    height = client.bottom - client.top;
    MoveWindow(report->tabs, 12, 12, width - 24, height - 66, TRUE);
    MoveWindow(report->dxvk_text, 30, 54, width - 60, height - 132, TRUE);
    MoveWindow(report->vkd3d_text, 30, 54, width - 60, height - 132, TRUE);
    MoveWindow(report->close_button, width - 100, height - 42, 88, 28, TRUE);
}

static void select_report_tab(struct capability_report_window *report, int tab)
{
    report->selected_tab = tab == 1 ? 1 : 0;
    TabCtrl_SetCurSel(report->tabs, report->selected_tab);
    ShowWindow(report->dxvk_text, report->selected_tab == 0 ? SW_SHOW : SW_HIDE);
    ShowWindow(report->vkd3d_text, report->selected_tab == 1 ? SW_SHOW : SW_HIDE);
}

static LRESULT CALLBACK capability_report_wndproc(HWND window, UINT message,
                                                   WPARAM wparam, LPARAM lparam)
{
    struct capability_report_window *report =
        (struct capability_report_window *)GetWindowLongPtrA(window, GWLP_USERDATA);

    switch (message) {
    case WM_CREATE:
        report = (struct capability_report_window *)((CREATESTRUCTA *)lparam)->lpCreateParams;
        SetWindowLongPtrA(window, GWLP_USERDATA, (LONG_PTR)report);
        return 0;
    case WM_SIZE:
        if (report) update_report_layout(report, window);
        return 0;
    case WM_NOTIFY:
        if (report && ((NMHDR *)lparam)->idFrom == REPORT_TABS &&
            ((NMHDR *)lparam)->code == TCN_SELCHANGE) {
            select_report_tab(report, TabCtrl_GetCurSel(report->tabs));
        }
        return 0;
    case WM_COMMAND:
        if (LOWORD(wparam) == REPORT_CLOSE) DestroyWindow(window);
        return 0;
    case WM_CLOSE:
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcA(window, message, wparam, lparam);
    }
}

static BOOL register_capability_report_class(HINSTANCE instance)
{
    WNDCLASSA window_class;
    static const char class_name[] = "WineHuaCapabilityReport";

    if (GetClassInfoA(instance, class_name, &window_class)) return TRUE;
    memset(&window_class, 0, sizeof(window_class));
    window_class.lpfnWndProc = capability_report_wndproc;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorA(NULL, IDC_ARROW);
    window_class.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    window_class.lpszClassName = class_name;
    return RegisterClassA(&window_class) != 0;
}

static void show_capability_report(const struct probe_state *state,
                                   const char *status, const char *message)
{
    static const char class_name[] = "WineHuaCapabilityReport";
    HINSTANCE instance = GetModuleHandleA(NULL);
    struct capability_report_window report;
    INITCOMMONCONTROLSEX common_controls;
    TCITEMA item;
    HWND window;
    MSG message_loop;
    char device_name[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE + 16];
    char api_version[32];
    char dxvk_report[4096];
    char vkd3d_report[16384];
    BOOL dxvk_supported = dxvk_transport_supported(state);
    BOOL vkd3d26 = vkd3d26_supported(state);
    BOOL vkd3d28 = vkd3d28_supported(state);
    BOOL vkd3d29 = vkd3d29_supported(state);
    BOOL vkd3d500k = vkd3d26_limited_500k_candidate(state);
    BOOL manual_evidence_only = state->interactive;

    json_safe_copy(device_name, sizeof(device_name), state->properties.deviceName);
    version_text(state->properties.apiVersion, api_version, sizeof(api_version));
    dxvk_report[0] = 0;
    append_report_text(dxvk_report, sizeof(dxvk_report),
                       "DXVK 2.6.2 transport capability\r\n\r\n"
                       "Scope: Windows PE -> winevulkan -> Venus\r\n"
                       "Adapter: %s\r\nVulkan API: %s\r\n"
                       "Probe status: %s%s%s\r\n\r\n"
                       "Required transport features\r\n",
                       device_name[0] ? device_name : "unavailable", api_version, status,
                       message && message[0] ? " - " : "", message && message[0] ? message : "");
    append_feature_result(dxvk_report, sizeof(dxvk_report), "Vulkan API 1.3", state->api13,
                          "adapter exposes Vulkan below 1.3");
    append_feature_result(dxvk_report, sizeof(dxvk_report), "core robustBufferAccess",
                          state->core.robustBufferAccess, "core feature is disabled");
    append_feature_result(dxvk_report, sizeof(dxvk_report), "robustBufferAccess2",
                          state->robust_buffer_access2, "VK_EXT_robustness2 feature is unavailable");
    append_feature_result(dxvk_report, sizeof(dxvk_report), "robustImageAccess2",
                          state->robust_image_access2, "VK_EXT_robustness2 feature is unavailable");
    append_feature_result(dxvk_report, sizeof(dxvk_report), "nullDescriptor",
                          state->null_descriptor, "VK_EXT_robustness2 feature is unavailable");
    append_feature_result(dxvk_report, sizeof(dxvk_report), "timelineSemaphore",
                          state->timeline_semaphore, "Vulkan 1.2 timeline feature is unavailable");
    append_feature_result(dxvk_report, sizeof(dxvk_report), "synchronization2",
                          state->synchronization2, "Vulkan 1.3 feature is unavailable");
    append_feature_result(dxvk_report, sizeof(dxvk_report), "dynamicRendering",
                          state->dynamic_rendering, "Vulkan 1.3 feature is unavailable");
    append_feature_result(dxvk_report, sizeof(dxvk_report), "maintenance4",
                          state->maintenance4, "Vulkan 1.3 feature is unavailable");
    append_report_text(dxvk_report, sizeof(dxvk_report), "\r\nRuntime checks\r\n");
    if (manual_evidence_only) {
        append_report_text(dxvk_report, sizeof(dxvk_report),
                           "NOT RUN in the interactive report.\r\n"
                           "The automated DXVK suite owns device creation and QueueSubmit2 "
                           "round-trip validation; this prevents a failed transport from "
                           "hiding the capability window.\r\n\r\n"
                           "DXVK result on this PE path: CAPABILITY EVIDENCE COLLECTED "
                           "(not an integration verdict)\r\n"
                           "This probe does not load or replace DXVK DLLs.\r\n");
    } else {
        append_feature_result(dxvk_report, sizeof(dxvk_report), "Vulkan device creation",
                              state->transport_device_create_ok,
                              "required DXVK transport features could not create a device");
        append_feature_result(dxvk_report, sizeof(dxvk_report), "QueueSubmit2 timeline round trip",
                              state->timeline_round_trip_ok,
                              "timeline submit, wait, or counter verification failed");
        append_report_text(dxvk_report, sizeof(dxvk_report),
                           "\r\nDXVK support on this PE path: %s\r\n"
                           "This probe does not load or replace DXVK DLLs.\r\n",
                           dxvk_supported ? "SUPPORTED" : "UNSUPPORTED");
    }

    vkd3d_report[0] = 0;
    append_report_text(vkd3d_report, sizeof(vkd3d_report),
                       "VKD3D capability evidence\r\n\r\n"
                       "Scope: Windows PE -> winevulkan -> Venus\r\n"
                       "vkd3d-proton loaded: no\r\n"
                       "Adapter: %s\r\nVulkan API: %s\r\n\r\n"
                       "Version verdicts from upstream driver requirements\r\n",
                       device_name[0] ? device_name : "unavailable", api_version);
    append_feature_result(vkd3d_report, sizeof(vkd3d_report), "vkd3d-proton 2.6 upstream (Vulkan 1.1)",
                          vkd3d26, "one or more mandatory 2.6 rows below failed");
    append_feature_result(vkd3d_report, sizeof(vkd3d_report), "vkd3d-proton 2.8 (Vulkan 1.1)",
                          vkd3d28, "one or more mandatory 2.8 rows below failed");
    append_feature_result(vkd3d_report, sizeof(vkd3d_report), "vkd3d-proton 2.9 Modern (Vulkan 1.3)",
                          vkd3d29, "one or more mandatory 2.9 rows below failed");
    append_feature_result(vkd3d_report, sizeof(vkd3d_report),
                          "Experimental 500K 2.6 candidate (default off)", vkd3d500k,
                          "same required features are not available, or a view limit is below 500000");
    append_feature_result(vkd3d_report, sizeof(vkd3d_report),
                          "2.6 bindless feature-chain Vulkan device creation",
                          state->vkd3d_bindless_device_create_ok,
                          "the requested descriptor-indexing, timeline, and robustness2 chain was rejected");
    append_report_text(vkd3d_report, sizeof(vkd3d_report),
                       "\r\nAPI baseline\r\n");
    append_feature_result(vkd3d_report, sizeof(vkd3d_report), "Vulkan API 1.1 (2.6 / 2.8)",
                          state->api11, "adapter exposes Vulkan below 1.1");
    append_feature_result(vkd3d_report, sizeof(vkd3d_report), "Vulkan API 1.3 (2.9 Modern only)",
                          state->api13, "adapter exposes Vulkan below 1.3");
    append_report_text(vkd3d_report, sizeof(vkd3d_report),
                       "\r\nDescriptor indexing feature inventory (required rows gate the bindless view heap)\r\n");
    append_feature_result(vkd3d_report, sizeof(vkd3d_report),
                          "VK_EXT_descriptor_indexing or Vulkan 1.2", state->extension_descriptor_indexing,
                          "neither the extension nor Vulkan 1.2 core capability is exposed");
    append_descriptor_indexing_feature_results(vkd3d_report, sizeof(vkd3d_report), state);
    append_report_text(vkd3d_report, sizeof(vkd3d_report),
                       "\r\nUpstream resource descriptor limits (per-stage and per-set, minimum 1000000)\r\n");
    append_uab_limit_pair(vkd3d_report, sizeof(vkd3d_report), "Sampled images",
                          state->vkd3d_properties12.maxPerStageDescriptorUpdateAfterBindSampledImages,
                          state->vkd3d_properties12.maxDescriptorSetUpdateAfterBindSampledImages,
                          1000000u);
    append_uab_limit_pair(vkd3d_report, sizeof(vkd3d_report), "Storage images",
                          state->vkd3d_properties12.maxPerStageDescriptorUpdateAfterBindStorageImages,
                          state->vkd3d_properties12.maxDescriptorSetUpdateAfterBindStorageImages,
                          1000000u);
    append_uab_limit_pair(vkd3d_report, sizeof(vkd3d_report), "Storage buffers",
                          state->vkd3d_properties12.maxPerStageDescriptorUpdateAfterBindStorageBuffers,
                          state->vkd3d_properties12.maxDescriptorSetUpdateAfterBindStorageBuffers,
                          1000000u);
    append_report_text(vkd3d_report, sizeof(vkd3d_report),
                       "\r\nSampler descriptor limits (per-stage and per-set, minimum 2048)\r\n");
    append_uab_limit_pair(vkd3d_report, sizeof(vkd3d_report), "Samplers",
                          state->vkd3d_properties12.maxPerStageDescriptorUpdateAfterBindSamplers,
                          state->vkd3d_properties12.maxDescriptorSetUpdateAfterBindSamplers,
                          2048u);
    append_report_text(vkd3d_report, sizeof(vkd3d_report),
                       "[INFO] Input attachments (per-stage / per-set): %u / %u - not a D3D12 view-heap gate\r\n"
                       "[INFO] Experimental 500K profile is default off; it never advances Gate B.\r\n",
                       state->vkd3d_properties12.maxPerStageDescriptorUpdateAfterBindInputAttachments,
                       state->vkd3d_properties12.maxDescriptorSetUpdateAfterBindInputAttachments);
    append_report_text(vkd3d_report, sizeof(vkd3d_report),
                       "Observed descriptors in all pools: %u\r\n\r\n"
                       "Mandatory extension and feature inventory\r\n",
                       state->vkd3d_properties12.maxUpdateAfterBindDescriptorsInAllPools);
    append_feature_result(vkd3d_report, sizeof(vkd3d_report), "timelineSemaphore (VK_KHR / core 1.2)",
                          state->timeline_semaphore, "required timeline semaphore feature is unavailable");
    append_feature_result(vkd3d_report, sizeof(vkd3d_report), "VK_KHR_sampler_mirror_clamp_to_edge (2.6 / 2.8)",
                          state->extension_sampler_mirror_clamp_to_edge,
                          "required Legacy extension is unavailable");
    append_feature_result(vkd3d_report, sizeof(vkd3d_report), "samplerMirrorClampToEdge feature (2.9 Modern)",
                          state->sampler_mirror_clamp_to_edge,
                          "required Modern core feature is unavailable");
    append_feature_result(vkd3d_report, sizeof(vkd3d_report), "VK_EXT_robustness2 robustBufferAccess2",
                          state->robust_buffer_access2, "required robustness2 feature is unavailable");
    append_feature_result(vkd3d_report, sizeof(vkd3d_report), "VK_EXT_robustness2 robustImageAccess2",
                          state->robust_image_access2, "required robustness2 feature is unavailable");
    append_feature_result(vkd3d_report, sizeof(vkd3d_report), "VK_EXT_robustness2 nullDescriptor",
                          state->null_descriptor, "required robustness2 feature is unavailable");
    append_feature_result(vkd3d_report, sizeof(vkd3d_report), "separate depth/stencil layouts (VK_KHR / core 1.2)",
                          state->separate_depth_stencil_layouts, "required layout capability is unavailable");
    append_feature_result(vkd3d_report, sizeof(vkd3d_report), "bind memory2 (VK_KHR / core 1.1)",
                          state->bind_memory2, "required memory-binding capability is unavailable");
    append_feature_result(vkd3d_report, sizeof(vkd3d_report), "create renderpass2 (2.6; VK_KHR / core 1.2)",
                          state->create_renderpass2, "required by vkd3d-proton 2.6");
    append_feature_result(vkd3d_report, sizeof(vkd3d_report), "copy commands2 (VK_KHR / core 1.3)",
                          state->copy_commands2, "required copy-commands capability is unavailable");
    append_feature_result(vkd3d_report, sizeof(vkd3d_report), "dynamicRendering (2.8; VK_KHR / core 1.3)",
                          state->dynamic_rendering, "required by vkd3d-proton 2.8");
    append_feature_result(vkd3d_report, sizeof(vkd3d_report), "VK_EXT_extended_dynamic_state (2.8)",
                          state->extended_dynamic_state, "required by vkd3d-proton 2.8");
    append_feature_result(vkd3d_report, sizeof(vkd3d_report), "VK_EXT_extended_dynamic_state2 (2.8)",
                          state->extended_dynamic_state2, "required by vkd3d-proton 2.8");
    append_feature_result(vkd3d_report, sizeof(vkd3d_report), "bufferDeviceAddress (2.8; VK_KHR / core 1.2)",
                          state->buffer_device_address, "required by vkd3d-proton 2.8");
    append_feature_result(vkd3d_report, sizeof(vkd3d_report), "VK_KHR_push_descriptor (2.8 / 2.9)",
                          state->push_descriptor, "required push descriptor extension is unavailable");
    append_feature_result(vkd3d_report, sizeof(vkd3d_report), "shaderDrawParameters (2.9 Modern)",
                          state->shader_draw_parameters, "required by vkd3d-proton 2.9");
    append_report_text(vkd3d_report, sizeof(vkd3d_report),
                       "\r\nRecommended, not gating\r\n");
    append_feature_result(vkd3d_report, sizeof(vkd3d_report), "VK_EXT_image_view_min_lod",
                          state->extension_image_view_min_lod, "upstream recommends it for optimal behavior");
    append_feature_result(vkd3d_report, sizeof(vkd3d_report),
                          "VK_EXT_mutable_descriptor_type or VK_VALVE alias",
                          state->extension_mutable_descriptor_type,
                          "upstream recommends it; it is not mandatory");
    append_report_text(vkd3d_report, sizeof(vkd3d_report),
                       "\r\nThis is Windows PE path evidence only. It does not load VKD3D DLLs.\r\n"
                       "Integration remains blocked unless Host, Guest, and Wine probes match this device capability.\r\n");
    printf("DXVK 2.6.2: %s\nVKD3D Proton 2.6 upstream: %s\n"
           "VKD3D Proton 2.8: %s\nVKD3D Proton 2.9: %s\n"
           "VKD3D experimental 500K candidate: %s\n",
           manual_evidence_only ? "EVIDENCE ONLY" :
               (dxvk_supported ? "SUPPORTED" : "UNSUPPORTED"),
           vkd3d26 ? "SUPPORTED" : "UNSUPPORTED",
           vkd3d28 ? "SUPPORTED" : "UNSUPPORTED",
           vkd3d29 ? "SUPPORTED" : "UNSUPPORTED",
           vkd3d500k ? "CANDIDATE" : "NOT READY");
    fflush(stdout);

    common_controls.dwSize = sizeof(common_controls);
    common_controls.dwICC = ICC_TAB_CLASSES;
    InitCommonControlsEx(&common_controls);
    if (!register_capability_report_class(instance)) return;

    memset(&report, 0, sizeof(report));
    report.selected_tab = state->initial_vkd3d_tab ? 1 : 0;
    window = CreateWindowExA(WS_EX_DLGMODALFRAME, class_name,
                             "WineHua Graphics Capability Probe",
                             WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME |
                             WS_MINIMIZEBOX | WS_MAXIMIZEBOX,
                             CW_USEDEFAULT, CW_USEDEFAULT, 760, 590,
                             NULL, NULL, instance, &report);
    if (!window) return;
    report.tabs = CreateWindowExA(0, WC_TABCONTROLA, "",
                                  WS_CHILD | WS_CLIPSIBLINGS | WS_VISIBLE,
                                  0, 0, 0, 0, window, (HMENU)(INT_PTR)REPORT_TABS,
                                  instance, NULL);
    report.dxvk_text = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", dxvk_report,
                                       WS_CHILD | ES_LEFT | ES_MULTILINE | ES_READONLY |
                                       ES_AUTOVSCROLL | WS_VSCROLL,
                                       0, 0, 0, 0, window, NULL, instance, NULL);
    report.vkd3d_text = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", vkd3d_report,
                                        WS_CHILD | ES_LEFT | ES_MULTILINE | ES_READONLY |
                                        ES_AUTOVSCROLL | WS_VSCROLL,
                                        0, 0, 0, 0, window, NULL, instance, NULL);
    report.close_button = CreateWindowExA(0, "BUTTON", "Close",
                                          WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                          0, 0, 0, 0, window,
                                          (HMENU)(INT_PTR)REPORT_CLOSE, instance, NULL);
    if (!report.tabs || !report.dxvk_text || !report.vkd3d_text || !report.close_button) {
        DestroyWindow(window);
        return;
    }
    set_control_font(report.tabs);
    set_control_font(report.dxvk_text);
    set_control_font(report.vkd3d_text);
    set_control_font(report.close_button);
    memset(&item, 0, sizeof(item));
    item.mask = TCIF_TEXT;
    item.pszText = "DXVK 2.6.2";
    TabCtrl_InsertItem(report.tabs, 0, &item);
    item.pszText = "VKD3D Capability";
    TabCtrl_InsertItem(report.tabs, 1, &item);
    update_report_layout(&report, window);
    select_report_tab(&report, report.selected_tab);
    ShowWindow(window, SW_SHOW);
    UpdateWindow(window);
    SetForegroundWindow(window);
    while (GetMessageA(&message_loop, NULL, 0, 0) > 0) {
        TranslateMessage(&message_loop);
        DispatchMessageA(&message_loop);
    }
}

static void write_progress(const struct probe_state *state, const char *stage)
{
    char progress[512];
    char temporary[1024];
    const char *vn_perf = getenv("VN_PERF");
    const BOOL no_semaphore_feedback =
        vn_perf && strstr(vn_perf, "no_semaphore_feedback");
    FILE *file;

    if (!state->result_path || !state->result_path[0]) return;
    snprintf(progress, sizeof(progress), "%s.progress", state->result_path);
    snprintf(temporary, sizeof(temporary), "%s.tmp.%lu", progress,
             (unsigned long)GetCurrentProcessId());
    if (!ensure_parent(progress)) return;
    file = fopen(temporary, "wb");
    if (!file) return;
    fprintf(file, "{\"pid\":%lu,\"stage\":\"%s\",\"timestampMs\":%llu,"
            "\"vnPerfNoSemaphoreFeedback\":%s}\n",
            (unsigned long)GetCurrentProcessId(), stage ? stage : "unknown",
            (unsigned long long)now_ms(), bool_text(no_semaphore_feedback));
    fflush(file);
    fclose(file);
    MoveFileExA(temporary, progress,
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}

static const char *policy_text(BOOL native_supported, const char *missing_policy)
{
    return native_supported ? "native" : missing_policy;
}

static void write_result(const struct probe_state *state, const char *status,
                         const char *message)
{
    char temporary[MAX_PATH];
    char loader_version[32];
    char device_version[32];
    char device_name[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE + 16];
    char safe_message[256];
    FILE *file;

    if (!state->result_path || !state->result_path[0]) return;
    if (!ensure_parent(state->result_path)) return;
    snprintf(temporary, sizeof(temporary), "%s.tmp.%lu", state->result_path,
             (unsigned long)GetCurrentProcessId());
    version_text(state->loader_api, loader_version, sizeof(loader_version));
    version_text(state->properties.apiVersion, device_version, sizeof(device_version));
    json_safe_copy(device_name, sizeof(device_name), state->properties.deviceName);
    json_safe_copy(safe_message, sizeof(safe_message), message ? message : "");
    file = fopen(temporary, "wb");
    if (!file) return;

    fprintf(file,
            "{\n"
            "  \"schemaVersion\": 1,\n"
            "  \"runId\": \"%s\",\n"
            "  \"testId\": \"%s\",\n"
            "  \"status\": \"%s\",\n"
            "  \"stage\": \"dxvk26-requirements\",\n"
            "  \"message\": \"%s\",\n"
            "  \"pid\": %lu,\n"
            "  \"architecture\": {\"peArchitecture\": \"%s\","
            "\"wineUnixArchitecture\": \"%s\","
            "\"vulkanLoaderArchitecture\": \"%s\","
            "\"venusIcdArchitecture\": \"%s\","
            "\"hostArchitecture\": \"%s\","
            "\"wow64ThunkEnabled\": %s,\"box64Enabled\": %s},\n"
            "  \"capabilities\": {\"loaderApiVersion\": \"%s\","
            "\"deviceApiVersion\": \"%s\",\"deviceName\": \"%s\","
            "\"vendorId\":%u,\"deviceId\":%u,\"driverVersion\":%u,"
            "\"graphicsQueueFamily\":%u,\"pushConstantBytes\":%u},\n"
            "  \"dxvk262\": {\n"
            "    \"transport\": {\"api13\":%s,\"coreRobustBufferAccess\":%s,"
            "\"robustBufferAccess2\":%s,\"robustImageAccess2\":%s,"
            "\"nullDescriptor\":%s,\"synchronization2\":%s,"
            "\"dynamicRendering\":%s,\"maintenance4\":%s,"
            "\"deviceCreateAttempted\":%s,\"deviceCreateResult\":%d,"
            "\"passed\":%s},\n"
            "    \"timelineRoundTrip\": {\"feature\":%s,"
            "\"semaphoreCreateResult\":%d,\"queueSubmitResult\":%d,"
            "\"waitResult\":%d,\"counterResult\":%d,"
            "\"observedValue\":%llu,\"passed\":%s},\n"
            "    \"vulkan12\": {\"timelineSemaphore\":%s,"
            "\"bufferDeviceAddress\":%s,\"descriptorIndexing\":%s},\n"
            "    \"updateAfterBindLimits\": {\"maxUpdateAfterBindDescriptorsInAllPools\":%u,"
            "\"maxDescriptorSetUpdateAfterBindSampledImages\":%u,"
            "\"maxDescriptorSetUpdateAfterBindStorageImages\":%u,"
            "\"maxDescriptorSetUpdateAfterBindStorageBuffers\":%u},\n"
            "    \"d3d11Features\": {\"geometryShader\":%s,"
            "\"tessellationShader\":%s,\"multiDrawIndirect\":%s,"
            "\"dualSrcBlend\":\"%s\",\"multiViewport\":\"%s\","
            "\"transformFeedback\":\"%s\",\"geometryStreams\":\"%s\"},\n"
            "    \"formats\": {\"bc1\":%s,\"bc2\":%s,\"bc3\":%s,"
            "\"bc4\":%s,\"bc5\":%s,\"bc6\":%s,\"bc7\":%s,"
            "\"rgba8SnormColorAttachment\":%s,\"d24s8Sampled\":%s,"
            "\"d24s8DepthStencilAttachment\":%s},\n"
            "    \"compatibility\": {\"bc\":\"%s\","
            "\"dualSrcBlend\":\"%s\",\"multiViewport\":\"%s\","
            "\"geometryStreams\":\"%s\"},\n"
            "    \"eligibility\": {\"transport\":\"%s\","
            "\"bringup\":\"%s\"}\n"
            "  },\n"
            "  \"vkd3dCapability\": {\"scope\":\"windows-pe-vulkan-path\","
            "\"vkd3dLoaded\":false,\"upstreamViewDescriptorMinimum\":1000000,"
            "\"experimentalViewDescriptorMinimum\":500000,\"samplerDescriptorMinimum\":2048,"
            "\"inputAttachmentsAreGating\":false,"
            "\"deviceCreation\":{\"attempted\":%s,\"result\":%d,\"passed\":%s},"
            "\"profiles\":{"
            "\"vkd3dProton26\":{\"minimumVulkanApi\":\"1.1\",\"supportedOnThisPath\":%s},"
            "\"vkd3dProton28\":{\"minimumVulkanApi\":\"1.1\",\"supportedOnThisPath\":%s},"
            "\"vkd3dProton29Modern\":{\"minimumVulkanApi\":\"1.3\",\"supportedOnThisPath\":%s},"
            "\"experimentalLimited500k\":{\"minimumVulkanApi\":\"1.1\","
            "\"defaultEnabled\":false,\"candidateOnThisPath\":%s}}},\n"
            "  \"capabilityAudit\":%s,\n"
            "  \"metrics\": {\"cpuReadBytes\":0,\"cpuUploadBytes\":0,"
            "\"gpuCopyCount\":0,\"queueSubmitCount\":0,"
            "\"fallbackDetected\":%s,\"durationMs\":%llu}\n"
            "}\n",
            state->run_id, state->test_id, status, safe_message,
            (unsigned long)GetCurrentProcessId(),
#ifdef _WIN64
            "x86_64", "x86_64",
#else
            "x86", "x86_64",
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
            loader_version, device_version, device_name, state->properties.vendorID,
            state->properties.deviceID, state->properties.driverVersion, state->queue_family,
            state->properties.limits.maxPushConstantsSize,
            bool_text(state->api13), bool_text(state->core.robustBufferAccess),
            bool_text(state->robust_buffer_access2), bool_text(state->robust_image_access2),
            bool_text(state->null_descriptor), bool_text(state->synchronization2),
            bool_text(state->dynamic_rendering), bool_text(state->maintenance4),
            bool_text(state->transport_features_ready), (int)state->transport_device_create_result,
            bool_text(state->transport_device_create_ok), bool_text(state->timeline_semaphore),
            (int)state->timeline_semaphore_create_result, (int)state->timeline_submit_result,
            (int)state->timeline_wait_result, (int)state->timeline_counter_result,
            (unsigned long long)state->timeline_observed_value,
            bool_text(state->timeline_round_trip_ok), bool_text(state->timeline_semaphore),
            bool_text(state->buffer_device_address), bool_text(state->descriptor_indexing),
            state->max_update_after_bind_descriptors_in_all_pools,
            state->max_descriptor_set_update_after_bind_sampled_images,
            state->max_descriptor_set_update_after_bind_storage_images,
            state->max_descriptor_set_update_after_bind_storage_buffers,
            bool_text(state->core.geometryShader), bool_text(state->core.tessellationShader),
            bool_text(state->core.multiDrawIndirect),
            policy_text(state->dual_src_blend, "emulated"),
            policy_text(state->multi_viewport, "withheld"),
            policy_text(state->transform_feedback, "native-unavailable"),
            policy_text(state->geometry_streams, "withheld"),
            bool_text(state->bc1), bool_text(state->bc2), bool_text(state->bc3),
            bool_text(state->bc4), bool_text(state->bc5), bool_text(state->bc6),
            bool_text(state->bc7), bool_text(state->rgba8_snorm_color_attachment),
            bool_text(state->d24s8_sampled), bool_text(state->d24s8_depth_stencil_attachment),
            state->texture_compression_bc ? "native" : "decode-required",
            policy_text(state->dual_src_blend, "fallback-required"),
            policy_text(state->multi_viewport, "withhold-required"),
            policy_text(state->geometry_streams, "withhold-required"),
            state->transport_device_create_ok && state->timeline_round_trip_ok ? "PASS" : "FAIL",
            state->transport_device_create_ok && state->timeline_round_trip_ok
                ? "D3D11_FEATURE_PROBE_PENDING" : "BLOCKED",
            bool_text(state->vkd3d_bindless_device_create_attempted),
            (int)state->vkd3d_bindless_device_create_result,
            bool_text(state->vkd3d_bindless_device_create_ok),
            bool_text(vkd3d26_supported(state)), bool_text(vkd3d28_supported(state)),
            bool_text(vkd3d29_supported(state)), bool_text(vkd3d26_limited_500k_candidate(state)),
            state->capability_audit ? state->capability_audit : "{}",
            bool_text(state->fallback_detected),
            (unsigned long long)(now_ms() - state->started_ms));
    fflush(file);
    fclose(file);
    MoveFileExA(temporary, state->result_path,
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}

static void publish_result(const struct probe_state *state, const char *status,
                           const char *message)
{
    write_result(state, status, message);
    if (state->interactive) show_capability_report(state, status, message);
}

static BOOL has_extension(const VkExtensionProperties *extensions, uint32_t count,
                          const char *name)
{
    uint32_t i;
    for (i = 0; i < count; ++i)
        if (!strcmp(extensions[i].extensionName, name)) return TRUE;
    return FALSE;
}

static BOOL format_supports(VkPhysicalDevice physical, VkFormat format,
                            VkFormatFeatureFlags features)
{
    VkFormatProperties properties;
    vkGetPhysicalDeviceFormatProperties(physical, format, &properties);
    return (properties.optimalTilingFeatures & features) == features;
}

static BOOL query_requirements(VkPhysicalDevice physical, struct probe_state *state)
{
    uint32_t count = 0;
    VkExtensionProperties *extensions = NULL;
    VkPhysicalDeviceFeatures2 features2 = { 0 };
    VkPhysicalDeviceVulkan11Features vk11 = { 0 };
    VkPhysicalDeviceVulkan12Features vk12 = { 0 };
    VkPhysicalDeviceVulkan13Features vk13 = { 0 };
    VkPhysicalDeviceDescriptorIndexingFeatures descriptor_indexing = { 0 };
    VkPhysicalDeviceTimelineSemaphoreFeatures timeline_semaphore = { 0 };
    VkPhysicalDeviceBufferDeviceAddressFeatures buffer_device_address = { 0 };
    VkPhysicalDeviceSeparateDepthStencilLayoutsFeatures separate_depth_stencil = { 0 };
    VkPhysicalDeviceDynamicRenderingFeatures dynamic_rendering = { 0 };
    VkPhysicalDeviceRobustness2FeaturesEXT robustness2 = { 0 };
    VkPhysicalDeviceExtendedDynamicStateFeaturesEXT extended_dynamic_state = { 0 };
    VkPhysicalDeviceExtendedDynamicState2FeaturesEXT extended_dynamic_state2 = { 0 };
    VkPhysicalDeviceTransformFeedbackFeaturesEXT transform_feedback = { 0 };
    VkPhysicalDeviceProperties2 properties2 = { 0 };
    VkPhysicalDeviceVulkan12Properties properties12 = { 0 };
    VkPhysicalDeviceDescriptorIndexingProperties descriptor_properties = { 0 };
    VkPhysicalDeviceIDProperties id_properties = { 0 };
    void **tail = &features2.pNext;
    BOOL has_robustness2;
    BOOL has_transform_feedback;

    features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    vk11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
    vk12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    vk13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    descriptor_indexing.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES;
    timeline_semaphore.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES;
    buffer_device_address.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES;
    separate_depth_stencil.sType =
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SEPARATE_DEPTH_STENCIL_LAYOUTS_FEATURES;
    dynamic_rendering.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES;
    robustness2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT;
    extended_dynamic_state.sType =
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT;
    extended_dynamic_state2.sType =
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_2_FEATURES_EXT;
    transform_feedback.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT;
    properties2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    properties12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES;
    descriptor_properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_PROPERTIES;
    id_properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;

    if (vkEnumerateDeviceExtensionProperties(physical, NULL, &count, NULL) != VK_SUCCESS)
        return FALSE;
    extensions = calloc(count ? count : 1, sizeof(*extensions));
    if (!extensions) return FALSE;
    if (count && vkEnumerateDeviceExtensionProperties(physical, NULL, &count, extensions) != VK_SUCCESS) {
        free(extensions);
        return FALSE;
    }
    has_robustness2 = has_extension(extensions, count, VK_EXT_ROBUSTNESS_2_EXTENSION_NAME);
    has_transform_feedback = has_extension(extensions, count,
                                            VK_EXT_TRANSFORM_FEEDBACK_EXTENSION_NAME);
    state->extension_descriptor_indexing = state->api12 ||
        has_extension(extensions, count, VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME);
    state->extension_timeline_semaphore = state->api12 ||
        has_extension(extensions, count, VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME);
    state->extension_sampler_mirror_clamp_to_edge = state->api12 ||
        has_extension(extensions, count, VK_KHR_SAMPLER_MIRROR_CLAMP_TO_EDGE_EXTENSION_NAME);
    state->extension_create_renderpass2 = state->api12 ||
        has_extension(extensions, count, VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME);
    state->extension_separate_depth_stencil_layouts = state->api12 ||
        has_extension(extensions, count, VK_KHR_SEPARATE_DEPTH_STENCIL_LAYOUTS_EXTENSION_NAME);
    state->extension_bind_memory2 = state->api11 ||
        has_extension(extensions, count, VK_KHR_BIND_MEMORY_2_EXTENSION_NAME);
    state->extension_copy_commands2 = state->api13 ||
        has_extension(extensions, count, VK_KHR_COPY_COMMANDS_2_EXTENSION_NAME);
    state->extension_dynamic_rendering = state->api13 ||
        has_extension(extensions, count, VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME);
    state->extension_buffer_device_address = state->api12 ||
        has_extension(extensions, count, VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME);
    state->extension_push_descriptor =
        has_extension(extensions, count, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME);
    state->extension_extended_dynamic_state =
        has_extension(extensions, count, VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME);
    state->extension_extended_dynamic_state2 =
        has_extension(extensions, count, VK_EXT_EXTENDED_DYNAMIC_STATE_2_EXTENSION_NAME);
    state->extension_image_view_min_lod =
        has_extension(extensions, count, VK_EXT_IMAGE_VIEW_MIN_LOD_EXTENSION_NAME);
    state->extension_mutable_descriptor_type =
        has_extension(extensions, count, "VK_EXT_mutable_descriptor_type") ||
        has_extension(extensions, count, "VK_VALVE_mutable_descriptor_type");
#define APPEND_FEATURE(feature, enabled) do { \
    if (enabled) { *tail = &(feature); tail = &(feature).pNext; } \
} while (0)
    APPEND_FEATURE(vk11, state->api11);
    APPEND_FEATURE(vk12, state->api12);
    APPEND_FEATURE(vk13, state->api13);
    APPEND_FEATURE(descriptor_indexing, !state->api12 && state->extension_descriptor_indexing);
    APPEND_FEATURE(timeline_semaphore, !state->api12 && state->extension_timeline_semaphore);
    APPEND_FEATURE(buffer_device_address, !state->api12 && state->extension_buffer_device_address);
    APPEND_FEATURE(separate_depth_stencil,
                   !state->api12 && state->extension_separate_depth_stencil_layouts);
    APPEND_FEATURE(dynamic_rendering, !state->api13 && state->extension_dynamic_rendering);
    APPEND_FEATURE(robustness2, has_robustness2);
    APPEND_FEATURE(extended_dynamic_state, state->extension_extended_dynamic_state);
    APPEND_FEATURE(extended_dynamic_state2, state->extension_extended_dynamic_state2);
    APPEND_FEATURE(transform_feedback, has_transform_feedback);
#undef APPEND_FEATURE
    vkGetPhysicalDeviceFeatures2(physical, &features2);
    if (state->api12) {
        properties2.pNext = &properties12;
        properties12.pNext = &id_properties;
    } else if (state->extension_descriptor_indexing) {
        properties2.pNext = &descriptor_properties;
        descriptor_properties.pNext = &id_properties;
    } else {
        properties2.pNext = &id_properties;
    }
    vkGetPhysicalDeviceProperties2(physical, &properties2);

    state->vkd3d_features11 = vk11;
    state->vkd3d_features12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    if (state->api12) {
        state->vkd3d_features12 = vk12;
        state->vkd3d_properties12 = properties12;
    } else {
        VkPhysicalDeviceVulkan12Features *features = &state->vkd3d_features12;
        VkPhysicalDeviceVulkan12Properties *properties = &state->vkd3d_properties12;

        features->descriptorIndexing = state->extension_descriptor_indexing;
        features->shaderInputAttachmentArrayDynamicIndexing =
            descriptor_indexing.shaderInputAttachmentArrayDynamicIndexing;
        features->shaderUniformTexelBufferArrayDynamicIndexing =
            descriptor_indexing.shaderUniformTexelBufferArrayDynamicIndexing;
        features->shaderStorageTexelBufferArrayDynamicIndexing =
            descriptor_indexing.shaderStorageTexelBufferArrayDynamicIndexing;
        features->shaderUniformBufferArrayNonUniformIndexing =
            descriptor_indexing.shaderUniformBufferArrayNonUniformIndexing;
        features->shaderSampledImageArrayNonUniformIndexing =
            descriptor_indexing.shaderSampledImageArrayNonUniformIndexing;
        features->shaderStorageBufferArrayNonUniformIndexing =
            descriptor_indexing.shaderStorageBufferArrayNonUniformIndexing;
        features->shaderStorageImageArrayNonUniformIndexing =
            descriptor_indexing.shaderStorageImageArrayNonUniformIndexing;
        features->shaderInputAttachmentArrayNonUniformIndexing =
            descriptor_indexing.shaderInputAttachmentArrayNonUniformIndexing;
        features->shaderUniformTexelBufferArrayNonUniformIndexing =
            descriptor_indexing.shaderUniformTexelBufferArrayNonUniformIndexing;
        features->shaderStorageTexelBufferArrayNonUniformIndexing =
            descriptor_indexing.shaderStorageTexelBufferArrayNonUniformIndexing;
        features->descriptorBindingUniformBufferUpdateAfterBind =
            descriptor_indexing.descriptorBindingUniformBufferUpdateAfterBind;
        features->descriptorBindingSampledImageUpdateAfterBind =
            descriptor_indexing.descriptorBindingSampledImageUpdateAfterBind;
        features->descriptorBindingStorageImageUpdateAfterBind =
            descriptor_indexing.descriptorBindingStorageImageUpdateAfterBind;
        features->descriptorBindingStorageBufferUpdateAfterBind =
            descriptor_indexing.descriptorBindingStorageBufferUpdateAfterBind;
        features->descriptorBindingUniformTexelBufferUpdateAfterBind =
            descriptor_indexing.descriptorBindingUniformTexelBufferUpdateAfterBind;
        features->descriptorBindingStorageTexelBufferUpdateAfterBind =
            descriptor_indexing.descriptorBindingStorageTexelBufferUpdateAfterBind;
        features->descriptorBindingUpdateUnusedWhilePending =
            descriptor_indexing.descriptorBindingUpdateUnusedWhilePending;
        features->descriptorBindingPartiallyBound = descriptor_indexing.descriptorBindingPartiallyBound;
        features->descriptorBindingVariableDescriptorCount =
            descriptor_indexing.descriptorBindingVariableDescriptorCount;
        features->runtimeDescriptorArray = descriptor_indexing.runtimeDescriptorArray;
        features->samplerMirrorClampToEdge = state->extension_sampler_mirror_clamp_to_edge;
        features->timelineSemaphore = timeline_semaphore.timelineSemaphore;
        features->bufferDeviceAddress = buffer_device_address.bufferDeviceAddress;
        features->bufferDeviceAddressCaptureReplay =
            buffer_device_address.bufferDeviceAddressCaptureReplay;
        features->bufferDeviceAddressMultiDevice =
            buffer_device_address.bufferDeviceAddressMultiDevice;
        features->separateDepthStencilLayouts = separate_depth_stencil.separateDepthStencilLayouts;
        properties->sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES;
        properties->maxUpdateAfterBindDescriptorsInAllPools =
            descriptor_properties.maxUpdateAfterBindDescriptorsInAllPools;
        properties->maxPerStageDescriptorUpdateAfterBindSamplers =
            descriptor_properties.maxPerStageDescriptorUpdateAfterBindSamplers;
        properties->maxPerStageDescriptorUpdateAfterBindUniformBuffers =
            descriptor_properties.maxPerStageDescriptorUpdateAfterBindUniformBuffers;
        properties->maxPerStageDescriptorUpdateAfterBindStorageBuffers =
            descriptor_properties.maxPerStageDescriptorUpdateAfterBindStorageBuffers;
        properties->maxPerStageDescriptorUpdateAfterBindSampledImages =
            descriptor_properties.maxPerStageDescriptorUpdateAfterBindSampledImages;
        properties->maxPerStageDescriptorUpdateAfterBindStorageImages =
            descriptor_properties.maxPerStageDescriptorUpdateAfterBindStorageImages;
        properties->maxPerStageDescriptorUpdateAfterBindInputAttachments =
            descriptor_properties.maxPerStageDescriptorUpdateAfterBindInputAttachments;
        properties->maxPerStageUpdateAfterBindResources =
            descriptor_properties.maxPerStageUpdateAfterBindResources;
        properties->maxDescriptorSetUpdateAfterBindSamplers =
            descriptor_properties.maxDescriptorSetUpdateAfterBindSamplers;
        properties->maxDescriptorSetUpdateAfterBindUniformBuffers =
            descriptor_properties.maxDescriptorSetUpdateAfterBindUniformBuffers;
        properties->maxDescriptorSetUpdateAfterBindUniformBuffersDynamic =
            descriptor_properties.maxDescriptorSetUpdateAfterBindUniformBuffersDynamic;
        properties->maxDescriptorSetUpdateAfterBindStorageBuffers =
            descriptor_properties.maxDescriptorSetUpdateAfterBindStorageBuffers;
        properties->maxDescriptorSetUpdateAfterBindStorageBuffersDynamic =
            descriptor_properties.maxDescriptorSetUpdateAfterBindStorageBuffersDynamic;
        properties->maxDescriptorSetUpdateAfterBindSampledImages =
            descriptor_properties.maxDescriptorSetUpdateAfterBindSampledImages;
        properties->maxDescriptorSetUpdateAfterBindStorageImages =
            descriptor_properties.maxDescriptorSetUpdateAfterBindStorageImages;
        properties->maxDescriptorSetUpdateAfterBindInputAttachments =
            descriptor_properties.maxDescriptorSetUpdateAfterBindInputAttachments;
        properties->shaderUniformBufferArrayNonUniformIndexingNative =
            descriptor_properties.shaderUniformBufferArrayNonUniformIndexingNative;
        properties->shaderSampledImageArrayNonUniformIndexingNative =
            descriptor_properties.shaderSampledImageArrayNonUniformIndexingNative;
        properties->shaderStorageBufferArrayNonUniformIndexingNative =
            descriptor_properties.shaderStorageBufferArrayNonUniformIndexingNative;
        properties->shaderStorageImageArrayNonUniformIndexingNative =
            descriptor_properties.shaderStorageImageArrayNonUniformIndexingNative;
        properties->shaderInputAttachmentArrayNonUniformIndexingNative =
            descriptor_properties.shaderInputAttachmentArrayNonUniformIndexingNative;
        properties->robustBufferAccessUpdateAfterBind =
            descriptor_properties.robustBufferAccessUpdateAfterBind;
        properties->quadDivergentImplicitLod = descriptor_properties.quadDivergentImplicitLod;
    }
    state->vkd3d_features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    if (state->api13) {
        state->vkd3d_features13 = vk13;
    } else {
        state->vkd3d_features13.dynamicRendering = dynamic_rendering.dynamicRendering;
    }

    state->robust_buffer_access2 = has_robustness2 && robustness2.robustBufferAccess2;
    state->robust_image_access2 = has_robustness2 && robustness2.robustImageAccess2;
    state->null_descriptor = has_robustness2 && robustness2.nullDescriptor;
    state->synchronization2 = state->api13 && state->vkd3d_features13.synchronization2;
    state->dynamic_rendering = state->extension_dynamic_rendering &&
        state->vkd3d_features13.dynamicRendering;
    state->maintenance4 = state->api13 && state->vkd3d_features13.maintenance4;
    state->timeline_semaphore = state->extension_timeline_semaphore &&
        state->vkd3d_features12.timelineSemaphore;
    state->buffer_device_address = state->extension_buffer_device_address &&
        state->vkd3d_features12.bufferDeviceAddress;
    state->descriptor_indexing = state->extension_descriptor_indexing &&
        state->vkd3d_features12.descriptorIndexing;
    state->sampler_mirror_clamp_to_edge = state->extension_sampler_mirror_clamp_to_edge &&
        (!state->api12 || state->vkd3d_features12.samplerMirrorClampToEdge);
    state->shader_draw_parameters = state->api11 && state->vkd3d_features11.shaderDrawParameters;
    state->create_renderpass2 = state->extension_create_renderpass2;
    state->separate_depth_stencil_layouts = state->extension_separate_depth_stencil_layouts &&
        (!state->api12 || state->vkd3d_features12.separateDepthStencilLayouts);
    state->bind_memory2 = state->extension_bind_memory2;
    state->copy_commands2 = state->extension_copy_commands2;
    state->push_descriptor = state->extension_push_descriptor;
    state->extended_dynamic_state = state->extension_extended_dynamic_state &&
        extended_dynamic_state.extendedDynamicState;
    state->extended_dynamic_state2 = state->extension_extended_dynamic_state2 &&
        extended_dynamic_state2.extendedDynamicState2;
    state->max_update_after_bind_descriptors_in_all_pools =
        state->vkd3d_properties12.maxUpdateAfterBindDescriptorsInAllPools;
    state->max_descriptor_set_update_after_bind_sampled_images =
        state->vkd3d_properties12.maxDescriptorSetUpdateAfterBindSampledImages;
    state->max_descriptor_set_update_after_bind_storage_images =
        state->vkd3d_properties12.maxDescriptorSetUpdateAfterBindStorageImages;
    state->max_descriptor_set_update_after_bind_storage_buffers =
        state->vkd3d_properties12.maxDescriptorSetUpdateAfterBindStorageBuffers;
    state->transform_feedback = has_transform_feedback && transform_feedback.transformFeedback;
    state->geometry_streams = has_transform_feedback && transform_feedback.geometryStreams;
    state->dual_src_blend = state->core.dualSrcBlend;
    state->multi_viewport = state->core.multiViewport;
    state->texture_compression_bc = state->core.textureCompressionBC;
    state->bc1 = format_supports(physical, VK_FORMAT_BC1_RGBA_UNORM_BLOCK,
                                 VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT);
    state->bc2 = format_supports(physical, VK_FORMAT_BC2_UNORM_BLOCK,
                                 VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT);
    state->bc3 = format_supports(physical, VK_FORMAT_BC3_UNORM_BLOCK,
                                 VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT);
    state->bc4 = format_supports(physical, VK_FORMAT_BC4_UNORM_BLOCK,
                                 VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT);
    state->bc5 = format_supports(physical, VK_FORMAT_BC5_UNORM_BLOCK,
                                 VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT);
    state->bc6 = format_supports(physical, VK_FORMAT_BC6H_UFLOAT_BLOCK,
                                 VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT);
    state->bc7 = format_supports(physical, VK_FORMAT_BC7_UNORM_BLOCK,
                                 VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT);
    state->rgba8_snorm_color_attachment = format_supports(
        physical, VK_FORMAT_R8G8B8A8_SNORM,
        VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT);
    state->d24s8_sampled = format_supports(physical, VK_FORMAT_D24_UNORM_S8_UINT,
                                            VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT);
    state->d24s8_depth_stencil_attachment = format_supports(
        physical, VK_FORMAT_D24_UNORM_S8_UINT,
        VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT);
    state->transport_features_ready = state->api13 && state->core.robustBufferAccess &&
        state->robust_buffer_access2 && state->robust_image_access2 &&
        state->null_descriptor && state->synchronization2 && state->dynamic_rendering &&
        state->maintenance4 && state->timeline_semaphore;
    state->capability_audit = winehua_vkd3d_capability_audit(
        physical, extensions, count, &state->vkd3d_features11, &state->vkd3d_features12,
        &state->vkd3d_features13, &state->vkd3d_properties12, &id_properties);
    if (!state->capability_audit) {
        free(extensions);
        return FALSE;
    }
    free(extensions);
    return TRUE;
}

static VkResult create_transport_device(VkPhysicalDevice physical,
                                        struct probe_state *state)
{
    const char *extensions[] = { VK_EXT_ROBUSTNESS_2_EXTENSION_NAME };
    float priority = 1.0f;
    VkPhysicalDeviceVulkan12Features vk12 = { 0 };
    VkPhysicalDeviceVulkan13Features vk13 = { 0 };
    VkPhysicalDeviceRobustness2FeaturesEXT robustness2 = { 0 };
    VkDeviceQueueCreateInfo queue = { 0 };
    VkDeviceCreateInfo create = { 0 };
    VkDevice device = VK_NULL_HANDLE;
    VkQueue device_queue = VK_NULL_HANDLE;
    VkSemaphoreTypeCreateInfo semaphore_type = { 0 };
    VkSemaphoreCreateInfo semaphore_create = { 0 };
    VkSemaphore semaphore = VK_NULL_HANDLE;
    VkCommandPoolCreateInfo pool_create = { 0 };
    VkCommandPool command_pool = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo command_alloc = { 0 };
    VkCommandBuffer command_buffer = VK_NULL_HANDLE;
    VkCommandBufferBeginInfo command_begin = { 0 };
    VkCommandBufferSubmitInfo command_info = { 0 };
    VkSemaphoreSubmitInfo signal = { 0 };
    VkSubmitInfo2 submit = { 0 };
    VkSemaphoreWaitInfo wait = { 0 };
    uint64_t signal_value = 1;
    VkResult result;

    if (!state->transport_features_ready) return VK_ERROR_FEATURE_NOT_PRESENT;
    vk12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    vk13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    robustness2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT;
    queue.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    create.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    vk12.timelineSemaphore = VK_TRUE;
    vk13.synchronization2 = VK_TRUE;
    vk13.dynamicRendering = VK_TRUE;
    vk13.maintenance4 = VK_TRUE;
    robustness2.robustBufferAccess2 = VK_TRUE;
    robustness2.nullDescriptor = VK_TRUE;
    robustness2.robustImageAccess2 = VK_TRUE;
    vk13.pNext = &vk12;
    vk12.pNext = &robustness2;
    queue.queueFamilyIndex = state->queue_family;
    queue.queueCount = 1;
    queue.pQueuePriorities = &priority;
    create.pNext = &vk13;
    create.queueCreateInfoCount = 1;
    create.pQueueCreateInfos = &queue;
    create.enabledExtensionCount = 1;
    create.ppEnabledExtensionNames = extensions;
    write_progress(state, "before-device-create");
    result = vkCreateDevice(physical, &create, NULL, &device);
    state->transport_device_create_result = result;
    if (result != VK_SUCCESS)
        return result;
    write_progress(state, "device-created");

    vkGetDeviceQueue(device, state->queue_family, 0, &device_queue);
    semaphore_type.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
    semaphore_type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    semaphore_create.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    semaphore_create.pNext = &semaphore_type;
    write_progress(state, "before-semaphore-create");
    state->timeline_semaphore_create_result =
        vkCreateSemaphore(device, &semaphore_create, NULL, &semaphore);
    if (state->timeline_semaphore_create_result != VK_SUCCESS) {
        result = state->timeline_semaphore_create_result;
        goto cleanup;
    }
    write_progress(state, "semaphore-created");

    /* Keep one real command in the submission.  DXVK never relies on an empty
     * queue submit, and some Venus implementations do not retire an empty
     * Submit2 timeline signal through the same feedback path. */
    pool_create.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_create.queueFamilyIndex = state->queue_family;
    result = vkCreateCommandPool(device, &pool_create, NULL, &command_pool);
    if (result != VK_SUCCESS)
        goto cleanup;
    command_alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    command_alloc.commandPool = command_pool;
    command_alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    command_alloc.commandBufferCount = 1;
    result = vkAllocateCommandBuffers(device, &command_alloc, &command_buffer);
    if (result != VK_SUCCESS)
        goto cleanup;
    command_begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    result = vkBeginCommandBuffer(command_buffer, &command_begin);
    if (result != VK_SUCCESS)
        goto cleanup;
    result = vkEndCommandBuffer(command_buffer);
    if (result != VK_SUCCESS)
        goto cleanup;

    /* DXVK 2.6 submits through Vulkan 1.3 synchronization2.  Qualify the
     * exact QueueSubmit2 timeline path instead of the legacy submit pNext
     * encoding, which DXVK does not use. */
    signal.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    signal.semaphore = semaphore;
    signal.value = signal_value;
    signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    command_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
    command_info.commandBuffer = command_buffer;
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos = &command_info;
    submit.signalSemaphoreInfoCount = 1;
    submit.pSignalSemaphoreInfos = &signal;
    write_progress(state, "before-queue-submit");
    state->timeline_submit_result = vkQueueSubmit2(device_queue, 1, &submit, VK_NULL_HANDLE);
    if (state->timeline_submit_result != VK_SUCCESS) {
        result = state->timeline_submit_result;
        goto cleanup;
    }
    write_progress(state, "queue-submitted");

    wait.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
    wait.semaphoreCount = 1;
    wait.pSemaphores = &semaphore;
    wait.pValues = &signal_value;
    write_progress(state, "before-semaphore-wait");
    state->timeline_wait_result = vkWaitSemaphores(device, &wait, 5000000000ULL);
    if (state->timeline_wait_result != VK_SUCCESS) {
        result = state->timeline_wait_result;
        goto cleanup;
    }
    write_progress(state, "semaphore-wait-complete");

    write_progress(state, "before-counter-query");
    state->timeline_counter_result =
        vkGetSemaphoreCounterValue(device, semaphore, &state->timeline_observed_value);
    state->timeline_round_trip_ok =
        state->timeline_counter_result == VK_SUCCESS &&
        state->timeline_observed_value >= signal_value;
    result = state->timeline_round_trip_ok ? VK_SUCCESS : VK_NOT_READY;

cleanup:
    if (command_pool != VK_NULL_HANDLE)
        vkDestroyCommandPool(device, command_pool, NULL);
    if (semaphore != VK_NULL_HANDLE)
        vkDestroySemaphore(device, semaphore, NULL);
    if (device != VK_NULL_HANDLE)
        vkDestroyDevice(device, NULL);
    write_progress(state, "device-destroyed");
    return result;
}

static VkResult create_vkd3d26_bindless_device(VkPhysicalDevice physical,
                                                const struct probe_state *state)
{
    const char *extensions[] = {
        VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME,
        VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME,
        VK_EXT_ROBUSTNESS_2_EXTENSION_NAME,
    };
    float priority = 1.0f;
    VkPhysicalDeviceDescriptorIndexingFeatures descriptor_indexing = { 0 };
    VkPhysicalDeviceTimelineSemaphoreFeatures timeline_semaphore = { 0 };
    VkPhysicalDeviceRobustness2FeaturesEXT robustness2 = { 0 };
    VkDeviceQueueCreateInfo queue = { 0 };
    VkDeviceCreateInfo create = { 0 };
    VkDevice device = VK_NULL_HANDLE;
    VkResult result;

    if (!state->api11 || !vkd3d_bindless_features_complete(state) ||
        !state->timeline_semaphore || !state->robust_buffer_access2 ||
        !state->robust_image_access2 || !state->null_descriptor ||
        state->queue_family == UINT32_MAX)
        return VK_ERROR_FEATURE_NOT_PRESENT;

    descriptor_indexing.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES;
    timeline_semaphore.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES;
    robustness2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT;
    queue.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    create.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    descriptor_indexing.shaderUniformBufferArrayNonUniformIndexing = VK_TRUE;
    descriptor_indexing.shaderUniformTexelBufferArrayDynamicIndexing = VK_TRUE;
    descriptor_indexing.shaderStorageTexelBufferArrayDynamicIndexing = VK_TRUE;
    descriptor_indexing.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
    descriptor_indexing.shaderStorageBufferArrayNonUniformIndexing = VK_TRUE;
    descriptor_indexing.shaderStorageImageArrayNonUniformIndexing = VK_TRUE;
    descriptor_indexing.shaderUniformTexelBufferArrayNonUniformIndexing = VK_TRUE;
    descriptor_indexing.shaderStorageTexelBufferArrayNonUniformIndexing = VK_TRUE;
    descriptor_indexing.descriptorBindingUniformBufferUpdateAfterBind = VK_TRUE;
    descriptor_indexing.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
    descriptor_indexing.descriptorBindingStorageImageUpdateAfterBind = VK_TRUE;
    descriptor_indexing.descriptorBindingStorageBufferUpdateAfterBind = VK_TRUE;
    descriptor_indexing.descriptorBindingUniformTexelBufferUpdateAfterBind = VK_TRUE;
    descriptor_indexing.descriptorBindingStorageTexelBufferUpdateAfterBind = VK_TRUE;
    descriptor_indexing.descriptorBindingUpdateUnusedWhilePending = VK_TRUE;
    descriptor_indexing.descriptorBindingPartiallyBound = VK_TRUE;
    descriptor_indexing.descriptorBindingVariableDescriptorCount = VK_TRUE;
    descriptor_indexing.runtimeDescriptorArray = VK_TRUE;
    timeline_semaphore.timelineSemaphore = VK_TRUE;
    robustness2.robustBufferAccess2 = VK_TRUE;
    robustness2.robustImageAccess2 = VK_TRUE;
    robustness2.nullDescriptor = VK_TRUE;
    descriptor_indexing.pNext = &timeline_semaphore;
    timeline_semaphore.pNext = &robustness2;

    queue.queueFamilyIndex = state->queue_family;
    queue.queueCount = 1;
    queue.pQueuePriorities = &priority;
    create.pNext = &descriptor_indexing;
    create.queueCreateInfoCount = 1;
    create.pQueueCreateInfos = &queue;
    create.enabledExtensionCount = 3;
    create.ppEnabledExtensionNames = extensions;
    result = vkCreateDevice(physical, &create, NULL, &device);
    if (result == VK_SUCCESS)
        vkDestroyDevice(device, NULL);
    return result;
}

static BOOL select_graphics_queue(VkPhysicalDevice physical, struct probe_state *state)
{
    VkQueueFamilyProperties *queues;
    uint32_t count = 0;
    uint32_t i;

    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, NULL);
    queues = calloc(count ? count : 1, sizeof(*queues));
    if (!queues) return FALSE;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, queues);
    for (i = 0; i < count; ++i) {
        if (queues[i].queueCount && (queues[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
            state->queue_family = i;
            break;
        }
    }
    free(queues);
    return state->queue_family != UINT32_MAX;
}

int main(int argc, char **argv)
{
    struct probe_state state;
    VkApplicationInfo application = { 0 };
    VkInstanceCreateInfo instance_info = { 0 };
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    uint32_t count = 0;
    VkResult result;
    const char *failure = "unknown failure";
    int exit_code = 1;

    if (argument_present(argc, argv, "--help") || argument_present(argc, argv, "-h")) {
        printf("Usage: winehua_dxvk26_requirements.exe [--vkd3d-capability] [--result FILE]\n"
               "Run without --automation to show DXVK and VKD3D capability tabs.\n"
               "--vkd3d-capability opens the VKD3D tab first.\n");
        return 0;
    }
    memset(&state, 0, sizeof(state));
    state.run_id = argument_value(argc, argv, "--run-id", "manual");
    state.test_id = argument_value(argc, argv, "--test-id", "dxvk26-requirements");
    state.result_path = argument_value(argc, argv, "--result", "");
    state.started_ms = now_ms();
    state.queue_family = UINT32_MAX;
    state.loader_api = VK_API_VERSION_1_0;
    state.transport_device_create_result = VK_NOT_READY;
    state.vkd3d_bindless_device_create_result = VK_NOT_READY;
    state.timeline_semaphore_create_result = VK_NOT_READY;
    state.timeline_submit_result = VK_NOT_READY;
    state.timeline_wait_result = VK_NOT_READY;
    state.timeline_counter_result = VK_NOT_READY;
    state.interactive = !argument_present(argc, argv, "--automation");
    state.initial_vkd3d_tab = argument_present(argc, argv, "--vkd3d-capability");
    {
        PFN_vkEnumerateInstanceVersion enumerate_version =
            (PFN_vkEnumerateInstanceVersion)vkGetInstanceProcAddr(VK_NULL_HANDLE,
                                                                    "vkEnumerateInstanceVersion");
        if (enumerate_version) enumerate_version(&state.loader_api);
    }
    application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    application.pApplicationName = "winehua_dxvk26_requirements";
    application.applicationVersion = 1;
    application.pEngineName = "WineHua";
    application.engineVersion = 1;
    /* A capability probe must be able to inspect the 1.1-based 2.6/2.8
     * profiles. Requesting 1.3 here would hide those devices entirely. */
    application.apiVersion = VK_API_VERSION_1_1;
    instance_info.pApplicationInfo = &application;
    result = vkCreateInstance(&instance_info, NULL, &instance);
    if (result != VK_SUCCESS) {
        publish_result(&state, "UNSUPPORTED", "vkCreateInstance Vulkan 1.1 failed");
        return 3;
    }
    result = vkEnumeratePhysicalDevices(instance, &count, NULL);
    if (result != VK_SUCCESS || !count) { failure = "no Vulkan physical device"; goto cleanup; }
    {
        VkPhysicalDevice *devices = calloc(count, sizeof(*devices));
        if (!devices) { failure = "physical device allocation failed"; goto cleanup; }
        result = vkEnumeratePhysicalDevices(instance, &count, devices);
        if (result == VK_SUCCESS) physical = devices[0];
        free(devices);
        if (result != VK_SUCCESS || !physical) { failure = "physical device enumeration failed"; goto cleanup; }
    }
    vkGetPhysicalDeviceProperties(physical, &state.properties);
    vkGetPhysicalDeviceFeatures(physical, &state.core);
    state.api11 = state.properties.apiVersion >= VK_API_VERSION_1_1;
    state.api12 = state.properties.apiVersion >= VK_API_VERSION_1_2;
    state.api13 = state.properties.apiVersion >= VK_API_VERSION_1_3;
    state.fallback_detected = strstr(state.properties.deviceName, "llvmpipe") != NULL ||
        strstr(state.properties.deviceName, "softpipe") != NULL;
    if (state.fallback_detected) { failure = "software Vulkan fallback detected"; goto cleanup; }
    if (!query_requirements(physical, &state)) { failure = "capability query failed"; goto cleanup; }
    if (!select_graphics_queue(physical, &state)) {
        failure = "no graphics queue family";
        goto cleanup;
    }
    if (state.initial_vkd3d_tab || state.interactive) {
        state.vkd3d_bindless_device_create_attempted = TRUE;
        state.vkd3d_bindless_device_create_result =
            create_vkd3d26_bindless_device(physical, &state);
        state.vkd3d_bindless_device_create_ok =
            state.vkd3d_bindless_device_create_result == VK_SUCCESS;
        if (!state.vkd3d_bindless_device_create_ok) {
            publish_result(&state, "UNSUPPORTED",
                           "VKD3D 2.6 bindless feature-chain vkCreateDevice failed");
            vkDestroyInstance(instance, NULL);
            free(state.capability_audit);
            return 3;
        }
        publish_result(&state, "PASS",
                       state.interactive ?
                           "Interactive DXVK and VKD3D capability evidence collected; "
                           "VKD3D 2.6 bindless device creation passed; no VKD3D runtime was loaded" :
                           "VKD3D 2.6 bindless device creation passed; no VKD3D runtime was loaded");
        exit_code = 0;
        goto cleanup;
    }
    result = create_transport_device(physical, &state);
    state.transport_device_create_ok = state.transport_device_create_result == VK_SUCCESS;
    if (!state.transport_device_create_ok) {
        publish_result(&state, "UNSUPPORTED", "DXVK 2.6 transport device requirements are unavailable");
        vkDestroyInstance(instance, NULL);
        free(state.capability_audit);
        return 3;
    }
    if (!state.timeline_round_trip_ok) {
        publish_result(&state, "FAIL", "DXVK 2.6 timeline queue-submit/wait round trip failed");
        vkDestroyInstance(instance, NULL);
        free(state.capability_audit);
        return 2;
    }
    publish_result(&state, "PASS",
                   "DXVK 2.6 transport requirements passed; no vkd3d runtime was loaded");
    exit_code = 0;

cleanup:
    if (exit_code) publish_result(&state, "FAIL", failure);
    if (instance) vkDestroyInstance(instance, NULL);
    free(state.capability_audit);
    return exit_code;
}
