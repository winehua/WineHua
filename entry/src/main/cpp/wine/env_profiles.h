#ifndef WINE_ENV_PROFILES_H
#define WINE_ENV_PROFILES_H

/**
 * env_profiles.h — 环境策略集中地 (重构第 3 步)
 *
 * 管线顺序只在此文件定义一次:
 *   BuildWineEnv (L0-L5 基线, wine_env.cpp)
 *   → AppendD3dBackendEnv (dxvk/vkd3d 受管 overlay, wine_env.cpp)
 *   → AppendStableDxvkEnv (桌面会话稳定化 overlay, 本文件, 可选)
 *   → WINEHUA_DESKTOP=shell (可选)
 *   → extraEnv (per-run/per-app 覆盖, 优先级最高)
 *
 * spawn 点声明 SessionEnvPolicy 拿成品, 不再各自追加策略行。
 */

#include <string>
#include <vector>

namespace winehua {

// -- 桌面会话 DXVK 稳定化 overlay --
// probeBase: WINEHUA_PERF_PROFILE / VN_WINEHUA_STRONG_RING_BARRIER / trace 键
// 的探测基准 (旧实现探测 LaunchParams.envStrs = 基线+D3D+compat 后的会话 env;
// 管线化后由 BuildSessionEnv 传"到此处为止的 env"快照, 语义一致)。
// 实现保证: 全部 probeBase 读取先于对 env 的写入, 允许调用方传同一 vector。
void AppendStableDxvkEnv(std::vector<std::string>& env,
                                const std::vector<std::string>& probeBase,
                                const std::string& d3dBackend,
                                const std::string& dxvkBackend);

// -- 会话/程序 env 管线 --
struct SessionEnvPolicy {
    // 基线参数 (透传 BuildWineEnv)
    std::string sockDir, sockName, libPath, binDir, homeDir, prefixDir;
    std::string wineLang = "zh_CN";
    int audioBootstrapFd = -1;
    // D3D overlay: d3dBackend 空 = 不注入 (RunWineExe 手动路径 — 该路径启动
    // explorer 等系统组件, 本身不需要 DXVK overlay)
    std::string d3dBackend, dxvkBackend;
    // DXVK/VKD3D 稳定化 overlay: WEAKBARRIER=0 clamp + DXVK_LOG + perf profile。
    // 消费方: explorer 桌面会话链 + RunWineExe 程序直启 (runWineProgram),
    // 两侧同源 — ArkTS 不再平行维护默认值拷贝
    bool applyStableOverlay = false;
    // WINEHUA_DESKTOP=shell: 接入 explorer shell desktop (任务栏可见)
    bool desktopShellFlag = false;
    // per-run/per-app 覆盖, 最后写入 (优先级最高)
    std::vector<std::string> extraEnv;
};

std::vector<std::string> BuildSessionEnv(const SessionEnvPolicy& p);

} // namespace winehua

#endif // WINE_ENV_PROFILES_H
