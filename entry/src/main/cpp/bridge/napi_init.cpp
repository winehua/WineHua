#include <napi/native_api.h>
#include "common/fs_utils.h"
#include "seam/registry.h"
#include "audio/audio_broker.h"
#include "audio_ipc_protocol.h"
#include "graphics/graphics_broker.h"
#include "wine/wine_constants.h"
#include "wine/wine_env.h"
#include "proc/broker.h"
#include "proc/wine_process.h"
#include "wine/wine_launch.h"
#include "wine/wine_exe.h"
#include "phone_adapter/phone_adapter.h"

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
#include <cmath>
#include <dlfcn.h>

#undef LOG_TAG
#undef LOG_DOMAIN
#define LOG_DOMAIN 0x0000
#define LOG_TAG "WL_NAPI"
#include <hilog/log.h>

// -- 全局状态 (NAPI 层, 被 wine_process / wine_launch 引用) --
napi_threadsafe_function gStateTsfn = nullptr;

namespace winehua {
bool ConfigureX11FrameProvider(bool enabled);
}
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

    return nullptr;
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
    p->prefixDir = WINE_PREFIX;
    if (argc >= 6) {
        char d3dBackend[64] = {};
        napi_get_value_string_utf8(env, args[5], d3dBackend, sizeof(d3dBackend), nullptr);
        if (!strcmp(d3dBackend, "wined3d") || !strncmp(d3dBackend, "dxvk_", 5) ||
            !strcmp(d3dBackend, "vkd3d_limited_500k"))
            p->d3dBackend = d3dBackend;
    }
    if (p->d3dBackend == "dxvk_modern_2_6")
        p->dxvkBackend = "dxvk_modern_2_6";
    if (argc >= 7) {
        char dxvkBackend[64] = {};
        napi_get_value_string_utf8(env, args[6], dxvkBackend, sizeof(dxvkBackend), nullptr);
        if (!strcmp(dxvkBackend, "dxvk_legacy") ||
            !strcmp(dxvkBackend, "dxvk_modern_2_6"))
            p->dxvkBackend = dxvkBackend;
    }
    if (argc >= 8) {
        // 设置页 "Wine 语言": 仅接受白名单值, 非法/缺省保持 zh_CN
        char wineLang[16] = {};
        napi_get_value_string_utf8(env, args[7], wineLang, sizeof(wineLang), nullptr);
        if (!strcmp(wineLang, "zh_CN") || !strcmp(wineLang, "en_US"))
            p->wineLang = wineLang;
    }
    if (argc >= 9) {
        uint32_t count = 0;
        bool valid = napi_get_array_length(env, args[8], &count) == napi_ok && count <= 6;
        std::vector<std::string> lines;
        for (uint32_t i = 0; valid && i < count; ++i) {
            napi_value item;
            size_t length = 0;
            valid = napi_get_element(env, args[8], i, &item) == napi_ok &&
                    napi_get_value_string_utf8(env, item, buf, sizeof(buf), &length) == napi_ok &&
                    length < sizeof(buf) - 1;
            if (valid) lines.emplace_back(buf, length);
        }
        if (!valid || !winehua::BuildWineDisplayEnvironment(lines, p->display)) {
            delete p;
            napi_throw_type_error(env, nullptr, "invalid HGS display environment");
            return nullptr;
        }
    }
    if (argc >= 10) {
        napi_valuetype type = napi_undefined;
        napi_typeof(env, args[9], &type);
        if (type != napi_undefined) {
            const auto dimension = [&](const char* name, int32_t& value) {
                napi_value field;
                double number;
                if (napi_get_named_property(env, args[9], name, &field) != napi_ok ||
                    napi_get_value_double(env, field, &number) != napi_ok ||
                    !std::isfinite(number) || number < 1 || number > 32767 || number != std::floor(number))
                    return false;
                value = static_cast<int32_t>(number);
                return true;
            };
            if (!p->display.x11 || type != napi_object ||
                !dimension("width", p->x11DesktopWidth) || !dimension("height", p->x11DesktopHeight)) {
                delete p;
                napi_throw_type_error(env, nullptr, "invalid X11 desktop size");
                return nullptr;
            }
        }
    }
    // 向后兼容: 旧调用未传 homeDir 时使用默认路径
    if (p->homeDir.empty()) {
        p->homeDir = "/storage/Users/currentUser/Download";
    }

    OH_LOG_WARN(LOG_APP,
                "[Launch] exe=%{public}s sock=%{public}s lib=%{public}s home=%{public}s prefix=%{public}s (async)",
                p->exePath.c_str(), p->sockPath.c_str(), p->libPath.c_str(), p->homeDir.c_str(),
                p->prefixDir.c_str());
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
    const std::string prefix = WINE_PREFIX;
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
    const char* prefix = WINE_PREFIX;
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
    hgs::seam::HostSessionControl().ResetSessionState();
    hgs::seam::HostSessionControl().ResetFirstFrame();
    return nullptr;
}

// -- NAPI: stopAll — 结束 Wine 会话，旧调用方可继续同时停止图形服务 --
static napi_value StopAll(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value arg;
    bool stopGraphics = true;
    napi_get_cb_info(env, info, &argc, &arg, nullptr, nullptr);
    if (argc && napi_get_value_bool(env, arg, &stopGraphics) != napi_ok) {
        napi_throw_type_error(env, nullptr, "stopGraphics must be a boolean");
        return nullptr;
    }
    winehua::GraphicsBroker::GetInstance().Stop();
    KillAllProcesses();
    SetBrokerDisplayEnvironment({});
    if (stopGraphics) hgs::seam::HostSessionControl().Stop();
    // 注册表进程退出后发 state:stopped，宿主继续自己的停止编排。
    NotifyWhenSessionDrained();
    return nullptr;
}






struct ImeEvent {
    int active;
    int x, y, w, h;
};

















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








// The Wine loader may ask the broker to create the real explorer.exe in another NCP.
static napi_value MatchX11DesktopProcess(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value args[2];
    int32_t launcher = 0, client = 0;
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    WineProcessEntry process{};
    const bool match = argc == 2 &&
        napi_get_value_int32(env, args[0], &launcher) == napi_ok && launcher > 0 &&
        napi_get_value_int32(env, args[1], &client) == napi_ok && client > 0 &&
        QueryProcessSnapshot(client, &process) && process.running &&
        (client == launcher || (process.requesterPid == launcher && process.exeBasename == "explorer.exe"));
    if (match) MarkDesktopShellProcesses();
    napi_value result;
    napi_get_boolean(env, match, &result);
    return result;
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

static napi_value ConfigureX11FrameProvider(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    bool enabled = false;
    const bool ok = argc == 1 && napi_get_value_bool(env, args[0], &enabled) == napi_ok &&
        winehua::ConfigureX11FrameProvider(enabled);
    napi_value result;
    napi_get_boolean(env, ok, &result);
    return result;
}

// -- 模块注册 --
EXTERN_C_START
static napi_value Init(napi_env env, napi_value exports) {
    OH_LOG_WARN(LOG_APP, "[MW-NAPI]  Init called, env=%{public}p", env);

    // 注册 NCP 子进程退出回调 (最早时机, 无条件): 沙箱 /proc 对 NCP 进程
    // 不可见, 退出检测以系统回调为权威信号。手机 fork 模式注册后不触发
    // (fork 子进程不走 NCP), 空转无害 — 故无需按模式分流, 模式分支只留在
    // spawn 之后的判活/杀/等待操作里 (此时 IsForkBackend 已由 setPhoneMode
    // 正确置位)
    RegisterNcpExitCallback();

    napi_property_descriptor desc[] = {
        {"setHostShadowProfile", nullptr, SetHostShadowProfile, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"configureX11FrameProvider", nullptr, ConfigureX11FrameProvider, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"matchX11DesktopProcess", nullptr, MatchX11DesktopProcess, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"launchClient",   nullptr, LaunchClient,   nullptr, nullptr, nullptr, napi_default, nullptr},
        {"stopClient",     nullptr, StopClient,     nullptr, nullptr, nullptr, napi_default, nullptr},
        {"stopAll",        nullptr, StopAll,        nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setStateCallback", nullptr, SetStateCallback, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"runWineExe",     nullptr, RunWineExe,     nullptr, nullptr, nullptr, napi_default, nullptr},
        {"runWineProgram", nullptr, RunWineProgram, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"queryWineProcess", nullptr, QueryWineProcess, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"terminateWineProcess", nullptr, TerminateWineProcess, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"checkWinePrefix",nullptr, CheckWinePrefix,nullptr, nullptr, nullptr, napi_default, nullptr},
        {"resetWinePrefix",nullptr, ResetWinePrefix,nullptr, nullptr, nullptr, napi_default, nullptr},
        // surfaceId 驱动的渲染器管理 (XComponentController 回调)
        {"setPhoneMode",     nullptr, SetPhoneMode,     nullptr, nullptr, nullptr, napi_default, nullptr},
        // ArkTS input forwarding (unified InputManager path)
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
