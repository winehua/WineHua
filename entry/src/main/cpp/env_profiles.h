#ifndef WINE_ENV_PROFILES_H
#define WINE_ENV_PROFILES_H

/**
 * env_profiles.h — 环境策略集中地 (重构第 3 步)
 *
 * 管线顺序只在此文件定义一次:
 *   BuildWineEnv (L0-L5 基线, wine_env.cpp)
 *   → AppendD3dBackendEnv (dxvk/vkd3d 受管 overlay, wine_env.cpp)
 *   → AppendCompatEnvLines (设置页兼容档位, 本文件)
 *   → AppendStableDesktopDxvkEnv (桌面会话稳定化 overlay, 本文件, 可选)
 *   → WINEHUA_DESKTOP=shell (可选)
 *   → extraEnv (per-run/per-app 覆盖, 优先级最高)
 *
 * spawn 点声明 SessionEnvPolicy 拿成品, 不再各自追加策略行。
 */

#include <string>
#include <vector>

namespace winehua {

// -- 兼容模式全局档位 (设置页 → launchClient compatEnvStr 分号串) --
// 键清单与各档取值的唯一来源在 ArkTS Box64Dynarec.ets (policy 归 ArkTS,
// 供 UI 档位/未来逐键微调演进); 本侧只是机制层前缀门: 放行
// BOX64_DYNAREC_* 行 (防注入其它 key), 不认识档位名, 空串 = 出厂基线不注入。
// 会话 env 经 UpsertEnvLine 压过基线 (每 key 最后写入者胜出);
// DXVK/desktop 的 WEAKBARRIER=0 clamp 在 AppendStableDesktopDxvkEnv 尾,
// 只会重新压回, 不会被档位击穿 (该 clamp 是 Venus 图形 ring 约束, 只
// 覆盖 explorer 会话链; wineboot/wineserver 无图形, 档位原值直接生效)。
// 仅 __aarch64__ (Box64) 设备有意义; x86_64 原生跑无 box64, 空转不注入。
#ifdef __aarch64__
// 统一过滤: 前缀 + entryParams 协议危险字符 ('|'/'\n') + 缺 '=' 畸形行 — 会话
// env 与 NCP entryParams 两条通道同一套行为 (原来源彼此漂移, 静默丢弃语义不一)
std::vector<std::string> FilterCompatLines(const std::string& compatEnvStr);

// 会话 env / SpawnRequest.env 增量注入。NCP 路线由 Spawner 经 EnvSpec
// 序列化为 __env= 段, 子进程 apply 晚于进程内基线, 档位胜出。
void AppendCompatEnvLines(std::vector<std::string>& env,
                          const std::string& compatEnvStr);
#endif // __aarch64__

// -- 桌面会话 DXVK 稳定化 overlay --
// probeBase: WINEHUA_PERF_PROFILE / VN_WINEHUA_STRONG_RING_BARRIER / trace 键
// 的探测基准 (旧实现探测 LaunchParams.envStrs = 基线+D3D+compat 后的会话 env;
// 管线化后由 BuildSessionEnv 传"到此处为止的 env"快照, 语义一致)。
// 实现保证: 全部 probeBase 读取先于对 env 的写入, 允许调用方传同一 vector。
void AppendStableDesktopDxvkEnv(std::vector<std::string>& env,
                                const std::vector<std::string>& probeBase,
                                const std::string& d3dBackend,
                                const std::string& dxvkBackend);

// -- 会话/程序 env 管线 --
struct SessionEnvPolicy {
    // 基线参数 (透传 BuildWineEnv)
    std::string sockDir, sockName, libPath, binDir, homeDir, prefixDir;
    std::string wineLang = "zh_CN";
    int audioBootstrapFd = -1;
    // D3D overlay: d3dBackend 空 = 不注入 (RunWineExe 手动路径, 由调用者
    // 经 extraEnv 自带 d3dLaunchEnvironment)
    std::string d3dBackend, dxvkBackend;
    // 兼容档位 (仅 aarch64 生效)
    std::string compatEnvStr;
    // explorer 桌面会话链收口: WEAKBARRIER=0 clamp + DXVK_LOG + perf profile
    bool stableDesktopOverlay = false;
    // WINEHUA_DESKTOP=shell: 接入 explorer shell desktop (任务栏可见)
    bool desktopShellFlag = false;
    // per-run/per-app 覆盖, 最后写入 (优先级最高)
    std::vector<std::string> extraEnv;
};

std::vector<std::string> BuildSessionEnv(const SessionEnvPolicy& p);

} // namespace winehua

#endif // WINE_ENV_PROFILES_H
