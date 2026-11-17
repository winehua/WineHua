#include "bridge/system_open.h"
#include "bridge/runtime_compat.h"
#include "wine/prefix_reset.h"
#include <napi/native_api.h>
#include "common/fs_utils.h"
#include "compositor/wayland_server.h"
#include "compositor/toplevel/desktop_compositor.h"
#include "bridge/plugin_manager.h"
#include "input/input_manager.h"
#include "input/pointer_extras.h"
#include "graphics/egl_renderer.h"
#include "audio/audio_broker.h"
#include "audio_ipc_protocol.h"
#include "graphics/graphics_broker.h"
#include "wine/wine_constants.h"
#include "wine_scheme.h"
#include "wine/wine_env.h"
#include "wine/container_session.h"
#include "proc/wine_process.h"
#include "wine/wine_launch.h"
#include "wine/wine_cpu_policy.h"
#include "wine/wine_exe.h"
#include "phone_adapter/phone_adapter.h"
#include "input/text_input.h"
#include "display/display_input.h"
#include "display/display_session.h"
#include "display/x11_external_host.h"
#include "display/x11_toplevel.h"
#include "display/xim_bridge.h"
#include "input/game_controller_bridge.h"
#include "input/controller/controller_napi.h"
#include "direct/vulkan_probe_launcher.h"
#include "direct/direct_shared_buffer_probe.h"
#include "direct/direct_wine_ipc_probe.h"
#include "direct/direct_surface_probe_launcher.h"
#include "direct/direct_output_probe.h"
#include "direct/direct_wine_surface_controller.h"
#include "direct/direct_vulkan_desktop_compositor.h"

#include <unistd.h>
#include <signal.h>
#include <window_manager/oh_window.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <dirent.h>
#include <fcntl.h>
#include <cstdlib>
#include <cstdio>
#include <mutex>
#include <cerrno>
#include <cstring>
#include <string>
#include <thread>
#include <atomic>
#include <algorithm>
#include <vector>
#include <dlfcn.h>

#undef LOG_TAG
#undef LOG_DOMAIN
#define LOG_DOMAIN 0x0000
#define LOG_TAG "WL_NAPI"
#include <hilog/log.h>

// ---- VPP product-surface includes (proton baseline merge) ----
#include "common/fps_counter.h"
#include "common/perf_utils.h"
#include "common/frame_loop_diagnostics.h"
#include "common/displayed_fps.h"
#include "common/app_log.h"
#include "graphics/host_vulkan_probe.h"
#include "graphics/graphics_profile.h"


// -- 全局状态 (NAPI 层, 被 wine_process / wine_launch 引用) --
napi_threadsafe_function gStateTsfn = nullptr;
std::string gSockPath;

// -- State 回调 -> ArkTS --
static void CallJsState(napi_env env, napi_value cb, void*, void* data) {
    char* msg = static_cast<char*>(data);
    if (env && cb && msg) {
        napi_value undef, arg;
        napi_get_undefined(env, &undef);
        napi_create_string_utf8(env, msg, NAPI_AUTO_LENGTH, &arg);
        napi_call_function(env, undef, cb, 1, &arg, nullptr);
    }
    free(msg);
}

// -- NAPI: setStateCallback --
static napi_value SetStateCallback(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    if (gStateTsfn) {
        napi_release_threadsafe_function(gStateTsfn, napi_tsfn_release);
        gStateTsfn = nullptr;
    }

    napi_value name;
    napi_create_string_utf8(env, "WLState", NAPI_AUTO_LENGTH, &name);
    napi_create_threadsafe_function(env, args[0], nullptr, name,
                                     0, 1, nullptr, nullptr, nullptr, CallJsState, &gStateTsfn);

    WaylandServer::GetInstance()->SetStateCallback([](const char* s) {
        if (gStateTsfn) {
            napi_call_threadsafe_function(gStateTsfn, strdup(s), napi_tsfn_blocking);
        }
    });
    return nullptr;
}

// -- NAPI: startServer --
static napi_value StartServer(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    char path[512] = {};
    napi_get_value_string_utf8(env, args[0], path, sizeof(path), nullptr);

    OH_LOG_WARN(LOG_APP, "[NAPI] startServer: %{public}s", path);
    // 确保 socket 父目录存在 (WINEPREFIX=.wine/)
    {
        std::string sockDir = path;
        auto pos = sockDir.find_last_of('/');
        if (pos != std::string::npos) {
            sockDir = sockDir.substr(0, pos);
            mkdir(sockDir.c_str(), 0755);
        }
    }
    gSockPath = path;
    bool ok = WaylandServer::GetInstance()->Start(path);
    OH_LOG_WARN(LOG_APP, "[NAPI] startServer result: %{public}s", ok ? "OK" : "FAIL");
    // 确认 socket 文件存在
    if (ok) {
        struct stat st;
        int sr = stat(path, &st);
        OH_LOG_WARN(LOG_APP, "[NAPI] wayland socket stat=%{public}d (errno=%{public}d)",
                    sr, sr == 0 ? 0 : errno);
    }

    napi_value r;
    napi_get_boolean(env, ok, &r);
    return r;
}

static napi_value SetHostShadowProfile(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1] = {};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    char profile[64] = "baseline";
    if (argc >= 1)
        napi_get_value_string_utf8(env, args[0], profile, sizeof(profile), nullptr);

    const bool skip = !strcmp(profile, "shadow-none");
    const bool directFence = !strcmp(profile, "shadow-precise-direct-fence");
    const bool preciseStrongTrace =
        !strcmp(profile, "shadow-precise-strong-ring-trace");
    const bool preciseStrongPerf =
        !strcmp(profile, "shadow-precise-strong-ring-perf");
    /* Timeline feedback is independently disabled in the Guest.  Keep the
     * Host-side mapped-memory semantics identical to the other precise
     * transport profiles so the A/B changes only feedback and ring topology. */
    const bool preciseNoSemaphoreFeedbackSingleRing =
        !strcmp(profile, "shadow-precise-no-semaphore-feedback-single-ring") ||
        !strcmp(profile,
                "shadow-precise-no-semaphore-feedback-single-ring-sync-submit") ||
        !strcmp(profile,
                "shadow-precise-no-semaphore-feedback-single-ring-readback-idle");
    const bool preciseNoSemaphoreFeedback =
        !strcmp(profile, "shadow-precise-no-semaphore-feedback") ||
        preciseNoSemaphoreFeedbackSingleRing ||
        !strcmp(profile,
                "shadow-precise-no-semaphore-feedback-single-ring-trace");
    /* Keep the guest single-ring workaround while restoring completion-time
     * Host-to-Guest visibility. This is a bounded diagnostic A/B, not a
     * product profile: it separates transport corruption from readback
     * coverage without changing the established precise path. */
    const bool fullNoSemaphoreFeedbackSingleRingTrace =
        !strcmp(profile,
                "shadow-full-no-semaphore-feedback-single-ring-trace");
    const bool legacyHostSync =
        !strcmp(profile, "shadow-precise-legacy-host-sync");
    const bool preciseDirtyPerf = !strcmp(profile, "shadow-precise-dirty-ring-perf");
    const bool preciseDirtyGpuFrameProfile =
        !strcmp(profile, "shadow-precise-dirty-ring-gpu-frame-profile");
    const bool preciseDirtyFrameTimeline =
        !strcmp(profile, "shadow-precise-dirty-ring-frame-timeline");
    const bool preciseDirtyNoMerge = !strcmp(profile, "shadow-precise-dirty-ring-no-merge");
    const bool preciseDirtyNoUpload = !strcmp(profile, "shadow-precise-dirty-ring-no-upload");
    const bool preciseDirtyNoUploadFast =
        !strcmp(profile, "shadow-precise-dirty-ring-no-upload-fast");
    const bool preciseDirtyDescriptorSerialized =
        !strcmp(profile, "shadow-precise-dirty-ring-inline-upload-descriptor-serialized");
    const bool preciseDirtyCoverageSort =
        !strcmp(profile, "shadow-precise-dirty-ring-inline-upload-coverage-sort");
    /* Diagnostic only: submit the private upload separately and wait for its
     * fence before the Guest copy, without a queue-wide idle. */
    const bool preciseDirtyUploadWait =
        !strcmp(profile, "shadow-precise-dirty-ring-upload-wait");
    const bool preciseDirtyCoverageSortSampled =
        !strcmp(profile, "shadow-precise-dirty-ring-coverage-sort-sampled");
    /* Keep the established precise-dirty/coverage upload path unchanged
     * while measuring only the host-side completion-wait mechanism. */
    const bool preciseDirtyCoveragePoll =
        !strcmp(profile, "shadow-precise-dirty-ring-coverage-poll");
    const bool preciseDirtyAliasCover =
        !strcmp(profile,
                "shadow-precise-dirty-ring-inline-upload-alias-cover");
    const bool preciseDirtyBgraArrayTrace =
        !strcmp(profile, "shadow-precise-dirty-ring-bgra-array-trace");
    const bool preciseDirtyFrameAssocTrace =
        !strcmp(profile, "shadow-precise-dirty-ring-frame-assoc-trace") ||
        preciseDirtyBgraArrayTrace;
    const bool preciseDirtyPresentImageTrace =
        !strcmp(profile, "shadow-precise-dirty-ring-present-image-trace");
    const bool preciseDirtyInlineUpload =
        !strcmp(profile, "shadow-precise-dirty-ring-inline-upload") ||
        preciseDirtyCoverageSort || preciseDirtyCoverageSortSampled ||
        preciseDirtyCoveragePoll ||
        preciseDirtyDescriptorSerialized ||
        preciseDirtyFrameAssocTrace || preciseDirtyAliasCover;
    const bool preciseDirtyInlineUploadSerialized =
        !strcmp(profile, "shadow-precise-dirty-ring-inline-upload-serialized");
    const bool preciseDirtyRing =
        !strcmp(profile, "shadow-precise-dirty-ring") ||
        preciseDirtyPresentImageTrace;
    const bool trace = !strcmp(profile, "shadow-trace") || preciseStrongTrace ||
        !strcmp(profile,
                "shadow-precise-no-semaphore-feedback-single-ring-trace") ||
        fullNoSemaphoreFeedbackSingleRingTrace;
    const bool explicitToHost = !strcmp(profile, "shadow-to-host-explicit");
    const bool deferShmemUnref = !strcmp(profile, "shadow-precise-retain-shmem");
    const bool cpuShadowUpload =
        !strcmp(profile, "shadow-precise-cpu-upload");
    const bool waitShadowUpload = !strcmp(profile, "shadow-precise-sync-submit") ||
        preciseDirtyUploadWait;
    const bool mailboxPresent = !strcmp(profile, "shadow-precise-strong-ring-mailbox");
    const bool asyncPresent = !strcmp(
        profile, "shadow-precise-strong-ring-async-present");
    const bool pollPresent = !strcmp(
        profile, "shadow-precise-strong-ring-fence-poll") ||
        preciseDirtyCoveragePoll;
    const bool precise = !strcmp(profile, "shadow-precise") ||
        preciseNoSemaphoreFeedback ||
        !strcmp(profile, "shadow-precise-single-ring") ||
        !strcmp(profile, "shadow-precise-sync-submit") ||
        (!strcmp(profile, "shadow-precise-strong-ring") || legacyHostSync || preciseStrongTrace ||
         preciseStrongPerf || preciseDirtyRing || preciseDirtyPerf || preciseDirtyNoMerge || preciseDirtyNoUpload ||
         preciseDirtyGpuFrameProfile ||
         preciseDirtyFrameTimeline ||
         preciseDirtyCoverageSortSampled ||
         preciseDirtyNoUploadFast || preciseDirtyInlineUpload ||
        preciseDirtyUploadWait ||
        preciseDirtyInlineUploadSerialized) ||
        asyncPresent ||
        pollPresent ||
        mailboxPresent ||
        directFence ||
        deferShmemUnref ||
        cpuShadowUpload;
    const char* mode = (preciseDirtyRing || preciseDirtyPerf || preciseDirtyGpuFrameProfile ||
                        preciseDirtyFrameTimeline ||
                        preciseDirtyCoverageSortSampled || preciseDirtyNoUpload ||
                        preciseDirtyNoUploadFast || preciseDirtyInlineUpload ||
                        preciseDirtyUploadWait ||
                        preciseDirtyInlineUploadSerialized ||
                        preciseDirtyNoMerge) ? "precise-dirty" : precise ? "precise" : skip ? "none" :
        (explicitToHost ? "to-host-explicit" : "full");
    setenv("VKR_WINEHUA_SHADOW_FROM_HOST", mode, 1);
    /* Preserve the precise shadow contract while carrying one diagnostic
     * selector through the existing graphics-broker IPC. The child converts
     * this selector to the concrete renderer flags before vtest starts. */
    const char* shadowSelector =
        preciseNoSemaphoreFeedbackSingleRing ? "gpu-upload" :
        legacyHostSync ? "legacy-host-sync" :
        preciseDirtyAliasCover ? "inline-gpu-upload-alias-cover" :
        preciseDirtyUploadWait ? "gpu-upload-wait" :
        preciseDirtyCoveragePoll ? "inline-gpu-upload-coverage-sort" :
        preciseDirtyCoverageSortSampled ? "inline-gpu-upload-coverage-sort-sampled" :
        preciseDirtyBgraArrayTrace ? "inline-gpu-upload-bgra-array-trace" :
        preciseDirtyCoverageSort ? "inline-gpu-upload-coverage-sort" :
        preciseDirtyDescriptorSerialized ? "inline-gpu-upload-descriptor-serialized" :
        preciseDirtyFrameAssocTrace ? "inline-gpu-upload-frame-assoc-trace" :
        preciseDirtyPresentImageTrace ? "present-image-trace" :
        preciseDirtyFrameTimeline ? "frame-timeline" :
        preciseDirtyGpuFrameProfile ? "gpu-frame-profile" :
        cpuShadowUpload ? "cpu-upload" :
        preciseDirtyInlineUploadSerialized ? "inline-gpu-upload-serialized" :
        preciseDirtyInlineUpload ? "inline-gpu-upload" :
        preciseDirtyNoUpload ? "no-gpu-upload" :
        preciseDirtyNoUploadFast ? "no-gpu-upload-fast" :
        (preciseStrongPerf || preciseDirtyPerf || preciseDirtyNoMerge) ? "perf" :
        directFence ? "vkd3d-gate-c" :
        trace ? "1" : "0";
    setenv("VKR_WINEHUA_SHADOW_TRACE", shadowSelector, 1);
    setenv("VKR_WINEHUA_SHADOW_MERGE_RANGES", preciseDirtyNoMerge ? "0" : "1", 1);
    setenv("VKR_WINEHUA_GPU_UPLOAD_WAIT", waitShadowUpload ? "1" : "0", 1);
    setenv("VKR_WINEHUA_DESCRIPTOR_UPDATE_SERIALIZE",
           preciseDirtyDescriptorSerialized ? "1" : "0", 1);
    setenv("VN_WINEHUA_DEFER_SHMEM_UNREF", deferShmemUnref ? "1" : "0", 1);
    const char* presentMode = mailboxPresent ? "mailbox" :
        (asyncPresent ? "fifo-async" : (pollPresent ? "fifo-poll" : "fifo"));
    setenv("WINEHUA_VENUS_PRESENT_MODE", presentMode, 1);
    /* Keep the App-side control plane separate from renderer environment.
     * Phone hosts run in this process, so virgl_child's derived renderer
     * settings must not change the profile observed by a later EnsureStarted. */
    setenv("WINEHUA_VIRGL_HOST_SHADOW_MODE", mode, 1);
    setenv("WINEHUA_VIRGL_HOST_SHADOW_SELECTOR", shadowSelector, 1);
    setenv("WINEHUA_VIRGL_HOST_SHADOW_MERGE_RANGES",
           preciseDirtyNoMerge ? "0" : "1", 1);
    setenv("WINEHUA_VIRGL_HOST_GPU_UPLOAD_WAIT",
           waitShadowUpload ? "1" : "0", 1);
    setenv("WINEHUA_VIRGL_HOST_DESCRIPTOR_UPDATE_SERIALIZE",
           preciseDirtyDescriptorSerialized ? "1" : "0", 1);
    setenv("WINEHUA_VIRGL_HOST_PRESENT_MODE", presentMode, 1);
    /* Gate C owns this writable log path so its Host-side vtest diagnostics
     * can be retrieved through HDC. Other profiles keep the regular cache. */
    setenv("WINEHUA_VIRGL_HOST_LOG_PATH",
           directFence
               ? "/data/storage/el2/base/temp/vkd3d_virgl_host.log"
               : "/data/storage/el2/base/cache/winehua_virgl_host.log",
           1);
    OH_LOG_INFO(LOG_APP,
                "[NAPI] host shadow profile=%{public}s mode=%{public}s "
                "trace=%{public}s selector=%{public}s perf_summary=%{public}s "
                "gpu_upload=%{public}s upload_wait=%{public}s "
                "descriptor_serialize=%{public}s defer_shmem_unref=%{public}s "
                "present_mode=%{public}s",
                profile, mode, trace ? "1" : "0", shadowSelector,
                (preciseStrongPerf || preciseDirtyPerf || preciseDirtyNoMerge ||
                 preciseDirtyNoUpload || preciseDirtyInlineUpload ||
                 preciseDirtyInlineUploadSerialized) ? "1" : "0",
                (legacyHostSync || preciseDirtyNoUpload || preciseDirtyNoUploadFast) ? "0" :
                    (cpuShadowUpload ? "cpu" : "auto"),
                waitShadowUpload ? "1" : "0",
                preciseDirtyDescriptorSerialized ? "1" : "0",
                deferShmemUnref ? "1" : "0", presentMode);

    napi_value result;
    napi_get_boolean(env, true, &result);
    return result;
}

static napi_value LaunchClient(napi_env env, napi_callback_info info) {
    size_t argc = 13;
    napi_value args[13] = {};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    auto* p = new LaunchParams();

    char buf[2048] = {};
    napi_get_value_string_utf8(env, args[0], buf, sizeof(buf), nullptr);
    p->exePath = buf;
    napi_get_value_string_utf8(env, args[2], buf, sizeof(buf), nullptr);
    p->sockPath = buf;
    napi_get_value_string_utf8(env, args[3], buf, sizeof(buf), nullptr);
    p->libPath = buf;
    if (argc >= 5) {
        napi_get_value_string_utf8(env, args[4], buf, sizeof(buf), nullptr);
        p->homeDir = buf;
    }
    std::string containerId = winehua::kDefaultContainerId;
    if (argc >= 9) {
        napi_valuetype type;
        size_t length = 0;
        if (napi_typeof(env, args[8], &type) != napi_ok || type != napi_string ||
            napi_get_value_string_utf8(env, args[8], nullptr, 0, &length) != napi_ok ||
            length == 0 || length > 64) {
            delete p;
            napi_value failed;
            napi_create_int32(env, -1, &failed);
            return failed;
        }
        std::vector<char> value(length + 1);
        if (napi_get_value_string_utf8(env, args[8], value.data(), value.size(), &length) != napi_ok) {
            delete p;
            napi_value failed;
            napi_create_int32(env, -1, &failed);
            return failed;
        }
        containerId.assign(value.data(), length);
    }
    winehua::ContainerSession container;
    if (!winehua::SetActiveContainerSession(containerId, &container)) {
        OH_LOG_ERROR(LOG_APP, "[Launch] invalid container id=%{public}s", containerId.c_str());
        delete p;
        napi_value failed;
        napi_create_int32(env, -1, &failed);
        return failed;
    }
    p->containerId = container.id;
    p->prefixDir = container.prefixDir;
    // Prefix and socket must be derived together.  Passing an arbitrary
    // socket path here would split the Wine child from its session runtime.
    p->sockPath = container.waylandSocket;
    if (argc >= 6) {
        char d3dBackend[64] = {};
        napi_get_value_string_utf8(env, args[5], d3dBackend, sizeof(d3dBackend), nullptr);
        if (!strcmp(d3dBackend, "wined3d") || !strcmp(d3dBackend, "dxvk_legacy") ||
            !strcmp(d3dBackend, "dxvk_modern_2_6") ||
            !strcmp(d3dBackend, "dxvk_legacy_d3d9") || !strcmp(d3dBackend, "wined3d_zink"))
            p->d3dBackend = d3dBackend;
    }
    if (argc >= 7) {
        char dxvkBackend[64] = {};
        napi_get_value_string_utf8(env, args[6], dxvkBackend, sizeof(dxvkBackend), nullptr);
        if (!strcmp(dxvkBackend, "dxvk_legacy") || !strcmp(dxvkBackend, "dxvk_modern_2_6"))
            p->dxvkBackend = dxvkBackend;
    }
    if (argc >= 8) {
        // 设置页 "Wine 语言": 仅接受白名单值, 非法/缺省保持 zh_CN
        char wineLang[16] = {};
        napi_get_value_string_utf8(env, args[7], wineLang, sizeof(wineLang), nullptr);
        if (!strcmp(wineLang, "zh_CN") || !strcmp(wineLang, "zh_TW") ||
            !strcmp(wineLang, "ja_JP") || !strcmp(wineLang, "en_US"))
            p->wineLang = wineLang;
    }
    winehua::SetActiveContainerLanguage(p->wineLang);
    if (argc >= 10) {
        bool directNcpSession = false;
        if (napi_get_value_bool(env, args[9], &directNcpSession) != napi_ok) {
            delete p;
            napi_value failed;
            napi_create_int32(env, -1, &failed);
            return failed;
        }
        p->directNcpSession = directNcpSession;
    }
    if (argc >= 11 && napi_get_value_bool(env, args[10], &p->desktopVulkanCompositor) != napi_ok) {
        delete p;
        napi_value failed;
        napi_create_int32(env, -1, &failed);
        return failed;
    }
    if (argc >= 12 && (napi_get_value_int32(env, args[11], &p->desktopStallSeconds) != napi_ok ||
                       p->desktopStallSeconds < 0 || p->desktopStallSeconds > 120)) {
        delete p;
        napi_value failed;
        napi_create_int32(env, -1, &failed);
        return failed;
    }
    if (argc >= 13) {
        bool array = false;
        uint32_t length = 0;
        bool valid = napi_is_array(env, args[12], &array) == napi_ok && array &&
            napi_get_array_length(env, args[12], &length) == napi_ok && length <= 32;
        for (uint32_t index = 0; valid && index < length; ++index) {
            napi_value item;
            size_t size = 0;
            valid = napi_get_element(env, args[12], index, &item) == napi_ok &&
                napi_get_value_string_utf8(env, item, nullptr, 0, &size) == napi_ok && size <= 256;
            if (!valid) break;
            std::vector<char> line(size + 1);
            valid = napi_get_value_string_utf8(env, item, line.data(), line.size(), &size) == napi_ok;
            const std::string value(line.data(), size);
            valid = valid && winehua::IsWineLaunchEnvironmentLine(value);
            if (valid) p->cpuEnvironment.push_back(value);
        }
        if (!valid) {
            delete p;
            napi_value failed;
            napi_create_int32(env, -1, &failed);
            return failed;
        }
    }
    // Current product callers leave legacy Direct booleans false. Resolve
    // them from the same Native policy that owns the child environment.
    winehua::ProductGraphicsPolicy sessionPolicy;
    if (winehua::ResolveProductGraphicsPolicy(
            winehua::ParseD3dBackend(p->d3dBackend), &sessionPolicy)) {
        p->directNcpSession = sessionPolicy.guest.directVulkan;
        p->desktopVulkanCompositor = sessionPolicy.vulkanDesktop;
    }
    // 向后兼容: 旧调用未传 homeDir 时使用默认路径
    if (p->homeDir.empty()) {
        p->homeDir = "/storage/Users/currentUser/Download";
    }

    OH_LOG_WARN(LOG_APP,
                "[Launch] container=%{public}s exe=%{public}s sock=%{public}s lib=%{public}s home=%{public}s prefix=%{public}s (async)",
                p->containerId.c_str(),
                p->exePath.c_str(), p->sockPath.c_str(), p->libPath.c_str(), p->homeDir.c_str(),
                p->prefixDir.c_str());
    OH_LOG_WARN(LOG_APP, "[Launch] desktop D3D=%{public}s DXVK=%{public}s lang=%{public}s directNcp=%{public}d",
                p->d3dBackend.c_str(), p->dxvkBackend.c_str(), p->wineLang.c_str(),
                p->directNcpSession ? 1 : 0);

    // 保证可执行
    if (access(p->exePath.c_str(), X_OK) != 0) chmod(p->exePath.c_str(), 0755);

    // 提取 sockDir, sockName, winehuaBin
    auto pos = p->sockPath.find_last_of('/');
    p->sockDir = (pos == std::string::npos) ? "/tmp" : p->sockPath.substr(0, pos);
    p->sockName = (pos == std::string::npos) ? p->sockPath : p->sockPath.substr(pos + 1);
    pos = p->exePath.find_last_of('/');
    p->winehuaBin = (pos != std::string::npos) ? p->exePath.substr(0, pos) : p->exePath;

    p->generation = BeginWineLaunch();
    if (!p->generation) {
        delete p;
        napi_value failed;
        napi_create_int32(env, -1, &failed);
        return failed;
    }
    // 启动后台线程: wineserver -> wineboot --init
    std::thread(LaunchThreadFunc, p).detach();

    OH_LOG_WARN(LOG_APP, "[Launch] background thread started, returning to JS");

    napi_value r;
    napi_create_int32(env, 0, &r);
    return r;
}

static napi_value GetWineStartupState(napi_env env, napi_callback_info) {
    const auto snapshot = GetWineStartupSnapshot();
    // Values are internal fixed protocol tokens, never account/user data.
    const std::string json = "{\"generation\":" + std::to_string(snapshot.generation) +
        ",\"active\":" + (snapshot.active ? "true" : "false") +
        ",\"state\":\"" + snapshot.state + "\",\"phase\":\"" + snapshot.phase + "\"}";
    napi_value value;
    napi_create_string_utf8(env, json.c_str(), json.size(), &value);
    return value;
}

// -- NAPI: checkWinePrefix -- 检测 .wine 是否已完整初始化 --
static napi_value CheckWinePrefix(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1] = {};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    std::string containerId = winehua::kDefaultContainerId;
    if (argc >= 1) {
        size_t length = 0;
        napi_valuetype type;
        if (napi_typeof(env, args[0], &type) != napi_ok || type != napi_string ||
            napi_get_value_string_utf8(env, args[0], nullptr, 0, &length) != napi_ok ||
            length == 0 || length > 64) {
            napi_value invalid;
            napi_get_boolean(env, false, &invalid);
            return invalid;
        }
        std::vector<char> value(length + 1);
        napi_get_value_string_utf8(env, args[0], value.data(), value.size(), &length);
        containerId.assign(value.data(), length);
    }
    winehua::ContainerSession container;
    if (!winehua::ResolveContainerSession(containerId, &container)) {
        napi_value invalid;
        napi_get_boolean(env, false, &invalid);
        return invalid;
    }
    const std::string& prefix = container.prefixDir;
    const std::string initMarker = prefix + "/.winehua-init-in-progress";
    bool ok = IsWinePrefixInitialized(prefix)
        && access(initMarker.c_str(), F_OK) != 0;
    OH_LOG_WARN(LOG_APP, "[Wine] checkWinePrefix prefix=%{public}s initialized=%{public}s",
                prefix.c_str(), ok ? "yes" : "no");
    napi_value r;
    napi_get_boolean(env, ok, &r);
    return r;
}

// -- NAPI: resetWinePrefix -- 一键清空受管 prefix 目录
static napi_value ResetWinePrefix(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1] = {};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    std::string containerId = winehua::kDefaultContainerId;
    if (argc >= 1) {
        size_t length = 0;
        napi_valuetype type;
        if (napi_typeof(env, args[0], &type) != napi_ok || type != napi_string ||
            napi_get_value_string_utf8(env, args[0], nullptr, 0, &length) != napi_ok ||
            length == 0 || length > 64) {
            napi_value invalid;
            napi_get_boolean(env, false, &invalid);
            return invalid;
        }
        std::vector<char> value(length + 1);
        napi_get_value_string_utf8(env, args[0], value.data(), value.size(), &length);
        containerId.assign(value.data(), length);
    }
    winehua::ContainerSession container;
    if (!winehua::ResolveContainerSession(containerId, &container)) {
        napi_value invalid;
        napi_get_boolean(env, false, &invalid);
        return invalid;
    }
    const char* prefix = container.prefixDir.c_str();
    OH_LOG_WARN(LOG_APP, "[NAPI] resetWinePrefix called container=%{public}s prefix=%{public}s",
                container.id.c_str(), prefix);
    CancelWineLaunch();
    KillAllProcesses();
    const std::string keep = container.id == winehua::kDefaultContainerId
        ? container.prefixDir + "/drive_c/SteamLinuxARM64" : "";
    bool ok = winehua::RemovePrefixTree(prefix, keep);
    if (mkdir(prefix, 0755) != 0 && errno != EEXIST) {
        ok = false;
        OH_LOG_ERROR(LOG_APP, "[NAPI] mkdir %{public}s failed: %{public}s",
                     prefix, strerror(errno));
    }
    OH_LOG_WARN(LOG_APP, "[NAPI] resetWinePrefix: %{public}s %{public}s",
                prefix, ok ? "cleared and recreated" : "reset failed");
    napi_value result;
    napi_get_boolean(env, ok, &result);
    return result;
}

// -- NAPI: stopClient — 杀掉所有 Wine 进程 --
static napi_value StopClient(napi_env, napi_callback_info) {
    /* Stop the VirGL renderer before reaping its Wine descendants.  The
     * renderer is itself an app-owned NCP child; killing the whole process
     * tree first can bypass its bounded shutdown and leave a stale Venus ring
     * across the next isolated session. */
    CancelWineLaunch();
    if (!WaylandServer::GetInstance()->HasExternalUsers()) winehua::GraphicsBroker::GetInstance().Stop();
    KillAllProcesses();
    // 会话终结统一收口 (与桌面退出同路径): 杀进程后进程级一次性状态全部
    // 复位, 下次引擎启动从冷启动基线开始 (StopAll 走 WaylandServer::Stop
    // 全量重建, 无需这里处理)
    if (!WaylandServer::GetInstance()->HasExternalUsers()) {
        WaylandServer::GetInstance()->ResetSessionState();
        WaylandServer::GetInstance()->ResetFirstFrame();
    }
    return nullptr;
}

// -- NAPI: stopAll — 杀掉所有 Wine 进程 (含主 wineserver) + 停 Wayland server --
static napi_value StopAll(napi_env, napi_callback_info) {
    CancelWineLaunch();
    if (!WaylandServer::GetInstance()->HasExternalUsers()) winehua::GraphicsBroker::GetInstance().Stop();
    KillAllProcesses();
    WaylandServer::GetInstance()->Stop();
    // 会话终结信号: zombie 感知等待全部死亡后发一次 state:stopped —
    // ArkTS 重启/重置/停止编排以它为继续条件 (取代阶段1 的进程表轮询)。
    // 放在 Wayland Stop (同步 join) 之后, 保证"完全退出"判据三项齐备才发声
    NotifyWhenSessionDrained();
    return nullptr;
}

// -- Toplevel 回调 -> ArkTS --
static napi_threadsafe_function gToplevelTsfn = nullptr;

struct ToplevelEvent {
    uint32_t id;
    std::string event;
    std::string data;
};

static void CallJsToplevel(napi_env env, napi_value cb, void*, void* raw) {
    auto* ev = static_cast<ToplevelEvent*>(raw);
    if (env && cb && ev) {
        OH_LOG_INFO(LOG_APP, "[MW-TSCB] calling JS toplevel cb: id=%{public}u event=%{public}s data=%{public}s",
                    ev->id, ev->event.c_str(), ev->data.c_str());
        napi_value undef, args[3];
        napi_get_undefined(env, &undef);
        napi_create_uint32(env, ev->id, &args[0]);
        napi_create_string_utf8(env, ev->event.c_str(), NAPI_AUTO_LENGTH, &args[1]);
        napi_create_string_utf8(env, ev->data.c_str(), NAPI_AUTO_LENGTH, &args[2]);
        napi_call_function(env, undef, cb, 3, args, nullptr);
    }
    delete ev;
}

// -- NAPI: setToplevelCallback --
static napi_value SetToplevelCallback(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    if (gToplevelTsfn) {
        napi_release_threadsafe_function(gToplevelTsfn, napi_tsfn_release);
        gToplevelTsfn = nullptr;
    }

    napi_value name;
    napi_create_string_utf8(env, "WLToplevel", NAPI_AUTO_LENGTH, &name);
    napi_create_threadsafe_function(env, args[0], nullptr, name,
                                     0, 1, nullptr, nullptr, nullptr, CallJsToplevel, &gToplevelTsfn);

    WaylandServer::GetInstance()->SetToplevelCallback([](uint32_t id, const char* event, const char* data) {
        if (gToplevelTsfn) {
            OH_LOG_INFO(LOG_APP, "[MW-TSCB] enqueue toplevel cb: id=%{public}u event=%{public}s", id, event);
            auto* ev = new ToplevelEvent{id, event ? event : "", data ? data : "{}"};
            napi_call_threadsafe_function(gToplevelTsfn, ev, napi_tsfn_blocking);
        } else {
            OH_LOG_WARN(LOG_APP, "[MW-TSCB] toplevel cb dropped (tsfn not ready): id=%{public}u event=%{public}s",
                        id, event);
        }
    });

    return nullptr;
}

// -- IME 激活回调 -> ArkTS (Wine 文本框聚焦 → 通知 ArkTS attach 弹软键盘) --
static napi_threadsafe_function gImeTsfn = nullptr;

struct ImeEvent {
    int active;
    int x, y, w, h;
};

static void CallJsIme(napi_env env, napi_value cb, void*, void* raw) {
    auto* ev = static_cast<ImeEvent*>(raw);
    if (env && cb && ev) {
        napi_value undef, args[5];
        napi_get_undefined(env, &undef);
        napi_create_int32(env, ev->active, &args[0]);
        napi_create_int32(env, ev->x, &args[1]);
        napi_create_int32(env, ev->y, &args[2]);
        napi_create_int32(env, ev->w, &args[3]);
        napi_create_int32(env, ev->h, &args[4]);
        napi_call_function(env, undef, cb, 5, args, nullptr);
    }
    delete ev;
}

// -- NAPI: setImeCallback -- (激活/失活回调注册, 同 setToplevelCallback 模式)
static napi_value SetImeCallback(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    if (gImeTsfn) {
        napi_release_threadsafe_function(gImeTsfn, napi_tsfn_release);
        gImeTsfn = nullptr;
    }

    napi_value name;
    napi_create_string_utf8(env, "WL_Ime", NAPI_AUTO_LENGTH, &name);
    napi_create_threadsafe_function(env, args[0], nullptr, name,
                                    0, 1, nullptr, nullptr, nullptr, CallJsIme, &gImeTsfn);

    TextInput::GetInstance()->SetActivateCallback([](bool active, int x, int y, int w, int h) {
        if (gImeTsfn) {
            auto* ev = new ImeEvent{active ? 1 : 0, x, y, w, h};
            napi_call_threadsafe_function(gImeTsfn, ev, napi_tsfn_blocking);
        } else {
            OH_LOG_WARN(LOG_APP, "[WL_NAPI] ime cb dropped (tsfn not ready) active=%{public}d", active);
        }
    });

    return nullptr;
}

static napi_value WineTextInputFocusedToplevel(napi_env env, napi_callback_info) {
    napi_value value;
    napi_create_uint32(env, winehua_display_focus_is_x11() ? winehua_display_focus_id() : TextInput::GetInstance()->FocusedToplevel(), &value);
    return value;
}

// -- NAPI: sendImeCommit -- (ArkTS inputMethod insertText → Wine commit_string)
static napi_value SendImeCommit(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    size_t len = 0;
    napi_get_value_string_utf8(env, args[0], nullptr, 0, &len);
    std::string text(len, '\0');
    napi_get_value_string_utf8(env, args[0], &text[0], len + 1, &len);
    if (winehua_display_focus_is_x11()) wl_ohos_input_post_text(text.c_str());
    else TextInput::GetInstance()->SendCommitString(text.c_str());
    return nullptr;
}

// -- NAPI: sendImePreedit -- (ArkTS setPreviewText 预上屏 → Wine preedit_string)
static napi_value SendImePreedit(napi_env env, napi_callback_info info) {
    size_t argc = 3;
    napi_value args[3];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    size_t len = 0;
    napi_get_value_string_utf8(env, args[0], nullptr, 0, &len);
    std::string text(len, '\0');
    napi_get_value_string_utf8(env, args[0], &text[0], len + 1, &len);
    int32_t b = 0, e = 0;
    napi_get_value_int32(env, args[1], &b);
    napi_get_value_int32(env, args[2], &e);
    if (!winehua_display_focus_is_x11()) TextInput::GetInstance()->SendPreeditString(text.c_str(), b, e);
    return nullptr;
}

// -- NAPI: imeBackspace -- (软键盘退格 → Wine KEY_BACKSPACE 按键注入;
//    Wine 的 delete_surrounding_text 是空实现, 退格走现有 key 注入链路)
static napi_value ImeBackspace(napi_env env, napi_callback_info info) {
    (void)env;
    (void)info;
    constexpr uint32_t KEY_BACKSPACE = 14;
    if (winehua_display_focus_is_x11()) {
        wl_ohos_input_post_key(KEY_BACKSPACE, true);
        wl_ohos_input_post_key(KEY_BACKSPACE, false);
        return nullptr;
    }
    auto* im = InputManager::GetInstance();
    im->InjectKeyboardKey(KEY_BACKSPACE, WL_KEYBOARD_KEY_STATE_PRESSED);
    im->InjectKeyboardKey(KEY_BACKSPACE, WL_KEYBOARD_KEY_STATE_RELEASED);
    return nullptr;
}

// -- NAPI: getCurrentToplevelId -- (WineWindow.aboutToAppear 同步读取, 无竞态)
static napi_value GetCurrentToplevelId(napi_env env, napi_callback_info info) {
    uint32_t id = PluginManager::GetInstance()->DequeuePendingToplevel();
    OH_LOG_INFO(LOG_APP, "[MW-NAPI] getCurrentToplevelId = %{public}u", id);
    napi_value r;
    napi_create_uint32(env, id, &r);
    return r;
}

// -- NAPI: cancelPendingToplevel -- (窗口在 loadContent 前被销毁时清除队列残坑,
//   防止后续页面出队拿到死 id → 渲染器挂错 toplevel 黑屏; 未在队列时 no-op)
static napi_value CancelPendingToplevel(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    uint32_t id = 0;
    napi_get_value_uint32(env, args[0], &id);
    PluginManager::GetInstance()->CancelPendingToplevel(id);
    return nullptr;
}

// -- NAPI: setPendingToplevel -- (WineWindowAbility 在 loadContent 前调用)
static napi_value SetPendingToplevel(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    uint32_t id = 0;
    napi_get_value_uint32(env, args[0], &id);
    PluginManager::GetInstance()->SetPendingToplevel(id);
    OH_LOG_INFO(LOG_APP, "[MW-NAPI] setPendingToplevel id=%{public}u", id);
    return nullptr;
}

// -- NAPI: destroyToplevel -- (ArkTS 关闭子窗口后调用)
static napi_value DestroyToplevel(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    uint32_t id = 0;
    napi_get_value_uint32(env, args[0], &id);
    if (!DirectWineSurfaceUnbindOutput(id))
        PluginManager::GetInstance()->DestroyToplevel(id);
    // 相对模式锁定兜底: 宿主主动销毁 toplevel (WWA 关窗) 时释放 host 锁定 —
    // 游戏卡死时 wine 不响应 sendToplevelClose, relative_pointer 永不销毁,
    // 正常解锁回调不来 (见 PointerExtras::ReleaseLockForToplevel)
    PointerExtras::GetInstance()->ReleaseLockForToplevel(id);
    OH_LOG_INFO(LOG_APP, "[MW-NAPI] destroyToplevel id=%{public}u", id);
    return nullptr;
}

// -- NAPI: sendToplevelClose -- (通知 Wine 关闭窗口)
static napi_value SendToplevelClose(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    uint32_t id = 0;
    napi_get_value_uint32(env, args[0], &id);
    OH_LOG_INFO(LOG_APP, "[MW-NAPI] sendToplevelClose id=%{public}u", id);
    if (x11_toplevel_owns(id)) x11_toplevel_close(id);
    else WaylandServer::GetInstance()->SendToplevelClose(id);
    return nullptr;
}

// -- NAPI: createRenderer -- (XComponentController.onSurfaceCreated 调用)
static napi_value SetToplevelIsolated(napi_env env, napi_callback_info info) {
    size_t argc = 2; napi_value args[2]{}; uint32_t id = 0; bool isolated = false;
    if (napi_get_cb_info(env, info, &argc, args, nullptr, nullptr) != napi_ok || argc != 2 ||
        napi_get_value_uint32(env, args[0], &id) != napi_ok || !id ||
        napi_get_value_bool(env, args[1], &isolated) != napi_ok) return nullptr;
    PluginManager::GetInstance()->SetToplevelIsolated(id, isolated);
    return nullptr;
}

static napi_value CreateRenderer(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value args[2];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 2) {
        OH_LOG_ERROR(LOG_APP, "[MW-NAPI] createRenderer: need 2 args (toplevelId, surfaceId)");
        return nullptr;
    }
    uint32_t tid = 0;
    napi_get_value_uint32(env, args[0], &tid);
    int64_t surfaceId = 0;
    bool lossless = true;
    napi_status s = napi_get_value_bigint_int64(env, args[1], &surfaceId, &lossless);
    if (s != napi_ok) {
        OH_LOG_ERROR(LOG_APP, "[MW-NAPI] createRenderer: BIGINT parse failed status=%{public}d", s);
        return nullptr;
    }
    OH_LOG_INFO(LOG_APP, "[MW-NAPI] createRenderer tl=%{public}u surfaceId=%{public}ld", tid, surfaceId);
    // Direct capability is not evidence that a window produces Vulkan frames.
    // One compositor always owns the output, including GDI-only windows and
    // later GDI/GPU transitions. Direct queues are imported as GPU layers.
    PluginManager::GetInstance()->CreateRenderer(tid, surfaceId);
    return nullptr;
}

// -- NAPI: resizeRenderer -- (XComponentController.onSurfaceChanged 调用)
static napi_value ResizeRenderer(napi_env env, napi_callback_info info) {
    size_t argc = 4;
    napi_value args[4];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 3) {
        OH_LOG_ERROR(LOG_APP, "[MW-NAPI] resizeRenderer: need 3 args (toplevelId, w, h)");
        return nullptr;
    }
    uint32_t tid = 0;
    napi_get_value_uint32(env, args[0], &tid);
    int32_t w = 0, h = 0;
    napi_get_value_int32(env, args[1], &w);
    napi_get_value_int32(env, args[2], &h);
    OH_LOG_INFO(LOG_APP, "[MW-NAPI] resizeRenderer tl=%{public}u %{public}dx%{public}d", tid, w, h);
    int64_t surfaceId = 0;
    if (argc >= 4) {
        bool lossless = false;
        if (napi_get_value_bigint_int64(env, args[3], &surfaceId, &lossless) != napi_ok || !lossless || !surfaceId) return nullptr;
    }
    PluginManager::GetInstance()->ResizeRenderer(tid, w, h, surfaceId);
    return nullptr;
}

// -- NAPI: destroyRenderer -- (XComponentController.onSurfaceDestroyed 调用)
static napi_value DestroyRenderer(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value args[2];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    uint32_t tid = 0;
    if (argc >= 1) {
        napi_get_value_uint32(env, args[0], &tid);
    }
    OH_LOG_INFO(LOG_APP, "[MW-NAPI] destroyRenderer tl=%{public}u", tid);
    int64_t surfaceId = 0;
    if (argc >= 2) {
        bool lossless = false;
        if (napi_get_value_bigint_int64(env, args[1], &surfaceId, &lossless) != napi_ok || !lossless || !surfaceId) return nullptr;
    }
    PluginManager::GetInstance()->DestroyRendererForSurface(tid, surfaceId);
    return nullptr;
}

// -- NAPI: setOutputSize --
static napi_value SetOutputSize(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value args[2];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 2) return nullptr;
    int32_t w, h;
    napi_get_value_int32(env, args[0], &w);
    napi_get_value_int32(env, args[1], &h);
    WaylandServer::GetInstance()->SetOutputSize(w, h);
    return nullptr;
}

// 已无效: C++ 坐标换算不使用 display scale (letterbox 由 renderer viewport 推导,
// globalDisplayScale_ 只写不读已删除)。保留导出仅为兼容 ArkTS 侧调用, 收到直接忽略。
static napi_value SetDisplayScale(napi_env env, napi_callback_info info) {
    (void)env;
    (void)info;
    return nullptr;
}

static napi_value SetDesktopMode(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc >= 1) {
        bool on;
        napi_get_value_bool(env, args[0], &on);
        WaylandServer::GetInstance()->SetDesktopMode(on);
        OH_LOG_INFO(LOG_APP, "[MW-NAPI] setDesktopMode = %{public}s", on ? "true" : "false");
    }
    return nullptr;
}

static napi_value SetPhoneMode(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc >= 1) {
        bool on;
        napi_get_value_bool(env, args[0], &on);
        PhoneAdapter_SetPhoneMode(on);
        OH_LOG_WARN(LOG_APP, "[MW-NAPI] setPhoneMode = %{public}s", on ? "true" : "false");
    }
    return nullptr;
}

static napi_value GetDesktopRootId(napi_env env, napi_callback_info) {
    uint32_t id = winehua_x11_desktop_id();
    if (!id) id = WaylandServer::GetInstance()->GetDesktopRootToplevelId();
    napi_value r;
    napi_create_uint32(env, id, &r);
    return r;
}

// -- Input forwarding NAPI (unified InputManager path) --
static napi_value SendPointerEvent(napi_env env, napi_callback_info info) {
    size_t argc = 8;
    napi_value args[8];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 5) return nullptr;
    uint32_t tl; int32_t action; double px, py; int32_t button;
    napi_get_value_uint32(env, args[0], &tl);
    napi_get_value_int32(env, args[1], &action);
    napi_get_value_double(env, args[2], &px);
    napi_get_value_double(env, args[3], &py);
    napi_get_value_int32(env, args[4], &button);
    // 可选: MouseEvent.rawDeltaX/Y (API15+, 仅 Move 传; 缺省 0 = 无 raw 数据,
    // InputManager 回退绝对差分); args[7] fromMouse: onMouse 物理鼠标通道
    // 传 true (触屏 onTouch 路径不传) — 相对模式 PRESS 是否跳过 enter 重定位
    double rawDx = 0, rawDy = 0;
    if (argc >= 7) {
        napi_get_value_double(env, args[5], &rawDx);
        napi_get_value_double(env, args[6], &rawDy);
    }
    bool fromMouse = false;
    if (argc >= 8) {
        napi_get_value_bool(env, args[7], &fromMouse);
    }
    // 跳过 MOVE (=3, 高频), 只记录 button/enter/leave。
    // 曾误写 action!=1: 跳过的是 PRESS (日志只见 Release 不见 Press),
    // 且 MOVE 全量刷屏 — ArkTS MouseAction: Press=1 Release=2 Move=3
    // (与 input_manager.cpp ACT_* 注释一致)
    if (action != 3) {
        OH_LOG_INFO(LOG_APP, "[PIPE] ptr tl=%{public}u a=%{public}d btn=0x%{public}x "
                    "px=(%{public}.0f,%{public}.0f) raw=(%{public}.1f,%{public}.1f) fromMouse=%{public}d",
                    tl, action, button, px, py, rawDx, rawDy, fromMouse ? 1 : 0);
    }
    if (winehua_x11_external_pointer(tl, action, px, py, button, rawDx, rawDy, fromMouse)) return nullptr;
    if (action == 1) winehua_display_focus_select(tl, winehua_x11_is_desktop(tl) || x11_toplevel_owns(tl));
    if (winehua_x11_is_desktop(tl)) winehua_x11_canvas_pointer_raw(action, px, py, button, rawDx, rawDy);
    else if (x11_toplevel_owns(tl)) wl_ohos_input_post_mm_pointer_raw(tl, px, py, action, button, rawDx, rawDy);
    else InputManager::GetInstance()->SendPointerEvent(tl, action, px, py, button, rawDx, rawDy, fromMouse);
    return nullptr;
}

// -- NAPI: dumpWindowLayout -- (WMS 全窗口布局列表, 层级排序 index 0 = 最高)
// 白屏 z 序判定: Fusion 主窗 (335 类) 与画面 popup (336 类) 的 rect 在列表中
// 的先后; 与 ArkTS 侧 subWinId 打点对照归属。
static napi_value DumpWindowLayout(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    int64_t displayId = 0;
    if (argc >= 1) napi_get_value_int64(env, args[0], &displayId);
    WindowManager_Rect* list = nullptr;
    size_t count = 0;
    const int32_t ret = OH_WindowManager_GetAllWindowLayoutInfoList(displayId, &list, &count);
    if (ret != 0 || !list) {
        OH_LOG_WARN(LOG_APP, "[WinLayout] Get table failed ret=%{public}d", ret);
        if (list) OH_WindowManager_ReleaseAllWindowLayoutInfoList(list);
        return nullptr;
    }
    for (size_t i = 0; i < count; i++) {
        OH_LOG_INFO(LOG_APP,
                    "[WinLayout] idx=%{public}zu/%{public}zu rect=(%{public}d,%{public}d %{public}ux%{public}u)",
                    i, count, list[i].posX, list[i].posY, list[i].width, list[i].height);
    }
    OH_WindowManager_ReleaseAllWindowLayoutInfoList(list);
    return nullptr;
}

// -- NAPI: registerHostWindow -- (ets 各 Ability 注册主窗口 id, 供
// OH_WindowManager_LockCursor 锁定光标用 — 仅获焦窗口能锁, 逐个尝试)
static napi_value RegisterHostWindow(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 1) return nullptr;
    int32_t windowId = 0;
    napi_get_value_int32(env, args[0], &windowId);
    PointerExtras::RegisterHostWindow(windowId);
    return nullptr;
}

// -- NAPI: setPointerLockCallback -- (锁定状态 → ets 隐藏/恢复系统光标)
// gPointerLockTsfn 在主线程注册/替换、wl 线程回调读取 (20260822 review:
// 裸指针跨线程读写 + 先 release 后 create 是 data race — 窗口重建 (launcher
// resume 再次 init) 恰逢相对模式切换时, wl 线程可能读到已关闭/被交换的
// 句柄 → UAF 或锁状态通知丢失)。修复: atomic 化; 替换时先原子摘除旧句柄
// 再 release(释放后 wl 线程不可能再读到旧值); 读到 closing 中旧句柄时
// napi_call_threadsafe_function 返回错误 — 忽略, 新句柄接管后续事件。
static std::atomic<napi_threadsafe_function> gPointerLockTsfn{nullptr};
static void CallJsPointerLock(napi_env env, napi_value cb, void*, void* data) {
    if (env && cb) {
        // data 打包: bit0 = locked, 高位 = 触发相对模式的约束 surface 的 toplevelId
        // (解锁时该值为 0)。送到 ets 供"桌面 shell 自身藏光标 vs 游戏真相对模式"
        // 的门禁区分。
        uintptr_t packed = reinterpret_cast<uintptr_t>(data);
        const bool locked = (packed & 1u) != 0;
        const uint32_t toplevelId = static_cast<uint32_t>(packed >> 1);
        napi_value undef, argLocked, argTl;
        napi_get_undefined(env, &undef);
        napi_get_boolean(env, locked, &argLocked);
        napi_create_uint32(env, toplevelId, &argTl);
        napi_value args[2] = {argLocked, argTl};
        napi_call_function(env, undef, cb, 2, args, nullptr);
    }
}
static napi_value SetPointerLockCallback(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 1) return nullptr;
    if (napi_threadsafe_function old = gPointerLockTsfn.exchange(nullptr)) {
        napi_release_threadsafe_function(old, napi_tsfn_release);
    }
    napi_value name;
    napi_create_string_utf8(env, "WLPointerLock", NAPI_AUTO_LENGTH, &name);
    napi_threadsafe_function newTsfn = nullptr;
    if (napi_create_threadsafe_function(env, args[0], nullptr, name,
                                         0, 1, nullptr, nullptr, nullptr, CallJsPointerLock,
                                         &newTsfn) != napi_ok) {
        OH_LOG_ERROR(LOG_APP, "[NAPI] setPointerLockCallback: create tsfn failed");
        return nullptr;
    }
    gPointerLockTsfn.store(newTsfn);
    PointerExtras::GetInstance()->SetPointerLockCallback([](bool locked, uint32_t toplevelId) {
        if (napi_threadsafe_function tsfn = gPointerLockTsfn.load()) {
            // bit0 = locked, 高位 = toplevelId (见 CallJsPointerLock 解包)
            uintptr_t packed = static_cast<uintptr_t>(locked ? 1u : 0u) |
                               (static_cast<uintptr_t>(toplevelId) << 1);
            if (napi_call_threadsafe_function(tsfn, reinterpret_cast<void*>(packed),
                    napi_tsfn_blocking) != napi_ok) {
                // tsfn 正在关闭 (窗口重建竞态): 忽略, 新句柄接管后续事件
            }
        }
    });
    return nullptr;
}

static napi_value SendKeyEvent(napi_env env, napi_callback_info info) {
    size_t argc = 3;
    napi_value args[3];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 3) return nullptr;
    uint32_t tl; int32_t evdevCode; bool pressed;
    napi_get_value_uint32(env, args[0], &tl);
    napi_get_value_int32(env, args[1], &evdevCode);
    napi_get_value_bool(env, args[2], &pressed);
    OH_LOG_INFO(LOG_APP, "[PIPE] key tl=%{public}u evdev=%{public}d down=%{public}s",
                tl, evdevCode, pressed ? "true" : "false");
    if (winehua_x11_is_desktop(tl) || x11_toplevel_owns(tl)) {
        const uint32_t focused = winehua_display_focus_id();
        if (focused && !winehua_display_focus_is_x11()) {
            InputManager::GetInstance()->SendKeyEvent(focused, evdevCode, pressed);
            return nullptr;
        }
    }
    winehua_display_focus_select(tl, winehua_x11_is_desktop(tl) || x11_toplevel_owns(tl));
    if (winehua_x11_is_desktop(tl)) wl_ohos_input_post_key(evdevCode, pressed);
    else if (x11_toplevel_owns(tl)) x11_toplevel_input_key(tl, evdevCode, pressed);
    else InputManager::GetInstance()->SendKeyEvent(tl, evdevCode, pressed);
    return nullptr;
}

static napi_value SendScrollEvent(napi_env env, napi_callback_info info) {
    size_t argc = 6;
    napi_value args[6];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 6) return nullptr;
    uint32_t tl; int32_t axis; double value; int32_t scrollStep; double px; double py;
    napi_get_value_uint32(env, args[0], &tl);
    napi_get_value_int32(env, args[1], &axis);
    napi_get_value_double(env, args[2], &value);
    napi_get_value_int32(env, args[3], &scrollStep);
    napi_get_value_double(env, args[4], &px);
    napi_get_value_double(env, args[5], &py);
    OH_LOG_INFO(LOG_APP, "[PIPE] scroll tl=%{public}u axis=%{public}s val=%{public}.1f step=%{public}d px=(%{public}.0f,%{public}.0f)",
                tl, axis == 0 ? "VERT" : "HORIZ", value, scrollStep, px, py);
    if (winehua_x11_external_scroll(tl, axis, value, scrollStep, px, py)) return nullptr;
    if (winehua_x11_is_desktop(tl)) {
        winehua_x11_canvas_pointer(3, px, py, 0);
        wl_ohos_input_post_axis_value(axis, value, scrollStep < 0);
    } else if (x11_toplevel_owns(tl)) {
        x11_toplevel_input_pointer(tl, px, py, 3, 0);
        wl_ohos_input_post_axis_value(axis, value, scrollStep < 0);
    }
    else InputManager::GetInstance()->SendScrollEvent(tl, axis, value, scrollStep, px, py);
    return nullptr;
}

static napi_value NotifyToplevelResize(napi_env env, napi_callback_info info) {
    size_t argc = 4;
    napi_value args[4];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 3) return nullptr;
    uint32_t tl; int32_t w, h;
    bool resizing = false;
    napi_get_value_uint32(env, args[0], &tl);
    napi_get_value_int32(env, args[1], &w);
    napi_get_value_int32(env, args[2], &h);
    if (argc >= 4) napi_get_value_bool(env, args[3], &resizing);
    OH_LOG_INFO(LOG_APP, "[NAPI] notifyToplevelResize tl=%{public}u %{public}dx%{public}d resize=%{public}s",
                tl, w, h, resizing ? "yes" : "no");
    if (winehua_x11_is_desktop(tl)) WineHua_DisplayRoute_Resize(w, h);
    else if (x11_toplevel_owns(tl)) x11_toplevel_geometry_size(tl, w, h);
    else InputManager::GetInstance()->RequestHostResize(tl, w, h, resizing);
    return nullptr;
}

// 拖拽缩放结束 (ArkTS windowRectChange DRAG_END): 发 configure(0,0) 清 RESIZING
// 状态, Wine 保持当前尺寸 (0 尺寸 → SWP_NOSIZE) 并退出 size-move。
static napi_value NotifyToplevelResizeEnd(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 1) return nullptr;
    uint32_t tl;
    napi_get_value_uint32(env, args[0], &tl);
    OH_LOG_INFO(LOG_APP, "[NAPI] notifyToplevelResizeEnd tl=%{public}u", tl);
    if (!x11_toplevel_owns(tl)) InputManager::GetInstance()->RequestHostResize(tl, 0, 0, false);
    return nullptr;
}

// Desktop 模式: 将 toplevel 提到 Z-order 最顶层
static napi_value RaiseToplevel(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 1) return nullptr;
    uint32_t tl;
    napi_get_value_uint32(env, args[0], &tl);
    // 用户显式操作 (任务栏/窗口点击) 路径: 已 fullscreen 的目标会重新取
    // 全屏优先级号, 支撑两个全屏窗口间的主动切换
    // X11 canvas children are hit-tested/raised by the Xwayland seat;
    // the synthetic canvas id has no Wayland ToplevelManager surface.
    if (winehua_x11_is_desktop(tl)) {
        winehua_display_focus_select(tl, true);
        return nullptr;
    }
    if (x11_toplevel_owns(tl)) {
        winehua_display_focus_select(tl, true);
        x11_toplevel_control(tl, 0, true);
        return nullptr;
    }
    WaylandServer::GetInstance()->RaiseToplevel(tl, true);
    return nullptr;
}

// Restore/raise/focus must execute on the Wayland event-loop thread.
static napi_value BindExternalToplevel(napi_env env, napi_callback_info info) {
    size_t count = 8; napi_value args[8], result; int32_t values[8] = {};
    napi_get_cb_info(env, info, &count, args, nullptr, nullptr);
    bool valid = count == 8;
    for (size_t i = 0; valid && i < count; ++i)
        valid = napi_get_value_int32(env, args[i], &values[i]) == napi_ok;
    bool bound = valid && values[0] > 0 && values[1] > 0 && values[2] > 0 && values[3] > 0 &&
        (winehua_x11_host_owns(values[1])
            ? winehua_x11_bind_external(values[0], values[1], values[2], values[3], values[4], values[5], values[6], values[7])
            : WaylandServer::GetInstance()->GetDesktopCompositor().BindExternalToplevel(
                values[0], values[1], values[2], values[3], values[4], values[5], values[6], values[7]));
    napi_get_boolean(env, bound, &result); return result;
}

static napi_value GetKeyboardFocusedToplevel(napi_env env, napi_callback_info) {
    napi_value result;
    napi_create_uint32(env, InputManager::GetInstance()->GetKeyboardFocusedToplevel(), &result);
    return result;
}

static napi_value MoveExternalHost(napi_env env, napi_callback_info info) {
    size_t count = 3; napi_value args[3], result; uint32_t source = 0, host = 0, edges = 0;
    napi_get_cb_info(env, info, &count, args, nullptr, nullptr);
    bool accepted = (count == 2 || count == 3) && napi_get_value_uint32(env, args[0], &source) == napi_ok &&
        napi_get_value_uint32(env, args[1], &host) == napi_ok &&
        (count == 2 || napi_get_value_uint32(env, args[2], &edges) == napi_ok) &&
        InputManager::GetInstance()->RequestExternalHostMove(source, host, edges);
    napi_get_boolean(env, accepted, &result); return result;
}

static napi_value IsExternalHostResizing(napi_env env, napi_callback_info info) {
    size_t count = 1; napi_value arg, result; uint32_t host = 0;
    napi_get_cb_info(env, info, &count, &arg, nullptr, nullptr);
    napi_get_boolean(env, count == 1 && napi_get_value_uint32(env, arg, &host) == napi_ok &&
        InputManager::GetInstance()->IsExternalHostResizing(host), &result);
    return result;
}

static napi_value UnbindExternalToplevel(napi_env env, napi_callback_info info) {
    size_t count = 1; napi_value args[1]; uint32_t id = 0;
    napi_get_cb_info(env, info, &count, args, nullptr, nullptr);
    if (count == 1 && napi_get_value_uint32(env, args[0], &id) == napi_ok) {
        winehua_x11_unbind_external(id);
        WaylandServer::GetInstance()->GetDesktopCompositor().UnbindExternalToplevel(id);
    }
    return nullptr;
}

static napi_value ActivateToplevel(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];
    uint32_t id = 0;
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc == 1 && napi_get_value_uint32(env, args[0], &id) == napi_ok)
        if (x11_toplevel_owns(id)) { winehua_display_focus_select(id, true); x11_toplevel_control(id, 0, true); }
        else InputManager::GetInstance()->RequestToplevelActivation(id);
    return nullptr;
}

// Desktop 模式: 接收物理像素坐标 (px, py), 通过 viewport 映射为 Wine 逻辑坐标后查找
// resize 后 surface 和逻辑尺寸比例变化, 由 renderer viewport 保证映射正确
static napi_value FindToplevelAt(napi_env env, napi_callback_info info) {
    size_t argc = 3;
    napi_value args[3];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 2) return nullptr;
    int32_t px, py;  // 物理像素坐标
    napi_get_value_int32(env, args[0], &px);
    napi_get_value_int32(env, args[1], &py);

    // Optional presentation owner: an isolated game is absent from the root
    // scene and its physical input must stay in its own renderer coordinate space.
    uint32_t presentationId = 0;
    if (argc >= 3) napi_get_value_uint32(env, args[2], &presentationId);
    auto* ws = WaylandServer::GetInstance();
    if (presentationId && ws->GetDesktopCompositor().IsToplevelIsolated(presentationId)) {
        napi_value result;
        napi_create_uint32(env, ws->GetToplevelManager().GetSurfaceForToplevel(presentationId) ? presentationId : 0, &result);
        return result;
    }
    // The canvas uses X coordinates/scene hit-testing downstream. There is
    // deliberately no EglRenderer or Wayland surface for this synthetic id.
    // Never fall back to id=1: it may belong to another session/client.
    if (const uint32_t desktop = winehua_x11_desktop_id()) {
        napi_value result;
        napi_create_uint32(env, desktop, &result);
        return result;
    }
    uint32_t rootId = ws->GetDesktopRootToplevelId();
    wl_fixed_t wx, wy;
    InputManager::GetInstance()->CoordTransform(px, py, rootId > 0 ? rootId : 1, &wx, &wy);
    int32_t lx = wl_fixed_to_int(wx);
    int32_t ly = wl_fixed_to_int(wy);

    uint32_t id = ws->FindToplevelAt(lx, ly);
    napi_value result;
    napi_create_uint32(env, id, &result);
    return result;
}

static napi_value SetToplevelVisible(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value args[2];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 2) return nullptr;
    uint32_t tl; bool visible;
    napi_get_value_uint32(env, args[0], &tl);
    napi_get_value_bool(env, args[1], &visible);
    InputManager::GetInstance()->SetToplevelVisible(tl, visible);
    if (visible) {
        // 6A: 兼容别名 NotifyWindowRestored 已删, 直调语义方法 (同实现同值)
        WaylandServer::GetInstance()->SetToplevelRestored(tl);
    }
    return nullptr;
}

// -- NAPI: getProcessList — 返回运行中进程列表 --
static napi_value GetProcessList(napi_env env, napi_callback_info info) {
    auto snapshot = GetProcessListSnapshot();
    snapshot.erase(std::remove_if(snapshot.begin(), snapshot.end(),
        [](const WineProcessEntry& entry) { return !entry.running || entry.independentRuntime; }), snapshot.end());

    napi_value arr;
    napi_create_array_with_length(env, snapshot.size(), &arr);

    for (size_t i = 0; i < snapshot.size(); i++) {
        const auto& entry = snapshot[i];
        napi_value obj;
        napi_create_object(env, &obj);

        napi_value pidVal, nameVal, pathVal, stateVal, shellVal, sessionVal;
        napi_create_int32(env, entry.pid, &pidVal);
        napi_create_string_utf8(env, entry.exeBasename.c_str(), NAPI_AUTO_LENGTH, &nameVal);
        napi_create_string_utf8(env, entry.exeFullPath.c_str(), NAPI_AUTO_LENGTH, &pathVal);
        napi_create_string_utf8(env, entry.running ? "running" : "exited",
                                NAPI_AUTO_LENGTH, &stateVal);
        // 桌面 root 出现前加入的会话基础进程 (desktop + 桌面前的 explorer 等),
        // ArkTS 据此隐藏"结束"操作防误操作破坏桌面运行
        napi_get_boolean(env, entry.desktopShell, &shellVal);
        const auto sessionId = WineSessionId(entry);
        napi_create_string_utf8(env, sessionId.c_str(), sessionId.size(), &sessionVal);

        napi_property_descriptor props[] = {
            {"sessionId", nullptr, nullptr, nullptr, nullptr, sessionVal, napi_default, nullptr},
            {"pid",   nullptr, nullptr, nullptr, nullptr, pidVal,   napi_default, nullptr},
            {"name",  nullptr, nullptr, nullptr, nullptr, nameVal,  napi_default, nullptr},
            {"path",  nullptr, nullptr, nullptr, nullptr, pathVal,  napi_default, nullptr},
            {"state", nullptr, nullptr, nullptr, nullptr, stateVal, napi_default, nullptr},
            {"desktopShell", nullptr, nullptr, nullptr, nullptr, shellVal, napi_default, nullptr},
        };
        napi_define_properties(env, obj, sizeof(props)/sizeof(props[0]), props);
        napi_set_element(env, arr, i, obj);
    }

    if (winehua::FrameLoopDiagnosticState() & 1)
        OH_LOG_INFO(LOG_APP, "[NAPI] getProcessList returned %{public}zu processes", snapshot.size());
    return arr;
}

// -- NAPI: killProcess — 杀掉指定进程 --
static napi_value KillProcess(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 1) return nullptr;

    int32_t pid = 0;
    napi_get_value_int32(env, args[0], &pid);
    OH_LOG_WARN(LOG_APP, "[NAPI] killProcess pid=%{public}d", pid);

    WineProcessEntry entry;
    const bool stopped = QueryProcessSnapshot(pid, &entry) &&
        StopRegisteredWineSession(WineSessionId(entry));

    napi_value r;
    napi_get_boolean(env, stopped, &r);
    return r;
}

// -- 模块注册 --

// ================= VPP product surface (ported from VintagePomeloPro main) =================
static napi_value BooleanResult(napi_env env, bool value) {
    napi_value result;
    napi_get_boolean(env, value, &result);
    return result;
}

static bool SetHostGraphicsEnv(const char* key, std::string_view value) {
    // std::string_view does not guarantee a trailing NUL. Profiles currently
    // come from literals, but copying here keeps this boundary correct if a
    // generated or sliced profile is introduced later.
    const std::string stableValue(value);
    if (setenv(key, stableValue.c_str(), 1) == 0) return true;
    OH_LOG_ERROR(LOG_APP,
                 "[NAPI] graphics environment apply failed key=%{public}s errno=%{public}d",
                 key, errno);
    return false;
}


// VPP: legacy shadow-profile store (setHostShadowProfile interplay)
static std::string gLegacyHostShadowProfile;
std::string GetStringArgument(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1] = {};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 1) return "";
    size_t length = 0;
    napi_get_value_string_utf8(env, args[0], nullptr, 0, &length);
    std::string value(length + 1, '\0');
    napi_get_value_string_utf8(env, args[0], value.data(), value.size(), &length);
    value.resize(length);
    return value;
}

static bool ApplyHostGraphicsProfile(const winehua::HostGraphicsProfile& profile) {
    bool applied = true;
    applied &= SetHostGraphicsEnv("WINEHUA_GRAPHICS_PROFILE", profile.name);
    applied &= SetHostGraphicsEnv("WINEHUA_VENUS_PRESENT_MODE", profile.presentMode);
    applied &= SetHostGraphicsEnv("WINEHUA_VIRGL_HOST_PRESENT_MODE", profile.presentMode);
    applied &= SetHostGraphicsEnv("VKR_WINEHUA_SHADOW_FROM_HOST", profile.shadowMode);
    applied &= SetHostGraphicsEnv("VKR_WINEHUA_SHADOW_TRACE", profile.shadowSelector);
    applied &= SetHostGraphicsEnv("VKR_WINEHUA_SHADOW_MERGE_RANGES",
                                  profile.mergeShadowRanges ? "1" : "0");
    applied &= SetHostGraphicsEnv("VKR_WINEHUA_GPU_UPLOAD_WAIT",
                                  profile.waitGpuUpload ? "1" : "0");
    applied &= SetHostGraphicsEnv("VKR_WINEHUA_DESCRIPTOR_UPDATE_SERIALIZE",
                                  profile.serializeDescriptorUpdates ? "1" : "0");
    applied &= SetHostGraphicsEnv("VN_WINEHUA_DEFER_SHMEM_UNREF",
                                  profile.deferSharedMemoryUnref ? "1" : "0");
    /* Keep the App-side control plane separate from renderer environment.
     * Phone hosts run in this process, so virgl_child's derived renderer
     * settings must not change the profile observed by a later EnsureStarted. */
    applied &= SetHostGraphicsEnv("WINEHUA_VIRGL_HOST_SHADOW_MODE",
                                  profile.shadowMode);
    applied &= SetHostGraphicsEnv("WINEHUA_VIRGL_HOST_SHADOW_SELECTOR",
                                  profile.shadowSelector);
    applied &= SetHostGraphicsEnv("WINEHUA_VIRGL_HOST_SHADOW_MERGE_RANGES",
                                  profile.mergeShadowRanges ? "1" : "0");
    applied &= SetHostGraphicsEnv("WINEHUA_VIRGL_HOST_GPU_UPLOAD_WAIT",
                                  profile.waitGpuUpload ? "1" : "0");
    applied &= SetHostGraphicsEnv("WINEHUA_VIRGL_HOST_DESCRIPTOR_UPDATE_SERIALIZE",
                                  profile.serializeDescriptorUpdates ? "1" : "0");
    applied &= SetHostGraphicsEnv("WINEHUA_VIRGL_HOST_PERF_SUMMARY",
                                  profile.perfSummary ? "1" : "0");

    if (!applied) return false;

    const char* gpuUpload = profile.gpuUpload == winehua::GpuUploadPolicy::Disabled
        ? "0" : (profile.gpuUpload == winehua::GpuUploadPolicy::Cpu ? "cpu" : "auto");
    OH_LOG_INFO(LOG_APP,
                "[NAPI] graphics profile=%{public}s mode=%{public}s "
                "selector=%{public}s perf_summary=%{public}s "
                "gpu_upload=%{public}s upload_wait=%{public}s "
                "descriptor_serialize=%{public}s defer_shmem_unref=%{public}s",
                profile.name.data(), profile.shadowMode.data(),
                profile.shadowSelector.data(), profile.perfSummary ? "1" : "0",
                gpuUpload, profile.waitGpuUpload ? "1" : "0",
                profile.serializeDescriptorUpdates ? "1" : "0",
                profile.deferSharedMemoryUnref ? "1" : "0");
    return true;
}

static napi_value SetHostGraphicsBackend(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1] = {};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    char backendName[64] = {};
    if (argc >= 1)
        napi_get_value_string_utf8(env, args[0], backendName, sizeof(backendName), nullptr);

    const winehua::D3dBackendKind backend = winehua::ParseD3dBackend(backendName);
    winehua::ProductGraphicsPolicy policy;
    if (!winehua::ResolveProductGraphicsPolicy(backend, &policy)) {
        OH_LOG_ERROR(LOG_APP, "[NAPI] unknown graphics backend=%{public}s", backendName);
        return BooleanResult(env, false);
    }
    OH_LOG_INFO(LOG_APP,
                "[NAPI] graphics backend=%{public}s route=%{public}s transport=%{public}s verified=%{public}d preciseMaps=%{public}d",
                backendName, policy.route.data(), policy.guest.directVulkan ? "direct" : "venus",
                winehua::IsProductDirectVulkanVerified() ? 1 : 0,
                policy.guest.directPreciseMaps ? 1 : 0);
    const bool applied = ApplyHostGraphicsProfile(policy.host);
    if (applied) gLegacyHostShadowProfile.clear();
    return BooleanResult(env, applied);
}

static napi_value RunHostVulkanProbe(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value args[2] = {};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    uint64_t surfaceId = 0;
    bool lossless = false;
    char runId[128] = {};
    if (argc < 2 ||
        napi_get_value_bigint_uint64(env, args[0], &surfaceId, &lossless) != napi_ok || !lossless ||
        napi_get_value_string_utf8(env, args[1], runId, sizeof(runId), nullptr) != napi_ok) {
        napi_value result;
        napi_get_boolean(env, false, &result);
        return result;
    }
    bool started = StartHostVulkanProbe(surfaceId, runId);
    OH_LOG_INFO(LOG_APP, "[HostVulkan] start surface=%{public}llu run=%{public}s result=%{public}s",
                static_cast<unsigned long long>(surfaceId), runId, started ? "true" : "false");
    napi_value result;
    napi_get_boolean(env, started, &result);
    return result;
}

static napi_value StopHostVulkanProbeNapi(napi_env env, napi_callback_info) {
    StopHostVulkanProbe();
    napi_value result;
    napi_get_boolean(env, true, &result);
    return result;
}

static napi_value GetHostGpuNameNapi(napi_env env, napi_callback_info) {
    const std::string name = ProbeGpuDeviceName();
    napi_value result;
    napi_create_string_utf8(env, name.c_str(), NAPI_AUTO_LENGTH, &result);
    OH_LOG_INFO(LOG_APP, "[HostVulkan] gpuName=%{public}s", name.c_str());
    return result;
}

static napi_value RefreshRenderer(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 1) {
        OH_LOG_ERROR(LOG_APP, "[MW-NAPI] refreshRenderer: need toplevelId");
        return nullptr;
    }
    uint32_t tid = 0;
    napi_get_value_uint32(env, args[0], &tid);
    PluginManager::GetInstance()->RefreshRenderer(tid);
    return nullptr;
}

static napi_value SetRendererPaused(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value args[2];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 2) return nullptr;
    uint32_t tl; bool paused;
    napi_get_value_uint32(env, args[0], &tl);
    napi_get_value_bool(env, args[1], &paused);
    PluginManager::GetInstance()->SetRendererPaused(tl, paused);
    return nullptr;
}

static napi_value WineTextInputPreedit(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1] = { nullptr };
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    char text[4096] = {};
    if (argc >= 1 && args[0] != nullptr) {
        size_t len = 0;
        napi_get_value_string_utf8(env, args[0], text, sizeof(text), &len);
    }
    // 预上屏光标放在 UTF-8 末尾 (字节偏移), 避免把 JS UTF-16 下标误当字节数。
    int32_t end = static_cast<int32_t>(strlen(text));
    const bool delivered = TextInput::GetInstance()->SendPreeditString(text, 0, end);
    napi_value result;
    napi_get_boolean(env, delivered, &result);
    return result;
}

static napi_value WineTextInputCommit(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1] = { nullptr };
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    char text[4096] = {};
    if (argc >= 1 && args[0] != nullptr) {
        size_t len = 0;
        napi_get_value_string_utf8(env, args[0], text, sizeof(text), &len);
    }
    const bool x11 = winehua_display_focus_is_x11();
    if (x11) wl_ohos_input_post_text(text);
    const bool delivered = x11 || TextInput::GetInstance()->SendCommitString(text);
    napi_value result;
    napi_get_boolean(env, delivered, &result);
    return result;
}

static napi_value WineTextInputActive(napi_env env, napi_callback_info) {
    napi_value result;
    napi_get_boolean(env, winehua_display_focus_is_x11() ?
        xim_bridge_is_active() : TextInput::GetInstance()->IsActivated(), &result);
    return result;
}

static napi_value WineTextInputEnabled(napi_env env, napi_callback_info) {
    napi_value result;
    napi_get_boolean(env, winehua_display_focus_is_x11() ?
        xim_bridge_is_active() : TextInput::GetInstance()->IsEnabled(), &result);
    return result;
}

static napi_value WineTextInputSetArmed(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1] = { nullptr };
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    bool armed = false;
    if (argc >= 1 && args[0] != nullptr) napi_get_value_bool(env, args[0], &armed);
    xim_bridge_set_armed(armed);
    xim_bridge_set_activation_callback([](int active, int x, int y, int w, int h, void*) {
        if (winehua_display_focus_is_x11())
            TextInput::GetInstance()->NotifyExternalActivated(active != 0, x, y, w, h);
    }, nullptr);
    TextInput::GetInstance()->SetArmed(armed);
    if (!armed) TextInput::GetInstance()->RearmActivation();
    return nullptr;
}

static napi_value InitAppLog(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1] = { nullptr };
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    char path[1024] = {};
    if (argc >= 1 && args[0] != nullptr) {
        size_t len = 0;
        napi_get_value_string_utf8(env, args[0], path, sizeof(path), &len);
    }
    WineHuaLogInit(path);
    return nullptr;
}

static napi_value ClearNativeLog(napi_env env, napi_callback_info info) {
    WineHuaLogClear();
    return nullptr;
}

static napi_value SetFrameLoopDiagnosticsNapi(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];
    bool enabled = false;
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc != 1 || napi_get_value_bool(env, args[0], &enabled) != napi_ok) {
        napi_throw_type_error(env, nullptr, "setFrameLoopDiagnostics requires a boolean");
        return nullptr;
    }
    winehua::SetFrameLoopDiagnostics(enabled);
    OH_LOG_INFO(LOG_APP, "[FRAME-LOOP] diagnostics=%{public}s", enabled ? "on" : "off");
    napi_value result;
    napi_get_undefined(env, &result);
    return result;
}

static napi_value GetDisplayFps(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    uint32_t id = 0;
    if (argc >= 1) napi_get_value_uint32(env, args[0], &id);
    double fps = static_cast<double>(DisplayFpsRegistry::Instance().Get(id));
    // Renderers run in a separate process. Read only this window's recent
    // sample; a Steam/background window must not overwrite the game's HUD.
    if (!id) id = WaylandServer::GetInstance()->GetDesktopRootToplevelId();
    winehua::ReadDisplayedFpsSample(winehua::kDisplayedFpsBasePath, id,
                                   winehua::PerfNowUs(), fps);

    napi_value result;
    napi_create_double(env, fps, &result);
    return result;
}


// ---- Registered Proton process sessions ----
static std::string ReadWineSessionArgument(napi_env env, napi_callback_info info) {
    size_t argc = 1; napi_value arg;
    napi_get_cb_info(env, info, &argc, &arg, nullptr, nullptr);
    char id[128] = {};
    if (argc != 1 || napi_get_value_string_utf8(env, arg, id, sizeof(id), nullptr) != napi_ok) return {};
    return id;
}
static bool ResolveNativeWineSession(const std::string& id, WineProcessEntry& entry) {
    if (id == "desktop") {
        const auto root = WaylandServer::GetInstance()->GetDesktopRootToplevelId();
        if (!root) return false;
        entry = {}; entry.pid = 0; entry.toplevelId = root; entry.running = true;
        entry.desktopShell = true; entry.exeFullPath = "desktop";
        return true;
    }
    return QueryWineSession(id, &entry);
}
static napi_value GetWineSession(napi_env env, napi_callback_info info) {
    const auto id = ReadWineSessionArgument(env, info);
    WineProcessEntry entry;
    napi_value result;
    if (!ResolveNativeWineSession(id, entry)) { napi_get_null(env, &result); return result; }
    napi_create_object(env, &result);
    auto number = [&](const char* key, int32_t value) {
        napi_value v; napi_create_int32(env, value, &v); napi_set_named_property(env, result, key, v);
    };
    auto string = [&](const char* key, const std::string& value) {
        napi_value v; napi_create_string_utf8(env, value.c_str(), value.size(), &v);
        napi_set_named_property(env, result, key, v);
    };
    number("pid", entry.pid); number("toplevelId", entry.toplevelId);
    string("sessionId", id); string("path", entry.exeFullPath);
    string("state", entry.running ? "running" : "stopped");
    return result;
}
static napi_value StopWineSession(napi_env env, napi_callback_info info) {
    const auto id = ReadWineSessionArgument(env, info);
    const bool stopped = id != "desktop" && StopRegisteredWineSession(id);
    napi_value result; napi_get_boolean(env, stopped, &result); return result;
}
static napi_value ActivateWineSession(napi_env env, napi_callback_info info) {
    const auto id = ReadWineSessionArgument(env, info);
    WineProcessEntry entry;
    bool active = ResolveNativeWineSession(id, entry) && entry.toplevelId;
    if (active) {
        auto& manager = WaylandServer::GetInstance()->GetToplevelManager();
        { auto lock = manager.Lock(); active = manager.FindToplevelLocked(entry.toplevelId) != nullptr; }
        if (active) WaylandServer::GetInstance()->RaiseToplevel(entry.toplevelId, true);
    }
    napi_value result; napi_get_boolean(env, active, &result); return result;
}

EXTERN_C_START

static napi_value SetWineLogPixelsNapi(napi_env env, napi_callback_info info)
{
    size_t argc = 1;
    napi_value argv[1], result;
    double value = 0;
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    bool accepted = argc == 1 && napi_get_value_double(env, argv[0], &value) == napi_ok &&
        std::isfinite(value) && value >= 96 && value <= 480 && std::floor(value) == value &&
        SetWineLogPixels(static_cast<int>(value));
    napi_get_boolean(env, accepted, &result);
    return result;
}

static napi_value Init(napi_env env, napi_value exports) {
    RegisterSystemOpen(env, exports);
    RegisterRuntimeCompat(env, exports);
    OH_LOG_WARN(LOG_APP, "[MW-NAPI]  Init called, env=%{public}p", env);
    LogWineScheme("libentry.so (主进程)");

    // 注册 NCP 子进程退出回调 (最早时机, 无条件): 沙箱 /proc 对 NCP 进程
    // 不可见, 退出检测以系统回调为权威信号。手机 fork 模式注册后不触发
    // (fork 子进程不走 NCP), 空转无害 — 故无需按模式分流, 模式分支只留在
    // spawn 之后的判活/杀/等待操作里 (此时 IsForkBackend 已由 setPhoneMode
    // 正确置位)
    RegisterNcpExitCallback();
    if (!EnsureChildReaper())
        OH_LOG_ERROR(LOG_APP, "[ProcReg] child reaper initialization failed");

    napi_property_descriptor desc[] = {
        {"runDirectVulkanProbe", nullptr, winehua::direct::RunVulkanProbe,
             nullptr, nullptr, nullptr, napi_default, nullptr},
        {"runPhoneSharedBufferProbe", nullptr, winehua::direct::RunPhoneSharedBufferProbe,
            nullptr, nullptr, nullptr, napi_default, nullptr},
        {"probeProductDirectSupport", nullptr, winehua::direct::ProbeProductDirectSupport,
            nullptr, nullptr, nullptr, napi_default, nullptr},
        {"preparePhoneDirectForkServer", nullptr, winehua::direct::PreparePhoneDirectForkServer,
             nullptr, nullptr, nullptr, napi_default, nullptr},
        {"runDirectVulkanForkServerProbe", nullptr, winehua::direct::RunVulkanForkServerProbe,
         nullptr, nullptr, nullptr, napi_default, nullptr},
        {"runDirectVulkanInlineProbe", nullptr, winehua::direct::RunVulkanInlineProbe,
         nullptr, nullptr, nullptr, napi_default, nullptr},
        {"runDirectVulkanCreateProbe", nullptr, winehua::direct::RunVulkanCreateProbe,
          nullptr, nullptr, nullptr, napi_default, nullptr},
        {"runDirectVulkanSystemCreateProbe", nullptr, winehua::direct::RunVulkanSystemCreateProbe,
          nullptr, nullptr, nullptr, napi_default, nullptr},
        {"runDirectWineIpcProbe", nullptr, winehua::direct::RunWineIpcProbe,
          nullptr, nullptr, nullptr, napi_default, nullptr},
        {"runDirectWineBrokerIpcProbe", nullptr, winehua::direct::RunWineBrokerIpcProbe,
          nullptr, nullptr, nullptr, napi_default, nullptr},
        {"runDirectWineSurfaceIpcProbe", nullptr, winehua::direct::RunWineSurfaceIpcProbe,
          nullptr, nullptr, nullptr, napi_default, nullptr},
        {"runDirectSurfaceProbe", nullptr, winehua::direct::RunSurfaceProbe,
          nullptr, nullptr, nullptr, napi_default, nullptr},
        {"runDirectSurfaceAbortProbe", nullptr, winehua::direct::RunSurfaceAbortProbe,
          nullptr, nullptr, nullptr, napi_default, nullptr},
        {"runDirectGpuSurfaceProbe", nullptr, winehua::direct::RunGpuSurfaceProbe,
          nullptr, nullptr, nullptr, napi_default, nullptr},
        {"runDirectGpuImportProbe", nullptr, winehua::direct::RunGpuImportProbe,
          nullptr, nullptr, nullptr, napi_default, nullptr},
        {"runDirectGpuSampleProbe", nullptr, winehua::direct::RunGpuSampleProbe,
          nullptr, nullptr, nullptr, napi_default, nullptr},
        {"runDirectGpuFenceProbe", nullptr, winehua::direct::RunGpuFenceProbe,
          nullptr, nullptr, nullptr, napi_default, nullptr},
        {"runDirectGpuCompositeProbe", nullptr, winehua::direct::RunGpuCompositeProbe,
          nullptr, nullptr, nullptr, napi_default, nullptr},
        {"runDirectGpuCompositeResizeProbe", nullptr, winehua::direct::RunGpuCompositeResizeProbe,
          nullptr, nullptr, nullptr, napi_default, nullptr},
        {"runDirectGpuCompositeThroughputProbe", nullptr,
          winehua::direct::RunGpuCompositeThroughputProbe,
          nullptr, nullptr, nullptr, napi_default, nullptr},
        {"runDirectGpuProducerPipelineProbe", nullptr,
          winehua::direct::RunGpuProducerPipelineProbe,
          nullptr, nullptr, nullptr, napi_default, nullptr},
        {"runDirectGpuDualSlotProbe", nullptr,
          winehua::direct::RunGpuDualSlotProbe,
          nullptr, nullptr, nullptr, napi_default, nullptr},
        {"runDirectGpuDualSlotResizeProbe", nullptr,
          winehua::direct::RunGpuDualSlotResizeProbe,
          nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setDirectProbeSurfaceId", nullptr, winehua::direct::SetDirectProbeSurfaceId,
          nullptr, nullptr, nullptr, napi_default, nullptr},
        {"clearDirectProbeSurfaceId", nullptr, winehua::direct::ClearDirectProbeSurfaceId,
          nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setDirectProbeSurfaceSize", nullptr, winehua::direct::SetDirectProbeSurfaceSize,
          nullptr, nullptr, nullptr, napi_default, nullptr},
        {"takeDirectProbeResizeRequest", nullptr, winehua::direct::TakeDirectProbeResizeRequest,
          nullptr, nullptr, nullptr, napi_default, nullptr},
        {"runDirectOutputProbe", nullptr, winehua::direct::RunDirectOutputProbe,
          nullptr, nullptr, nullptr, napi_default, nullptr},
        {"startServer",    nullptr, StartServer,    nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setHostShadowProfile", nullptr, SetHostShadowProfile, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"getWineStartupState", nullptr, GetWineStartupState, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"launchClient",   nullptr, LaunchClient,   nullptr, nullptr, nullptr, napi_default, nullptr},
        {"stopClient",     nullptr, StopClient,     nullptr, nullptr, nullptr, napi_default, nullptr},
        {"stopAll",        nullptr, StopAll,        nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setStateCallback", nullptr, SetStateCallback, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setToplevelCallback", nullptr, SetToplevelCallback, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setImeCallback", nullptr, SetImeCallback, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"sendImeCommit", nullptr, SendImeCommit, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"sendImePreedit", nullptr, SendImePreedit, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"imeBackspace", nullptr, ImeBackspace, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"getCurrentToplevelId", nullptr, GetCurrentToplevelId, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setPendingToplevel", nullptr, SetPendingToplevel, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"cancelPendingToplevel", nullptr, CancelPendingToplevel, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"destroyToplevel", nullptr, DestroyToplevel, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"sendToplevelClose", nullptr, SendToplevelClose, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"runWineExe",     nullptr, RunWineExe,     nullptr, nullptr, nullptr, napi_default, nullptr},
        {"runWineProgram", nullptr, RunWineProgram, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"queryWineProcess", nullptr, QueryWineProcess, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"terminateWineProcess", nullptr, TerminateWineProcess, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"checkWinePrefix",nullptr, CheckWinePrefix,nullptr, nullptr, nullptr, napi_default, nullptr},
        {"resetWinePrefix",nullptr, ResetWinePrefix,nullptr, nullptr, nullptr, napi_default, nullptr},
        // surfaceId 驱动的渲染器管理 (XComponentController 回调)
        {"setToplevelIsolated", nullptr, SetToplevelIsolated, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"createRenderer",  nullptr, CreateRenderer,  nullptr, nullptr, nullptr, napi_default, nullptr},
        {"resizeRenderer",  nullptr, ResizeRenderer,  nullptr, nullptr, nullptr, napi_default, nullptr},
        {"destroyRenderer", nullptr, DestroyRenderer, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setOutputSize",   nullptr, SetOutputSize,   nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setWineLogPixels", nullptr, SetWineLogPixelsNapi, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setDisplayScale",  nullptr, SetDisplayScale,  nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setDesktopMode",   nullptr, SetDesktopMode,   nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setPhoneMode",     nullptr, SetPhoneMode,     nullptr, nullptr, nullptr, napi_default, nullptr},
        {"getDesktopRootId", nullptr, GetDesktopRootId, nullptr, nullptr, nullptr, napi_default, nullptr},
        // ArkTS input forwarding (unified InputManager path)
        {"sendPointerEvent", nullptr, SendPointerEvent, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"sendKeyEvent",     nullptr, SendKeyEvent,     nullptr, nullptr, nullptr, napi_default, nullptr},
        {"sendScrollEvent",   nullptr, SendScrollEvent,   nullptr, nullptr, nullptr, napi_default, nullptr},
        {"registerHostWindow", nullptr, RegisterHostWindow, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"dumpWindowLayout", nullptr, DumpWindowLayout, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setPointerLockCallback", nullptr, SetPointerLockCallback, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"notifyToplevelResize",nullptr,NotifyToplevelResize,nullptr, nullptr, nullptr, napi_default, nullptr},
        {"notifyToplevelResizeEnd",nullptr,NotifyToplevelResizeEnd,nullptr, nullptr, nullptr, napi_default, nullptr},
        {"findToplevelAt",   nullptr, FindToplevelAt,   nullptr, nullptr, nullptr, napi_default, nullptr},
        {"raiseToplevel",    nullptr, RaiseToplevel,    nullptr, nullptr, nullptr, napi_default, nullptr},
        {"activateToplevel", nullptr, ActivateToplevel, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"bindExternalToplevel", nullptr, BindExternalToplevel, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"moveExternalHost", nullptr, MoveExternalHost, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"isExternalHostResizing", nullptr, IsExternalHostResizing, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"getKeyboardFocusedToplevel", nullptr, GetKeyboardFocusedToplevel, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"unbindExternalToplevel", nullptr, UnbindExternalToplevel, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setToplevelVisible", nullptr, SetToplevelVisible, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"getProcessList",   nullptr, GetProcessList,   nullptr, nullptr, nullptr, napi_default, nullptr},
        {"killProcess",     nullptr, KillProcess,     nullptr, nullptr, nullptr, napi_default, nullptr},
        {"initGameController", nullptr, InitGameController, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"cleanupGameController", nullptr, CleanupGameController, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"isGamepadConnected", nullptr, IsGamepadConnected, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"getGamepadCount", nullptr, GetGamepadCount, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setGamepadButtonCallback", nullptr, SetGamepadButtonCallback, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setGamepadAxisCallback", nullptr, SetGamepadAxisCallback, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setGamepadDeviceCallback", nullptr, SetGamepadDeviceCallback, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setGamepadRumbleCallback", nullptr, SetGamepadRumbleCallback, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"controllerSetEnabled", nullptr, ControllerSetEnabled, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"controllerSetButton", nullptr, ControllerSetButton, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"controllerSetAxis", nullptr, ControllerSetAxis, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"controllerSetHat", nullptr, ControllerSetHat, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"controllerResetSource", nullptr, ControllerResetSource, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"controllerGetState", nullptr, ControllerGetState, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"controllerGetStateText", nullptr, ControllerGetStateText, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"controllerStartBridge", nullptr, ControllerStartBridge, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"controllerStopBridge", nullptr, ControllerStopBridge, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"controllerGetSocketPath", nullptr, ControllerGetSocketPath, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"controllerSetOutputMode", nullptr, ControllerSetOutputMode, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"controllerGetOutputMode", nullptr, ControllerGetOutputMode, nullptr, nullptr, nullptr, napi_default, nullptr},
        // ---- VPP product surface ----
        {"setHostGraphicsBackend", nullptr, SetHostGraphicsBackend, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"runHostVulkanProbe", nullptr, RunHostVulkanProbe, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"stopHostVulkanProbe", nullptr, StopHostVulkanProbeNapi, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"getHostGpuName", nullptr, GetHostGpuNameNapi, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"refreshRenderer", nullptr, RefreshRenderer, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setRendererPaused", nullptr, SetRendererPaused, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"wineTextInputPreedit", nullptr, WineTextInputPreedit, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"wineTextInputCommit", nullptr, WineTextInputCommit, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"wineTextInputActive", nullptr, WineTextInputActive, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"wineTextInputEnabled", nullptr, WineTextInputEnabled, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"wineTextInputFocusedToplevel", nullptr, WineTextInputFocusedToplevel, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"wineTextInputSetArmed", nullptr, WineTextInputSetArmed, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"initAppLog", nullptr, InitAppLog, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"clearNativeLog", nullptr, ClearNativeLog, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"getDisplayFps", nullptr, GetDisplayFps, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setFrameLoopDiagnostics", nullptr, SetFrameLoopDiagnosticsNapi, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"controllerSetStick", nullptr, ControllerSetStick, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"controllerSetTrigger", nullptr, ControllerSetTrigger, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"getWineSession", nullptr, GetWineSession, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"stopWineSession", nullptr, StopWineSession, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"activateWineSession", nullptr, ActivateWineSession, nullptr, nullptr, nullptr, napi_default, nullptr},
    };
    napi_define_properties(env, exports, sizeof(desc) / sizeof(desc[0]), desc);

    // surfaceId 架构: 不再使用 libraryname='entry', XComponent 通过
    // 自定义 Controller 回调拿到 surfaceId, 由 createRenderer/renderer 管理。
    // 不再需要保存 gEnv/gExports, 不再依赖 XComponent exports 对象。
    OH_LOG_WARN(LOG_APP, "[MW-NAPI] Init complete OK");
    return exports;
}
EXTERN_C_END

static napi_module demoModule = {
    .nm_version = 1,
    .nm_flags = 0,
    .nm_filename = nullptr,
    .nm_register_func = Init,
    .nm_modname = "entry",
    .nm_priv = nullptr,
    .reserved = {0},
};

extern "C" __attribute__((constructor)) void RegisterEntryModule() {
    napi_module_register(&demoModule);
}
