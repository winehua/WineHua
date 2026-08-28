#include "env_profiles.h"
#include "wine_env.h"

#include <cstdlib>
#include <cstring>

namespace winehua {

#ifdef __aarch64__
std::vector<std::string> FilterCompatLines(const std::string& compatEnvStr)
{
    std::vector<std::string> raw;
    std::string cur;
    for (const char c : compatEnvStr) {
        if (c == ';') {
            if (!cur.empty()) raw.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) raw.push_back(cur);
    std::vector<std::string> filtered;
    for (const std::string& line : raw) {
        if (line.rfind("BOX64_DYNAREC_", 0) != 0)
            continue;
        if (line.find('|') != std::string::npos || line.find('\n') != std::string::npos)
            continue;
        if (line.find('=') == std::string::npos)
            continue;
        filtered.push_back(line);
    }
    return filtered;
}

void AppendCompatEnvLines(std::vector<std::string>& env,
                          const std::string& compatEnvStr)
{
    for (const std::string& line : FilterCompatLines(compatEnvStr))
        UpsertEnvLine(env, line);
}
#endif // __aarch64__

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

void AppendStableDesktopDxvkEnv(std::vector<std::string>& env,
                                const std::vector<std::string>& probeBase,
                                const std::string& d3dBackend,
                                const std::string& dxvkBackend)
{
    const bool usesDxvkOverlay = d3dBackend.rfind("dxvk_", 0) == 0 ||
                                 d3dBackend == "vkd3d_limited_500k";
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
    const bool traceKeysEnabled = d3dBackend == "dxvk_modern_2_6" ||
        (d3dBackend == "vkd3d_limited_500k" && dxvkBackend == "dxvk_modern_2_6");
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
     * reaches this same overlay via SessionEnvPolicy.stableDesktopOverlay.
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
    // D3D overlay (受管 dxvk/vkd3d 运行时)
    if (!p.d3dBackend.empty())
        AppendD3dBackendEnv(env, p.d3dBackend, p.dxvkBackend, p.binDir);
    // 兼容模式全局档位: 压过基线; WEAKBARRIER=0 clamp 在 stable overlay 尾
    // 会再压回 — 档位不击穿 DXVK/desktop 约束
#ifdef __aarch64__
    AppendCompatEnvLines(env, p.compatEnvStr);
#endif
    // 桌面稳定化 overlay。probe 快照 = 到此处为止的 env (基线+D3D+compat),
    // 与旧实现探测 LaunchParams.envStrs 语义一致
    if (p.stableDesktopOverlay)
        AppendStableDesktopDxvkEnv(env, env, p.d3dBackend, p.dxvkBackend);
    if (p.desktopShellFlag)
        UpsertEnvLine(env, "WINEHUA_DESKTOP=shell");
    // per-app 覆盖最后写入, 优先级最高
    for (const std::string& line : p.extraEnv)
        UpsertEnvLine(env, line);
    return env;
}

} // namespace winehua
