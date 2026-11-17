#include "env_profiles.h"
#include "wine_env.h"
#include "dll_overrides.h"
#include "graphics/graphics_profile.h"

#include <cstdlib>
#include <cstdio>
#include <cstring>

namespace winehua {

// 兼容模式档位 (Box64Dynarec.ets) 仅随程序级 extraEnv (runWineProgram per-app
// environment) 注入, 不再与会话链 (wineserver/wineboot/explorer) 耦合 —
// 会话进程始终走出厂基线, 档位异常只影响对应程序, 不会卡死桌面启动。

// 旧 FindLaunchEnvironmentValue: 后写胜出, 反向扫描取最后一个匹配
static std::string FindEnvValue(const std::vector<std::string>& probeBase, const char* key)
{
    const std::string prefix = std::string(key) + "=";
    for (auto it = probeBase.rbegin(); it != probeBase.rend(); ++it) {
        if (it->rfind(prefix, 0) == 0)
            return it->substr(prefix.size());
    }
    return {};
}

void AppendStableDxvkEnv(std::vector<std::string>& env,
                                const std::vector<std::string>& probeBase,
                                const std::string& d3dBackend,
                                 const std::string&)
{
    const bool usesDxvkOverlay = d3dBackend == "dxvk_legacy_d3d9" ||
                                 d3dBackend == "dxvk_legacy" ||
                                 d3dBackend == "dxvk_modern_2_6";
    if (!usesDxvkOverlay) return;

    // 全部 probeBase 读取集中在写入之前 (允许调用方 env 与 probeBase 别名)
    /* SetHostShadowProfile carries the selected diagnostic profile through
     * the host-side broker environment before Explorer is launched.  Keep
     * the desktop descendants on that explicit profile instead of replacing
     * it with the product default below. */
    const char* shadowTrace = getenv("VKR_WINEHUA_SHADOW_TRACE");
    const bool guestPerf = shadowTrace && !strcmp(shadowTrace, "perf");
    std::string selectedProfile = FindEnvValue(probeBase, "WINEHUA_PERF_PROFILE");
    const std::string strongRing = FindEnvValue(probeBase, "VN_WINEHUA_STRONG_RING_BARRIER");
    const char* traceKeys[] = {
        "DXVK_WINEHUA_TRACE_SAMPLED",
        "DXVK_WINEHUA_TRACE_FLOW",
        "DXVK_WINEHUA_TRACE_API",
    };
    constexpr size_t kTraceKeyCount = sizeof(traceKeys) / sizeof(traceKeys[0]);
    std::string traceValues[kTraceKeyCount];
    const bool traceKeysEnabled = d3dBackend == "dxvk_modern_2_6";
    if (traceKeysEnabled) {
        for (size_t i = 0; i < kTraceKeyCount; ++i)
            traceValues[i] = FindEnvValue(probeBase, traceKeys[i]);
    }

    if (selectedProfile.empty()) {
        if (shadowTrace && !strcmp(shadowTrace, "inline-gpu-upload-frame-assoc-trace"))
            selectedProfile = "shadow-precise-dirty-ring-frame-assoc-trace";
        else if (shadowTrace && !strcmp(shadowTrace, "present-image-trace"))
            selectedProfile = "shadow-precise-dirty-ring-present-image-trace";
        else if (shadowTrace && !strcmp(shadowTrace, "gpu-frame-profile"))
            selectedProfile = "shadow-precise-dirty-ring-gpu-frame-profile";
        else if (shadowTrace && !strcmp(shadowTrace, "frame-timeline"))
            selectedProfile = "shadow-precise-dirty-ring-frame-timeline";
        else if (shadowTrace && !strcmp(shadowTrace, "inline-gpu-upload-descriptor-serialized"))
            selectedProfile = "shadow-precise-dirty-ring-inline-upload-descriptor-serialized";
        else if (shadowTrace && !strcmp(shadowTrace, "inline-gpu-upload-serialized"))
            selectedProfile = "shadow-precise-dirty-ring-inline-upload-serialized";
        else if (shadowTrace && !strcmp(shadowTrace, "inline-gpu-upload-alias-cover"))
            selectedProfile = "shadow-precise-dirty-ring-inline-upload-alias-cover";
        else if (shadowTrace && !strcmp(shadowTrace, "inline-gpu-upload-coverage-sort"))
            selectedProfile = "shadow-precise-dirty-ring-inline-upload-coverage-sort";
        else if (shadowTrace && !strcmp(shadowTrace, "inline-gpu-upload"))
            selectedProfile = "shadow-precise-dirty-ring-inline-upload";
        else if (shadowTrace && !strcmp(shadowTrace, "no-gpu-upload-fast"))
            selectedProfile = "shadow-precise-dirty-ring-no-upload-fast";
        else if (shadowTrace && !strcmp(shadowTrace, "no-gpu-upload"))
            selectedProfile = "shadow-precise-dirty-ring-no-upload";
        else
            selectedProfile = guestPerf ? "shadow-precise-strong-ring-perf"
                                        : "shadow-precise-dirty-ring-inline-upload-coverage-sort";
    }

    /* Explorer-launched programs inherit the desktop process environment and
     * never see runWineProgram's per-process overrides; runWineProgram itself
     * reaches this same overlay via SessionEnvPolicy.applyStableOverlay.
     * Keep the product-correct defaults here so both chains share one source. */
    /* Product sessions retain warnings and errors without formatting DXVK's
     * informational startup stream. Smoke and explicit diagnostics override
     * this through runWineProgram's per-process environment. */
    UpsertEnvLine(env, "DXVK_LOG_LEVEL=warn");
    UpsertEnvLine(env, "DXVK_LOG_PATH=C:\\windows\\temp");
#ifdef __aarch64__
    UpsertEnvLine(env, "BOX64_DYNAREC_WEAKBARRIER=0");
#endif
    UpsertEnvLine(env, "WINEHUA_PERF_PROFILE=" + selectedProfile);
    UpsertEnvLine(env, "DXVK_WINEHUA_PRECISE_SHADOW=1");
    if (selectedProfile == "shadow-precise-dirty-ring-inline-upload-descriptor-serialized") {
        UpsertEnvLine(env, "VKR_WINEHUA_DESCRIPTOR_UPDATE_SERIALIZE=1");
    }
    UpsertEnvLine(env, "VN_WINEHUA_STRONG_RING_BARRIER=" +
                  (strongRing.empty() ? "1" : strongRing));
    if (traceKeysEnabled) {
        for (size_t i = 0; i < kTraceKeyCount; ++i) {
            env.push_back(std::string(traceKeys[i]) + "=" +
                          (traceValues[i].empty() ? "0" : traceValues[i]));
        }
    }
    if (guestPerf) {
        UpsertEnvLine(env, "VN_WINEHUA_PERF_SUMMARY=1");
        UpsertEnvLine(env, "VN_WINEHUA_PERF_LOG=/storage/Users/currentUser/Download/app.hackeris.winehua/winehua_guest_ring_perf.log");
        /* vn_log uses MESA_LOG_DEBUG.  Raise only the explicit diagnostic
         * profile so the Guest ring summary survives the OHOS logger filter. */
        UpsertEnvLine(env, "MESA_LOG_LEVEL=debug");
    }
}

std::vector<std::string> BuildSessionEnv(const SessionEnvPolicy& p)
{
    // L0-L5: 基线 + Box64 档 + runtime libs + audio fd + 桌面标记 + graphics broker
    std::vector<std::string> env = BuildWineEnv(p.sockDir, p.sockName, p.libPath,
                                                p.binDir, p.audioBootstrapFd, p.homeDir,
                                                p.prefixDir, p.wineLang);
    // DXVK 1.10.3 is the default, DXVK 2.6.2 is opt-in, and WineD3D remains
    // available for VirGL. VKD3D artifacts may be packaged for future work,
    // but no caller can activate that overlay through this product path.
    const std::string d3dBackend = p.d3dBackend == "wined3d_zink" ||
        p.d3dBackend == "dxvk_legacy_d3d9" || p.d3dBackend == "wined3d" ||
        p.d3dBackend == "dxvk_modern_2_6" ? p.d3dBackend : "dxvk_legacy";
    const std::string dxvkBackend = d3dBackend == "dxvk_modern_2_6" ?
        "dxvk_modern_2_6" : "dxvk_legacy";
    if (!d3dBackend.empty())
        AppendD3dBackendEnv(env, d3dBackend, dxvkBackend, p.binDir);
    // 桌面稳定化 overlay。probe 快照 = 到此处为止的 env (基线+D3D),
    // 与旧实现探测 LaunchParams.envStrs 语义一致
    if (p.applyStableOverlay)
        AppendStableDxvkEnv(env, env, d3dBackend, dxvkBackend);
    if (p.desktopShellFlag)
        UpsertEnvLine(env, "WINEHUA_DESKTOP=shell");
    std::vector<std::string> graphicsEnvironment;
    const bool graphicsPolicyApplied = BuildProductGuestGraphicsEnvironment(
        ParseD3dBackend(d3dBackend), &graphicsEnvironment);
    if (graphicsPolicyApplied)
        for (const auto& line : graphicsEnvironment) UpsertEnvLine(env, line);
    // Direct Vulkan programs intentionally keep d3dBackend=wined3d, so the
    // D3D overlay above does not configure Venus.
    if (FindEnvValue(p.extraEnv, "WINEHUA_VULKAN_RUNTIME") == "1" &&
        d3dBackend != "dxvk_legacy_d3d9" && d3dBackend != "wined3d_zink" &&
        d3dBackend != "dxvk_legacy" && d3dBackend != "dxvk_modern_2_6")
        AppendVulkanRuntimeEnv(env, p.binDir);
    // Per-app overrides keep their original empty-value semantics. Display
    // identity belongs to the entire native Wine session, never to one app.
    for (const std::string& line : p.extraEnv) {
        if (line.rfind("WINEHUA_DISPLAY_ROUTE=", 0) == 0 ||
            line.rfind("WAYLAND_DISPLAY=", 0) == 0 || line.rfind("DISPLAY=", 0) == 0) continue;
        if (line.rfind("WINEHUA_D3D_BACKEND=", 0) == 0 ||
            line.rfind("WINEHUA_DXVK_ROOT=", 0) == 0 ||
            line.rfind("WINEHUA_DXVK_PROFILE=", 0) == 0 ||
            line.rfind("WINEHUA_DXVK_VERSION=", 0) == 0 ||
            line.rfind("WINEHUA_VKD3D_", 0) == 0)
            continue;
        if (line.rfind("WINEDLLOVERRIDES=", 0) == 0) {
            std::string merged;
            if (MergeDllOverrides(FindEnvValue(env, "WINEDLLOVERRIDES"),
                                  line.substr(std::strlen("WINEDLLOVERRIDES=")), merged))
                UpsertEnvLine(env, "WINEDLLOVERRIDES=" + merged);
            else
                std::fprintf(stderr, "[WineEnv] invalid DLL overrides; keeping previous rules\n");
            continue;
        }
        UpsertEnvLine(env, line);
    }
    if (graphicsPolicyApplied) {
        // Transport is immutable for the session: the system loader also
        // needs Direct producer IPC and the matching desktop consumer.
        for (const auto& line : graphicsEnvironment)
            if (line.rfind("WINEHUA_VULKAN_BACKEND=", 0) == 0 ||
                line.rfind("WINEHUA_GRAPHICS_PROFILE=", 0) == 0 ||
                line.rfind("WINEHUA_VK_PRECISE_MAP=", 0) == 0 ||
                line.rfind("DXVK_WINEHUA_PRECISE_SHADOW=", 0) == 0 ||
                line.rfind("DXVK_WINEHUA_FLUSH_DYNAMIC_MAPPED=", 0) == 0)
                UpsertEnvLine(env, line);
    }
    // Derive this after merging app rules; user environment cannot force the
    // bundled overlay when Wine was explicitly requested to load builtin.
    UpsertEnvLine(env, std::string("WINEHUA_DXVK9_OVERLAY=") +
        (d3dBackend == "dxvk_legacy_d3d9" &&
         NativeDllPreferred(FindEnvValue(env, "WINEDLLOVERRIDES"), "d3d9") ? "1" : "0"));
    AppendWineDpiEnv(env); // Per-app environment must not split the session DPI.
    return env;
}

} // namespace winehua
