#include "wine_env.h"
#include "wine_constants.h"
#include "audio/audio_broker.h"
#include "audio_ipc_protocol.h"
#include "env_spec.h"
#include "graphics/graphics_broker.h"
#include "compositor/wayland_server.h"

#include <unistd.h>
#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#undef LOG_TAG
#undef LOG_DOMAIN
#define LOG_DOMAIN 0x0000
#define LOG_TAG "WL_NAPI"
#include <hilog/log.h>

int CreateAudioBootstrapFd(const std::string& runtimeDir) {
    if (!winehua::AudioBroker::GetInstance().EnsureStarted(runtimeDir)) {
        OH_LOG_ERROR(LOG_APP, "[AudioBroker] failed to start for runtimeDir=%{public}s", runtimeDir.c_str());
        return -1;
    }
    int fd = winehua::AudioBroker::GetInstance().CreateBootstrapHandle();
    if (fd < 0) {
        OH_LOG_ERROR(LOG_APP, "[AudioBroker] failed to create bootstrap FD for runtimeDir=%{public}s", runtimeDir.c_str());
        return -1;
    }
    OH_LOG_INFO(LOG_APP, "[AudioBroker] bootstrap ready runtimeDir=%{public}s", runtimeDir.c_str());
    return fd;
}

std::vector<std::string> BuildWineEnv(const std::string& sockDir,
                                      const std::string& sockName,
                                      const std::string& libPath,
                                      const std::string& binDir,
                                      int audioBootstrapFd,
                                      const std::string& homeDir,
                                      const std::string& prefixDir,
                                      const std::string& wineLang) {
    std::string runtimeLibPath = binDir + ":" + binDir + "/x86_64-unix:" + binDir + "/../lib/x86_64";
    winehua::GraphicsBackendState graphicsState = winehua::GraphicsBroker::GetInstance().GetState();
    std::string guestReceiverLibDir;
    bool useGuestReceiverRuntime = graphicsState.active == winehua::GraphicsBackend::Virgl;

    if (useGuestReceiverRuntime && graphicsState.guestReceiverPresent && !graphicsState.guestReceiverRuntimeDir.empty()) {
        guestReceiverLibDir = graphicsState.guestReceiverRuntimeDir + "/lib";
        if (access(guestReceiverLibDir.c_str(), F_OK) == 0) {
            runtimeLibPath = guestReceiverLibDir + ":" + runtimeLibPath;
        }
    }

    // ==== Layer 0: 硬基线 ====
    // 分歧键 (合成器 socket 参数) 主进程侧自行给定; 公共键与子进程
    // setup_wine_env 同一张表 (wine_env_baseline.h), 增键只改一处。
    // NOTE: WINEDLLDIR0/1, WINEDLLPATH 在 DXVK 路径下会被 AppendD3dBackendEnv 覆盖
    std::vector<std::string> env = {
        "XDG_RUNTIME_DIR=" + sockDir,
        "WAYLAND_DISPLAY=" + sockName,
    };
    {
        std::vector<std::string> baseline = winehua::BuildWineBaselineLines(
            {binDir, homeDir, prefixDir});
        env.insert(env.end(), baseline.begin(), baseline.end());
    }
    // 仅主进程侧基线: locale / WINEDEBUG 静默 / GStreamer 插件路径
    // (子进程 WINEDEBUG 由 select_winedebug_profile 决定, 不走此表)
    env.push_back("WINEDEBUG=-all");
    env.push_back("LANG=" + wineLang + ".UTF-8");
    // OHOS musl 无 locale 数据, setlocale 激活失败返回 "C";
    // Wine 的 unix_to_win_locale 遇 "C" 只读 LC_ALL 兜底 (ntdll/unix/env.c),
    // 单设 LANG 无效, 必须补 LC_ALL 才能解析出对应 LCID (0x0804 zh-CN),
    // 与 LANG 同取设置页 wineLang (zh_CN/en_US)
    env.push_back("LC_ALL=" + wineLang + ".UTF-8");
    // winegstreamer 运行时加载 GStreamer 插件 (gst-plugins-base/good/libav)
    env.push_back("GST_PLUGIN_PATH=" + binDir + "/x86_64-unix/gstreamer-1.0");
    env.push_back("GST_PLUGIN_SYSTEM_PATH=" + binDir + "/x86_64-unix/gstreamer-1.0");
    // ==== Layer 1: Box64 性能调优 (仅 ARM64, 键值表见 wine_env_baseline.h) ====
    // NOTE: BOX64_DYNAREC_WEAKBARRIER=2 在桌面 DXVK 下会被 AppendStableDxvkEnv 覆盖为 0
    winehua::AppendBox64PerfStrings(env);
    // ==== Layer 2: 运行时库路径 ====
    // NOTE: BOX64_LD_LIBRARY_PATH (ARM64) 在 DXVK 路径下会被 AppendD3dBackendEnv 覆盖
#ifdef __aarch64__
    env.push_back("LD_LIBRARY_PATH=" + libPath);
    env.push_back("BOX64_LD_LIBRARY_PATH=" + runtimeLibPath);
#else
    env.push_back("LD_LIBRARY_PATH=" + runtimeLibPath);
#endif
    // ==== Layer 3: 音频 bootstrap (条件) ====
    if (audioBootstrapFd >= 0) {
        env.push_back("WINE_OHOS_AUDIO_ENABLE=1");
        env.push_back("WINE_OHOS_AUDIO_BOOTSTRAP_FD=" + std::to_string(audioBootstrapFd));
        env.push_back("WINE_OHOS_AUDIO_PROTOCOL_VERSION=" + std::to_string(WINEHUA_AUDIO_PROTOCOL_VERSION));
    }
    // ==== Layer 4: 桌面模式标记 ====
    winehua::GraphicsBroker::GetInstance().SetWineRuntimeBinaryDir(binDir);
    // 告知 winewayland.drv 当前是桌面模式还是独立窗口模式
    // NOTE: 桌面模式下 wine_child.cpp 也会通过 __winehua_desktop__ token 设置同值（冗余保险）
    env.push_back(std::string("WINEHUA_DESKTOP_MODE=") +
                  (WaylandServer::GetInstance()->IsDesktopMode() ? "1" : "0"));
    // WINEHUA_SIMULATE_RESOLUTION: win32u per-process 模拟 ChangeDisplaySettings
    // (记录游戏主动 CDS 请求的分辨率, 查询时返回 — DDraw 全屏游戏依赖)。
    // 仅 PC 多窗口模式注入: Pad 模拟桌面 (RootCompositing) 由合成器缩放绘制,
    // 不需要分辨率模拟。
    if (!WaylandServer::GetInstance()->IsDesktopMode())
        env.push_back("WINEHUA_SIMULATE_RESOLUTION=1");
    // 相对模式 enter 静默校准 (方向 A) 启用开关: wine 侧只在显式 "1" 时启用。
    // 默认注入 "1" (静默校准 = 默认行为, 三游戏验证通过); 排查问题时改 "0"
    // (或删除本行 = 未设置) 回退硬件绝对移动路径。
    env.push_back("WINEWAYLAND_ENTER_SILENT=1");
    // ==== Layer 5: 图形状态 ====
    // NOTE: BOX64_EMULATED_LIBS (ARM64) 在 DXVK 路径下会被 AppendD3dBackendEnv 覆盖
    winehua::GraphicsBroker::GetInstance().AppendWineEnv(env);

    OH_LOG_INFO(LOG_APP,
                "[WineEnv] backend=%{public}s guestMode=%{public}s guestLib=%{public}s runtimeLibPath=%{public}s",
                winehua::GraphicsBroker::BackendName(graphicsState.active),
                graphicsState.guestReceiverMode.empty() ? "stock-egl" : graphicsState.guestReceiverMode.c_str(),
                guestReceiverLibDir.empty() ? "(none)" : guestReceiverLibDir.c_str(),
                runtimeLibPath.c_str());
    return env;
}

void UpsertEnvLine(std::vector<std::string>& env, const std::string& line)
{
    const size_t sep = line.find('=');
    if (sep == std::string::npos || sep == 0) return;
    const std::string key = line.substr(0, sep);
    // 清理所有同 key 的旧条目, 然后追加新值 (与旧 UpsertEnv 行为一致,
    // 避免 AppendStableDxvkEnv 等 push_back 路径产生重复 key)
    env.erase(std::remove_if(env.begin(), env.end(), [&](const std::string& existing) {
        return existing.compare(0, key.size(), key) == 0 &&
               existing.size() > key.size() && existing[key.size()] == '=';
    }), env.end());
    env.push_back(line);
}

void AppendD3dBackendEnv(std::vector<std::string>& env,
                         const std::string& d3dBackend,
                         const std::string& dxvkBackend,
                         const std::string& binDir)
{
    if (d3dBackend == "vkd3d_limited_500k")
    {
        const bool modern26 = dxvkBackend == "dxvk_modern_2_6";
        const std::string dxvkRuntimeProfile = modern26 ? "modern-2.6" : "legacy";
        const std::string overlayRoot = std::string(WINE_RUNTIME_ROOT) +
            "/vkd3d/limited-500k";
        const std::string overlay64 = overlayRoot + "/x64";
        const std::string dxvkRoot = std::string(WINE_RUNTIME_ROOT) +
            "/dxvk/" + dxvkRuntimeProfile;
        const std::string dxvk64 = dxvkRoot + "/x64";
        const std::string dxvk86 = dxvkRoot + "/x86";
        const std::string guestVulkanRoot = binDir + "/guest_vulkan";
        const std::string guestVulkanLib = guestVulkanRoot + "/lib";
        const std::string guestVulkanIcd = guestVulkanRoot +
            "/share/vulkan/icd.d/venus_icd.x86_64.json";
        const std::string box64LibraryPath = guestVulkanLib + ":" +
            binDir + "/guest_gfx/lib:" + binDir + ":" +
            binDir + "/x86_64-unix:" + std::string(WINE_RUNTIME_ROOT) +
            "/lib/x86_64";
        /* Keep VKD3D first for d3d12, then the independently selected DXVK
         * overlays for d3d11/dxgi. The Wine loader gives both overlay
         * families priority over an application's private DLL directory. */
        const std::string wineDllPath = overlay64 + ":" + dxvk64 + ":" +
            dxvk86 + ":" + binDir + "/x86_64-windows:" +
            binDir + "/i386-windows:" + binDir;
        const std::vector<std::string> managed = {
            "WINEHUA_D3D_BACKEND=" + d3dBackend,
            "WINEHUA_VKD3D_ROOT=" + overlayRoot,
            "WINEHUA_VKD3D_PROFILE=limited-500k",
            "WINEHUA_VKD3D_VERSION=2.6",
            "WINEHUA_DXVK_ROOT=" + dxvkRoot,
            "WINEHUA_DXVK_PROFILE=" + dxvkRuntimeProfile,
            "WINEHUA_DXVK_VERSION=" + std::string(modern26 ? "2.6.2" : "1.10.3"),
            /* Product sessions use the qualified precise mapping contract
             * without enabling the Gate C trace selector. Direct fence waits
             * remain enabled explicitly below. */
            "WINEHUA_PERF_PROFILE=shadow-precise",
            "WINEHUA_VULKAN_RUNTIME=1",
            "WINEHUA_VULKAN_LOADER_ARCH=x86_64",
            "WINEHUA_VENUS_ICD_ARCH=x86_64",
#ifdef __aarch64__
            "USE_LIBBOX64=1",
            "BOX64_LD_LIBRARY_PATH=" + box64LibraryPath,
            "BOX64_EMULATED_LIBS=libvulkan.so:libvulkan.so.1:"
                "libEGL.so:libEGL.so.1:libGLESv2.so:libGLESv2.so.2:"
                "libGLESv1_CM.so:libGLESv1_CM.so.1:libGL.so:libGL.so.1:"
                "libwayland-client.so:libwayland-client.so.0:libwayland-server.so:"
                "libwayland-server.so.0:libwayland-egl.so:libwayland-egl.so.1:"
                "libdrm.so:libdrm.so.2:libffi.so:libffi.so.8:"
                "libglib-2.0.so:libglib-2.0.so.0:"
                "libgobject-2.0.so:libgobject-2.0.so.0:"
                "libgio-2.0.so:libgio-2.0.so.0:"
                "libgmodule-2.0.so:libgmodule-2.0.so.0:"
                "libgstreamer-1.0.so:libgstreamer-1.0.so.0:"
                "libgstbase-1.0.so:libgstbase-1.0.so.0:"
                "libgstvideo-1.0.so:libgstvideo-1.0.so.0:"
                "libgstaudio-1.0.so:libgstaudio-1.0.so.0:"
                "libgsttag-1.0.so:libgsttag-1.0.so.0:"
                "libgstpbutils-1.0.so:libgstpbutils-1.0.so.0:"
                "libgstallocators-1.0.so:libgstallocators-1.0.so.0:"
                "libgstapp-1.0.so:libgstapp-1.0.so.0:"
                "libgstcontroller-1.0.so:libgstcontroller-1.0.so.0:"
                "libgstfft-1.0.so:libgstfft-1.0.so.0:"
                "libgstnet-1.0.so:libgstnet-1.0.so.0:"
                "libgstriff-1.0.so:libgstriff-1.0.so.0:"
                "libgstrtp-1.0.so:libgstrtp-1.0.so.0:"
                "libgstrtsp-1.0.so:libgstrtsp-1.0.so.0:"
                "libgstsdp-1.0.so:libgstsdp-1.0.so.0:"
                "libgstcodecparsers-1.0.so:libgstcodecparsers-1.0.so.0:"
                "libgstmpegts-1.0.so:libgstmpegts-1.0.so.0:"
                "libxml2.so:libxml2.so.2:libz.so:libz.so.1",
            "BOX64_DYNAREC_WEAKBARRIER=0",
#endif
            "VK_DRIVER_FILES=" + guestVulkanIcd,
            "VK_ICD_FILENAMES=" + guestVulkanIcd,
            "VN_DEBUG=vtest",
            "VN_PERF=" + std::string(modern26
                ? "no_fence_feedback,no_query_feedback,no_semaphore_feedback,no_multi_ring"
                : "no_fence_feedback,no_query_feedback,no_multi_ring"),
            "VN_WINEHUA_STRONG_RING_BARRIER=1",
            /* [实验-8025复现] sync 开关取舍: 黑屏根因 = REMOTE_MEMORY_SYNC 是
             * dxvk/vkd3d 的唯一发布点 — mesa vn 显式 flush + Unmap 默认 no-op
             * (mesa/src/virtio/vulkan/vn_device_memory.c:619-666), 关掉则
             * host 侧 dirty list 为空, precise-dirty to-host 不拷 → 黑屏
             * (已实测, 80FPS 帧在跑画面黑)。下两行 + FORCE_COHERENT 为性能
             * 嫌疑: PERSISTENT 常驻映射强制发布 / DIRECT_FENCE 每帧阻塞往返,
             * 先关闭看帧率; 保留 r9 修复 (STRONG_RING_BARRIER/WEAKBARRIER)。 */
            "VN_WINEHUA_REMOTE_MEMORY_SYNC=1",
            "VKR_WINEHUA_SHADOW_FROM_HOST=precise",
            "WINEDLLOVERRIDES=d3d12=n;d3d11=n;dxgi=n",
            "WINEDLLPATH=" + wineDllPath,
            "WINEDLLDIR0=" + overlay64,
            "WINEDLLDIR1=" + dxvk64,
            "WINEDLLDIR2=" + dxvk86,
            /* ntdll stops scanning WINEDLLDIRn at the first missing index.
             * Keep Wine's PE runtime directories contiguous after the D3D
             * overlays so their imports can still resolve system DLLs. */
            "WINEDLLDIR3=" + binDir + "/x86_64-windows",
            "WINEDLLDIR4=" + binDir + "/i386-windows",
            "WINEDLLDIR5=" + binDir,
        };
        for (const std::string& line : managed) UpsertEnvLine(env, line);
        if (!modern26)
        {
            const std::vector<std::string> legacyCompatibility = {
                "WINEHUA_DXVK_RELAXED_FEATURES=1",
                "DXVK_WINEHUA_COMMAND_QUERY_RESET=1",
                "DXVK_WINEHUA_FLUSH_DYNAMIC_MAPPED=1",
                "DXVK_WINEHUA_EMULATE_RGBA8_SNORM_RT=auto",
                "DXVK_WINEHUA_BATCH_MAPPED_FLUSH=1",
            };
            for (const std::string& line : legacyCompatibility) UpsertEnvLine(env, line);
        }
        return;
    }
    if (d3dBackend.rfind("dxvk_", 0) != 0) return;

    std::string profile = d3dBackend.substr(strlen("dxvk_"));
    if (profile.empty()) profile = "legacy";
    const bool legacy = profile == "legacy";
    const bool modern26 = profile == "modern_2_6";
    if (!legacy && !modern26) return;
    const std::string runtimeProfile = modern26 ? "modern-2.6" : "legacy";
    const std::string overlayRoot = std::string(WINE_RUNTIME_ROOT) +
        "/dxvk/" + runtimeProfile;
    const std::string overlay64 = overlayRoot + "/x64";
    const std::string overlay86 = overlayRoot + "/x86";
    const std::string guestVulkanRoot = binDir + "/guest_vulkan";
    const std::string guestVulkanLib = guestVulkanRoot + "/lib";
    const std::string guestVulkanIcd = guestVulkanRoot +
        "/share/vulkan/icd.d/venus_icd.x86_64.json";
    const std::string box64LibraryPath = guestVulkanLib + ":" +
        binDir + "/guest_gfx/lib:" + binDir + ":" +
        binDir + "/x86_64-unix:" + std::string(WINE_RUNTIME_ROOT) + "/lib/x86_64";
    const std::string wineDllPath = overlay64 + ":" + overlay86 + ":" +
        binDir + "/x86_64-windows:" + binDir + "/i386-windows:" + binDir;

    const std::vector<std::string> managed = {
        "WINEHUA_D3D_BACKEND=" + d3dBackend,
        "WINEHUA_DXVK_ROOT=" + overlayRoot,
        "WINEHUA_DXVK_PROFILE=" + runtimeProfile,
        "WINEHUA_DXVK_VERSION=" + std::string(modern26 ? "2.6.2" : "1.10.3"),
        "WINEHUA_VULKAN_RUNTIME=1",
        "WINEHUA_VULKAN_LOADER_ARCH=x86_64",
        "WINEHUA_VENUS_ICD_ARCH=x86_64",
#ifdef __aarch64__
        "USE_LIBBOX64=1",
#endif
#ifdef __aarch64__
        "BOX64_LD_LIBRARY_PATH=" + box64LibraryPath,
        "BOX64_EMULATED_LIBS=libvulkan.so:libvulkan.so.1:"
            "libEGL.so:libEGL.so.1:libGLESv2.so:libGLESv2.so.2:"
            "libGLESv1_CM.so:libGLESv1_CM.so.1:libGL.so:libGL.so.1:"
            "libwayland-client.so:libwayland-client.so.0:libwayland-server.so:"
            "libwayland-server.so.0:libwayland-egl.so:libwayland-egl.so.1:"
            "libdrm.so:libdrm.so.2:libffi.so:libffi.so.8:"
            // GStreamer 链 (winegstreamer): glib + gst core/base + bad/ugly 依赖库
            "libglib-2.0.so:libglib-2.0.so.0:"
            "libgobject-2.0.so:libgobject-2.0.so.0:"
            "libgio-2.0.so:libgio-2.0.so.0:"
            "libgmodule-2.0.so:libgmodule-2.0.so.0:"
            "libgstreamer-1.0.so:libgstreamer-1.0.so.0:"
            "libgstbase-1.0.so:libgstbase-1.0.so.0:"
            "libgstvideo-1.0.so:libgstvideo-1.0.so.0:"
            "libgstaudio-1.0.so:libgstaudio-1.0.so.0:"
            "libgsttag-1.0.so:libgsttag-1.0.so.0:"
            "libgstpbutils-1.0.so:libgstpbutils-1.0.so.0:"
            "libgstallocators-1.0.so:libgstallocators-1.0.so.0:"
            "libgstapp-1.0.so:libgstapp-1.0.so.0:"
            "libgstcontroller-1.0.so:libgstcontroller-1.0.so.0:"
            "libgstfft-1.0.so:libgstfft-1.0.so.0:"
            "libgstnet-1.0.so:libgstnet-1.0.so.0:"
            "libgstriff-1.0.so:libgstriff-1.0.so.0:"
            "libgstrtp-1.0.so:libgstrtp-1.0.so.0:"
            "libgstrtsp-1.0.so:libgstrtsp-1.0.so.0:"
            "libgstsdp-1.0.so:libgstsdp-1.0.so.0:"
            // bad/ugly 插件依赖: videoparsersbad 需 codecparsers, mpegtsdemux 需 mpegts
            "libgstcodecparsers-1.0.so:libgstcodecparsers-1.0.so.0:"
            "libgstmpegts-1.0.so:libgstmpegts-1.0.so.0:"
            "libxml2.so:libxml2.so.2:libz.so:libz.so.1",
#endif
        "VK_DRIVER_FILES=" + guestVulkanIcd,
        "VK_ICD_FILENAMES=" + guestVulkanIcd,
        "VN_DEBUG=vtest",
        /* Host GPU writes to Venus feedback buffers are not automatically
         * visible through WineHua's explicit Guest/Host shadow mapping.
         * Query the real Host objects instead of polling stale Guest words. */
        /* This Guest Mesa/Host virglrenderer runtime uses WineHua's remote
         * shared-ring transport. Per-thread Venus rings can corrupt that
         * transport (the Host decoder observes an invalid command length),
         * so advertise the runtime capability here for every DXVK version.
         * Re-enable multi-ring only after a replacement Venus runtime passes
         * the x86/x64 command-stream qualification gate. */
        "VN_PERF=" + std::string(modern26
            ? "no_fence_feedback,no_query_feedback,no_semaphore_feedback,no_multi_ring"
            : "no_fence_feedback,no_query_feedback,no_multi_ring"),
        "WINEDLLOVERRIDES=d3d11=n;dxgi=n",
        "VN_WINEHUA_REMOTE_MEMORY_SYNC=1",
        "WINEDLLPATH=" + wineDllPath,
        "WINEDLLDIR0=" + overlay64,
        "WINEDLLDIR1=" + overlay86,
        /* Preserve the contiguous Wine PE runtime search path after the
         * selected DXVK overlays. */
        "WINEDLLDIR2=" + binDir + "/x86_64-windows",
        "WINEDLLDIR3=" + binDir + "/i386-windows",
        "WINEDLLDIR4=" + binDir,
    };
    for (const std::string& line : managed) UpsertEnvLine(env, line);
    if (!legacy) return;

    const std::vector<std::string> legacyCompatibility = {
        "WINEHUA_DXVK_RELAXED_FEATURES=1",
        "DXVK_WINEHUA_COMMAND_QUERY_RESET=1",
        "DXVK_WINEHUA_FLUSH_DYNAMIC_MAPPED=1",
        /* Prefer the native RGBA8 SNORM render-target path. On devices such
         * as Maleoon where sampling is supported but color attachment usage
         * is not, DXVK may substitute its qualified RGBA16F backing image. */
        "DXVK_WINEHUA_EMULATE_RGBA8_SNORM_RT=auto",
        "DXVK_WINEHUA_BATCH_MAPPED_FLUSH=1",
    };
    for (const std::string& line : legacyCompatibility) UpsertEnvLine(env, line);
}

// 迁移期 shim: 新代码请直接用 winehua::EnvSpec (env_spec.h)。
// 序列化规则 (fd 变量禁入 / 不可编码字符过滤) 已收口到 env_spec.cpp;
// fromLines 对同 key 行取最后值, 与子进程逐条 setenv 覆盖语义一致。
std::string SerializeEnvToEntryParams(const std::vector<std::string>& env) {
    return winehua::EnvSpec::fromLines(env).serializeEntryParams();
}

void LogGraphicsBackendStateForLaunch(const char* tag) {
    winehua::GraphicsBackendState state = winehua::GraphicsBroker::GetInstance().GetState();
    OH_LOG_INFO(LOG_APP,
                "[%{public}s] graphics requested=%{public}s active=%{public}s runtimeReady=%{public}s "
                "guestReceiver=%{public}s(%{public}s) virglSocketReady=%{public}s virglLibraryPresent=%{public}s",
                tag,
                winehua::GraphicsBroker::BackendName(state.requested),
                winehua::GraphicsBroker::BackendName(state.active),
                state.runtimeReady ? "true" : "false",
                state.guestReceiverPresent ? "true" : "false",
                state.guestReceiverMode.empty() ? "stock-egl" : state.guestReceiverMode.c_str(),
                state.virglSocketReady ? "true" : "false",
                state.virglLibraryPresent ? "true" : "false");
    if (!state.lastError.empty())
        OH_LOG_WARN(LOG_APP, "[%{public}s] graphics note: %{public}s", tag, state.lastError.c_str());
}
