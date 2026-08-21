#include <napi/native_api.h>
#include "fs_utils.h"
#include "wayland_server.h"
#include "plugin_manager.h"
#include "input_manager.h"
#include "pointer_extras.h"
#include "egl_renderer.h"
#include "audio_broker.h"
#include "audio_ipc_protocol.h"
#include "graphics_broker.h"
#include "wine_constants.h"
#include "wine_scheme.h"
#include "wine_env.h"
#include "wine_process.h"
#include "wine_launch.h"
#include "wine_exe.h"
#include "host_vulkan_probe.h"
#include "experiment_payload.h"
#include "phone_adapter/phone_adapter.h"
#include "text_input.h"

#include <unistd.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <dirent.h>
#include <fcntl.h>
#include <cstdlib>
#include <cstdio>
#include <cerrno>
#include <cstring>
#include <string>
#include <thread>
#include <atomic>
#include <algorithm>
#include <dlfcn.h>

#undef LOG_TAG
#undef LOG_DOMAIN
#define LOG_DOMAIN 0x0000
#define LOG_TAG "WL_NAPI"
#include <hilog/log.h>

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
    size_t argc = 10;
    napi_value args[10] = {};
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
    if (argc >= 6) napi_get_value_bool(env, args[5], &p->automationMode);
    p->prefixDir = WINE_PREFIX;
    if (argc >= 7) {
        char prefixMode[32] = {};
        napi_get_value_string_utf8(env, args[6], prefixMode, sizeof(prefixMode), nullptr);
        if (!strcmp(prefixMode, "clean")) p->prefixDir = WINE_SMOKE_PREFIX;
    }
    if (p->prefixDir == WINE_SMOKE_PREFIX && !p->automationMode) {
        // The clean prefix is reserved for isolated smoke and experiment
        // sessions. Never let a missing ArkTS boolean turn it into a desktop
        // session, which would start Explorer ahead of the requested test.
        OH_LOG_WARN(LOG_APP, "[Launch] clean prefix forces automation mode");
        p->automationMode = true;
    }
    if (argc >= 8) {
        char d3dBackend[64] = {};
        napi_get_value_string_utf8(env, args[7], d3dBackend, sizeof(d3dBackend), nullptr);
        if (!strcmp(d3dBackend, "wined3d") || !strncmp(d3dBackend, "dxvk_", 5) ||
            !strcmp(d3dBackend, "vkd3d_limited_500k"))
            p->d3dBackend = d3dBackend;
    }
    if (p->d3dBackend == "dxvk_modern_2_6")
        p->dxvkBackend = "dxvk_modern_2_6";
    if (argc >= 9) {
        char dxvkBackend[64] = {};
        napi_get_value_string_utf8(env, args[8], dxvkBackend, sizeof(dxvkBackend), nullptr);
        if (!strcmp(dxvkBackend, "dxvk_legacy") ||
            !strcmp(dxvkBackend, "dxvk_modern_2_6"))
            p->dxvkBackend = dxvkBackend;
    }
    if (argc >= 10) {
        // 设置页 "Wine 语言": 仅接受白名单值, 非法/缺省保持 zh_CN
        char wineLang[16] = {};
        napi_get_value_string_utf8(env, args[9], wineLang, sizeof(wineLang), nullptr);
        if (!strcmp(wineLang, "zh_CN") || !strcmp(wineLang, "en_US"))
            p->wineLang = wineLang;
    }
    // 向后兼容: 旧调用未传 homeDir 时使用默认路径
    if (p->homeDir.empty()) {
        p->homeDir = "/storage/Users/currentUser/Download";
    }

    OH_LOG_WARN(LOG_APP,
                "[Launch] exe=%{public}s sock=%{public}s lib=%{public}s home=%{public}s prefix=%{public}s automation=%{public}s (async)",
                p->exePath.c_str(), p->sockPath.c_str(), p->libPath.c_str(), p->homeDir.c_str(),
                p->prefixDir.c_str(), p->automationMode ? "true" : "false");
    OH_LOG_WARN(LOG_APP, "[Launch] desktop D3D=%{public}s DXVK=%{public}s lang=%{public}s",
                p->d3dBackend.c_str(), p->dxvkBackend.c_str(), p->wineLang.c_str());

    // 保证可执行
    if (access(p->exePath.c_str(), X_OK) != 0) chmod(p->exePath.c_str(), 0755);

    // 提取 sockDir, sockName, winehuaBin
    auto pos = p->sockPath.find_last_of('/');
    p->sockDir = (pos == std::string::npos) ? "/tmp" : p->sockPath.substr(0, pos);
    p->sockName = (pos == std::string::npos) ? p->sockPath : p->sockPath.substr(pos + 1);
    pos = p->exePath.find_last_of('/');
    p->winehuaBin = (pos != std::string::npos) ? p->exePath.substr(0, pos) : p->exePath;

    signal(SIGCHLD, sigchld_handler);

    // 启动后台线程: wineserver -> wineboot --init
    std::thread(LaunchThreadFunc, p).detach();

    OH_LOG_WARN(LOG_APP, "[Launch] background thread started, returning to JS");

    napi_value r;
    napi_create_int32(env, 0, &r);
    return r;
}

// -- NAPI: checkWinePrefix -- 检测 .wine 是否已完整初始化 --
static napi_value CheckWinePrefix(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1] = {};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    std::string prefix = WINE_PREFIX;
    if (argc >= 1) {
        char mode[32] = {};
        napi_get_value_string_utf8(env, args[0], mode, sizeof(mode), nullptr);
        if (!strcmp(mode, "clean")) prefix = WINE_SMOKE_PREFIX;
    }
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
static bool RmDir(const char* path) {
    DIR* d = opendir(path);
    if (!d) return errno == ENOENT;
    bool ok = true;
    dirent* e;
    while ((e = readdir(d))) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        std::string full = std::string(path) + "/" + e->d_name;
        struct stat st;
        if (lstat(full.c_str(), &st) == 0) {
            if (S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode)) {
                if (!RmDir(full.c_str())) ok = false;
            } else if (unlink(full.c_str()) != 0) {
                ok = false;
                OH_LOG_ERROR(LOG_APP, "[NAPI] unlink %{public}s failed: %{public}s",
                             full.c_str(), strerror(errno));
            }
        } else {
            ok = false;
            OH_LOG_ERROR(LOG_APP, "[NAPI] lstat %{public}s failed: %{public}s",
                         full.c_str(), strerror(errno));
        }
    }
    closedir(d);
    if (rmdir(path) != 0 && errno != ENOENT) {
        ok = false;
        OH_LOG_ERROR(LOG_APP, "[NAPI] rmdir %{public}s failed: %{public}s",
                     path, strerror(errno));
    }
    return ok;
}

static napi_value ResetWinePrefix(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1] = {};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    const char* prefix = WINE_PREFIX;
    if (argc >= 1) {
        char mode[32] = {};
        napi_get_value_string_utf8(env, args[0], mode, sizeof(mode), nullptr);
        if (!strcmp(mode, "clean")) prefix = WINE_SMOKE_PREFIX;
    }
    OH_LOG_WARN(LOG_APP, "[NAPI] resetWinePrefix called prefix=%{public}s", prefix);
    KillAllProcesses();
    bool ok = RmDir(prefix);
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

// -- NAPI: stageExperimentPayload --
// Import a verified test payload into C:\\smoke\\experiments only after
// validating every artifact hash. No product runtime directory is writable.
static napi_value StageExperimentPayloadNapi(napi_env env, napi_callback_info info) {
    size_t argc = 5;
    napi_value args[5] = {};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    bool ok = false;
    std::string message;
    char experimentId[96] = {};
    char prefixMode[32] = "reuse";
    char sourceUrl[512] = {};
    if (argc < 4 ||
        napi_get_value_string_utf8(env, args[0], experimentId, sizeof(experimentId), nullptr) != napi_ok ||
        napi_get_value_string_utf8(env, args[3], prefixMode, sizeof(prefixMode), nullptr) != napi_ok) {
        message = "invalid experiment staging arguments";
    } else {
        if (argc >= 5 &&
            napi_get_value_string_utf8(env, args[4], sourceUrl, sizeof(sourceUrl), nullptr) != napi_ok) {
            message = "invalid experiment source URL";
        }
        bool namesIsArray = false;
        bool hashesIsArray = false;
        uint32_t nameCount = 0;
        uint32_t hashCount = 0;
        napi_is_array(env, args[1], &namesIsArray);
        napi_is_array(env, args[2], &hashesIsArray);
        if (namesIsArray && hashesIsArray) {
            napi_get_array_length(env, args[1], &nameCount);
            napi_get_array_length(env, args[2], &hashCount);
        }
        std::vector<winehua::ExperimentArtifact> artifacts;
        if (!namesIsArray || !hashesIsArray || nameCount != hashCount || nameCount == 0 || nameCount > 16) {
            message = "invalid experiment artifact list";
        } else {
            artifacts.reserve(nameCount);
            for (uint32_t index = 0; index < nameCount; ++index) {
                napi_value nameValue = nullptr;
                napi_value hashValue = nullptr;
                char name[128] = {};
                char hash[96] = {};
                if (napi_get_element(env, args[1], index, &nameValue) != napi_ok ||
                    napi_get_element(env, args[2], index, &hashValue) != napi_ok ||
                    napi_get_value_string_utf8(env, nameValue, name, sizeof(name), nullptr) != napi_ok ||
                    napi_get_value_string_utf8(env, hashValue, hash, sizeof(hash), nullptr) != napi_ok) {
                    message = "invalid experiment artifact item";
                    artifacts.clear();
                    break;
                }
                artifacts.push_back({name, hash});
            }
            if (!artifacts.empty() && message.empty())
                ok = winehua::StageExperimentPayload(experimentId, artifacts, prefixMode, sourceUrl, &message);
        }
    }
    OH_LOG_INFO(LOG_APP, "[Experiment] staging id=%{public}s result=%{public}s message=%{public}s",
                experimentId, ok ? "PASS" : "FAIL", message.c_str());
    napi_value result;
    napi_get_boolean(env, ok, &result);
    return result;
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


// -- NAPI: stopClient — 杀掉所有 Wine 进程 --
static napi_value StopClient(napi_env, napi_callback_info) {
    /* Stop the VirGL renderer before reaping its Wine descendants.  The
     * renderer is itself an app-owned NCP child; killing the whole process
     * tree first can bypass its bounded shutdown and leave a stale Venus ring
     * across the next isolated session. */
    winehua::GraphicsBroker::GetInstance().Stop();
    KillAllProcesses();
    // 会话终结统一收口 (与桌面退出同路径): 杀进程后进程级一次性状态全部
    // 复位, 下次引擎启动从冷启动基线开始 (StopAll 走 WaylandServer::Stop
    // 全量重建, 无需这里处理)
    WaylandServer::GetInstance()->ResetSessionState();
    WaylandServer::GetInstance()->ResetFirstFrame();
    return nullptr;
}

// -- NAPI: stopAll — 杀掉所有 Wine 进程 (含主 wineserver) + 停 Wayland server --
static napi_value StopAll(napi_env, napi_callback_info) {
    winehua::GraphicsBroker::GetInstance().Stop();
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

// -- NAPI: sendImeCommit -- (ArkTS inputMethod insertText → Wine commit_string)
static napi_value SendImeCommit(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    size_t len = 0;
    napi_get_value_string_utf8(env, args[0], nullptr, 0, &len);
    std::string text(len, '\0');
    napi_get_value_string_utf8(env, args[0], &text[0], len + 1, &len);
    TextInput::GetInstance()->SendCommitString(text.c_str());
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
    TextInput::GetInstance()->SendPreeditString(text.c_str(), b, e);
    return nullptr;
}

// -- NAPI: imeBackspace -- (软键盘退格 → Wine KEY_BACKSPACE 按键注入;
//    Wine 的 delete_surrounding_text 是空实现, 退格走现有 key 注入链路)
static napi_value ImeBackspace(napi_env env, napi_callback_info info) {
    (void)env;
    (void)info;
    constexpr uint32_t KEY_BACKSPACE = 14;
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
    PluginManager::GetInstance()->DestroyToplevel(id);
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
    WaylandServer::GetInstance()->SendToplevelClose(id);
    return nullptr;
}

// -- NAPI: createRenderer -- (XComponentController.onSurfaceCreated 调用)
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
    PluginManager::GetInstance()->CreateRenderer(tid, surfaceId);
    return nullptr;
}

// -- NAPI: resizeRenderer -- (XComponentController.onSurfaceChanged 调用)
static napi_value ResizeRenderer(napi_env env, napi_callback_info info) {
    size_t argc = 3;
    napi_value args[3];
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
    PluginManager::GetInstance()->ResizeRenderer(tid, w, h);
    return nullptr;
}

// -- NAPI: destroyRenderer -- (XComponentController.onSurfaceDestroyed 调用)
static napi_value DestroyRenderer(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    uint32_t tid = 0;
    if (argc >= 1) {
        napi_get_value_uint32(env, args[0], &tid);
    }
    OH_LOG_INFO(LOG_APP, "[MW-NAPI] destroyRenderer tl=%{public}u", tid);
    PluginManager::GetInstance()->DestroyToplevel(tid);
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
    uint32_t id = WaylandServer::GetInstance()->GetDesktopRootToplevelId();
    napi_value r;
    napi_create_uint32(env, id, &r);
    return r;
}

// -- NAPI: takeWindowMask -- (ARGB 异型窗口剪影掩码, ArkTS 轮询拉取)
static napi_value TakeWindowMask(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    uint32_t id = 0;
    if (argc >= 1) {
        napi_get_value_uint32(env, args[0], &id);
    }
    int w = 0, h = 0;
    std::vector<uint8_t> bits;
    if (!WaylandServer::GetInstance()->TakeWindowMask(id, w, h, bits)) {
        return nullptr;
    }
    napi_value result, wv, hv, buf;
    napi_create_object(env, &result);
    napi_create_int32(env, w, &wv);
    napi_create_int32(env, h, &hv);
    void* data = nullptr;
    napi_create_arraybuffer(env, bits.size(), &data, &buf);
    if (data && !bits.empty()) {
        memcpy(data, bits.data(), bits.size());
    }
    napi_value wKey, hKey, bufKey;
    napi_create_string_utf8(env, "w", 1, &wKey);
    napi_create_string_utf8(env, "h", 1, &hKey);
    napi_create_string_utf8(env, "buffer", 6, &bufKey);
    napi_set_property(env, result, wKey, wv);
    napi_set_property(env, result, hKey, hv);
    napi_set_property(env, result, bufKey, buf);
    return result;
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
    if (action != 1) {  // 跳过 MOVE (高频), 只记录 button/enter/leave
        OH_LOG_INFO(LOG_APP, "[PIPE] ptr tl=%{public}u a=%{public}d btn=0x%{public}x "
                    "px=(%{public}.0f,%{public}.0f)",
                    tl, action, button, px, py);
    }
    InputManager::GetInstance()->SendPointerEvent(tl, action, px, py, button, rawDx, rawDy, fromMouse);
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
static napi_threadsafe_function gPointerLockTsfn = nullptr;
static void CallJsPointerLock(napi_env env, napi_value cb, void*, void* data) {
    if (env && cb) {
        napi_value undef, arg;
        napi_get_undefined(env, &undef);
        napi_get_boolean(env, reinterpret_cast<uintptr_t>(data) != 0, &arg);
        napi_call_function(env, undef, cb, 1, &arg, nullptr);
    }
}
static napi_value SetPointerLockCallback(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 1) return nullptr;
    if (gPointerLockTsfn) {
        napi_release_threadsafe_function(gPointerLockTsfn, napi_tsfn_release);
        gPointerLockTsfn = nullptr;
    }
    napi_value name;
    napi_create_string_utf8(env, "WLPointerLock", NAPI_AUTO_LENGTH, &name);
    napi_create_threadsafe_function(env, args[0], nullptr, name,
                                     0, 1, nullptr, nullptr, nullptr, CallJsPointerLock,
                                     &gPointerLockTsfn);
    PointerExtras::GetInstance()->SetPointerLockCallback([](bool locked) {
        if (gPointerLockTsfn) {
            napi_call_threadsafe_function(gPointerLockTsfn,
                reinterpret_cast<void*>(static_cast<uintptr_t>(locked ? 1 : 0)),
                napi_tsfn_blocking);
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
    InputManager::GetInstance()->SendKeyEvent(tl, evdevCode, pressed);
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
    InputManager::GetInstance()->SendScrollEvent(tl, axis, value, scrollStep, px, py);
    return nullptr;
}

static napi_value NotifyToplevelResize(napi_env env, napi_callback_info info) {
    size_t argc = 3;
    napi_value args[3];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 3) return nullptr;
    uint32_t tl; int32_t w, h;
    napi_get_value_uint32(env, args[0], &tl);
    napi_get_value_int32(env, args[1], &w);
    napi_get_value_int32(env, args[2], &h);
    OH_LOG_INFO(LOG_APP, "[NAPI] notifyToplevelResize tl=%{public}u %{public}dx%{public}d",
                tl, w, h);
    WaylandServer::GetInstance()->NotifyToplevelResize(tl, w, h);
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
    WaylandServer::GetInstance()->RaiseToplevel(tl, true);
    return nullptr;
}

// Desktop 模式: 接收物理像素坐标 (px, py), 通过 viewport 映射为 Wine 逻辑坐标后查找
// resize 后 surface 和逻辑尺寸比例变化, 由 renderer viewport 保证映射正确
static napi_value FindToplevelAt(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value args[2];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 2) return nullptr;
    int32_t px, py;  // 物理像素坐标
    napi_get_value_int32(env, args[0], &px);
    napi_get_value_int32(env, args[1], &py);

    auto* ws = WaylandServer::GetInstance();
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
        WaylandServer::GetInstance()->NotifyWindowRestored(tl);
    }
    return nullptr;
}

// -- NAPI: getProcessList — 返回运行中进程列表 --
static napi_value GetProcessList(napi_env env, napi_callback_info info) {
    auto snapshot = GetProcessListSnapshot();
    snapshot.erase(std::remove_if(snapshot.begin(), snapshot.end(),
        [](const WineProcessEntry& entry) { return !entry.running; }), snapshot.end());

    napi_value arr;
    napi_create_array_with_length(env, snapshot.size(), &arr);

    for (size_t i = 0; i < snapshot.size(); i++) {
        const auto& entry = snapshot[i];
        napi_value obj;
        napi_create_object(env, &obj);

        napi_value pidVal, nameVal, pathVal, stateVal, shellVal;
        napi_create_int32(env, entry.pid, &pidVal);
        napi_create_string_utf8(env, entry.exeBasename.c_str(), NAPI_AUTO_LENGTH, &nameVal);
        napi_create_string_utf8(env, entry.exeFullPath.c_str(), NAPI_AUTO_LENGTH, &pathVal);
        napi_create_string_utf8(env, entry.running ? "running" : "exited",
                                NAPI_AUTO_LENGTH, &stateVal);
        // 桌面 root 出现前加入的会话基础进程 (desktop + 桌面前的 explorer 等),
        // ArkTS 据此隐藏"结束"操作防误操作破坏桌面运行
        napi_get_boolean(env, entry.desktopShell, &shellVal);

        napi_property_descriptor props[] = {
            {"pid",   nullptr, nullptr, nullptr, nullptr, pidVal,   napi_default, nullptr},
            {"name",  nullptr, nullptr, nullptr, nullptr, nameVal,  napi_default, nullptr},
            {"path",  nullptr, nullptr, nullptr, nullptr, pathVal,  napi_default, nullptr},
            {"state", nullptr, nullptr, nullptr, nullptr, stateVal, napi_default, nullptr},
            {"desktopShell", nullptr, nullptr, nullptr, nullptr, shellVal, napi_default, nullptr},
        };
        napi_define_properties(env, obj, sizeof(props)/sizeof(props[0]), props);
        napi_set_element(env, arr, i, obj);
    }

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

    KillChildProcess(pid);
    RemoveProcess(pid);

    napi_value r;
    napi_get_boolean(env, true, &r);
    return r;
}

// -- 模块注册 --
EXTERN_C_START
static napi_value Init(napi_env env, napi_value exports) {
    OH_LOG_WARN(LOG_APP, "[MW-NAPI]  Init called, env=%{public}p", env);
    LogWineScheme("libentry.so (主进程)");

    // 注册 NCP 子进程退出回调 (最早时机, 无条件): 沙箱 /proc 对 NCP 进程
    // 不可见, 退出检测以系统回调为权威信号。手机 fork 模式注册后不触发
    // (fork 子进程不走 NCP), 空转无害 — 故无需按模式分流, 模式分支只留在
    // spawn 之后的判活/杀/等待操作里 (此时 IsForkBackend 已由 setPhoneMode
    // 正确置位)
    RegisterNcpExitCallback();

    napi_property_descriptor desc[] = {
        {"startServer",    nullptr, StartServer,    nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setHostShadowProfile", nullptr, SetHostShadowProfile, nullptr, nullptr, nullptr, napi_default, nullptr},
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
        {"destroyToplevel", nullptr, DestroyToplevel, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"sendToplevelClose", nullptr, SendToplevelClose, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"runWineExe",     nullptr, RunWineExe,     nullptr, nullptr, nullptr, napi_default, nullptr},
        {"runWineProgram", nullptr, RunWineProgram, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"runGuestProgram", nullptr, RunGuestProgram, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"runHostProgram", nullptr, RunHostProgram, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"runHostReplay", nullptr, RunHostReplay, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"isHostReplayRunning", nullptr, IsHostReplayRunning, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"queryWineProcess", nullptr, QueryWineProcess, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"terminateWineProcess", nullptr, TerminateWineProcess, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"checkWinePrefix",nullptr, CheckWinePrefix,nullptr, nullptr, nullptr, napi_default, nullptr},
        {"resetWinePrefix",nullptr, ResetWinePrefix,nullptr, nullptr, nullptr, napi_default, nullptr},
        {"stageExperimentPayload", nullptr, StageExperimentPayloadNapi, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"runHostVulkanProbe", nullptr, RunHostVulkanProbe, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"stopHostVulkanProbe", nullptr, StopHostVulkanProbeNapi, nullptr, nullptr, nullptr, napi_default, nullptr},
        // surfaceId 驱动的渲染器管理 (XComponentController 回调)
        {"createRenderer",  nullptr, CreateRenderer,  nullptr, nullptr, nullptr, napi_default, nullptr},
        {"resizeRenderer",  nullptr, ResizeRenderer,  nullptr, nullptr, nullptr, napi_default, nullptr},
        {"destroyRenderer", nullptr, DestroyRenderer, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setOutputSize",   nullptr, SetOutputSize,   nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setDisplayScale",  nullptr, SetDisplayScale,  nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setDesktopMode",   nullptr, SetDesktopMode,   nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setPhoneMode",     nullptr, SetPhoneMode,     nullptr, nullptr, nullptr, napi_default, nullptr},
        {"getDesktopRootId", nullptr, GetDesktopRootId, nullptr, nullptr, nullptr, napi_default, nullptr},
        // ArkTS input forwarding (unified InputManager path)
        {"sendPointerEvent", nullptr, SendPointerEvent, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"sendKeyEvent",     nullptr, SendKeyEvent,     nullptr, nullptr, nullptr, napi_default, nullptr},
        {"sendScrollEvent",   nullptr, SendScrollEvent,   nullptr, nullptr, nullptr, napi_default, nullptr},
        {"registerHostWindow", nullptr, RegisterHostWindow, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setPointerLockCallback", nullptr, SetPointerLockCallback, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"notifyToplevelResize",nullptr,NotifyToplevelResize,nullptr, nullptr, nullptr, napi_default, nullptr},
        {"takeWindowMask", nullptr, TakeWindowMask, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"findToplevelAt",   nullptr, FindToplevelAt,   nullptr, nullptr, nullptr, napi_default, nullptr},
        {"raiseToplevel",    nullptr, RaiseToplevel,    nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setToplevelVisible", nullptr, SetToplevelVisible, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"getProcessList",   nullptr, GetProcessList,   nullptr, nullptr, nullptr, napi_default, nullptr},
        {"killProcess",     nullptr, KillProcess,     nullptr, nullptr, nullptr, napi_default, nullptr},
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
