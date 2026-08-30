#ifndef WINE_ENV_H
#define WINE_ENV_H

/**
 * wine_env.h — Wine 环境变量设置
 */

#include <string>
#include <vector>

#include "wine_constants.h"
#include "wine_env_baseline.h"

// Box64 性能调优 (SetBox64PerfEnv / AppendBox64PerfStrings) 与公共基线
// (BuildWineBaselineLines / ApplyEnvLinesToEnviron) 已收口到
// wine_env_baseline.h 单表, 此处 via include 透出, 调用点用 winehua:: 前缀。

// -- Wine 环境变量构建 --
// wineLang: Wine locale 语言 ("zh_CN"/"en_US"), 决定基线 LANG=<wineLang>.UTF-8;
// 桌面会话经 launchClient 由用户设置传入, 程序直启路径由 ArkTS 经 environment
// 覆盖 (UpsertEnvLine 后于基线生效), 其余调用点保持默认中文
std::vector<std::string> BuildWineEnv(const std::string& sockDir,
                                      const std::string& sockName,
                                      const std::string& libPath,
                                      const std::string& binDir,
                                      int audioBootstrapFd,
                                      const std::string& homeDir,
                                      const std::string& prefixDir = WINE_PREFIX,
                                      const std::string& wineLang = "zh_CN");

// Add the managed product D3D overlays to a process environment. D3D12 and
// D3D11/DXGI are selected independently so a qualified DXVK 2.6.2 device does
// not get downgraded merely because the session also enables VKD3D 2.6.
void AppendD3dBackendEnv(std::vector<std::string>& env,
                         const std::string& d3dBackend,
                         const std::string& dxvkBackend,
                         const std::string& binDir);

// -- 环境变量辅助 --
void UpsertEnvLine(std::vector<std::string>& env, const std::string& line);

// -- Audio bootstrap --
int CreateAudioBootstrapFd(const std::string& runtimeDir);

// -- entryParams 序列化 --
// 迁移期 shim, 内部转发到 winehua::EnvSpec (env_spec.h); 新代码直接用 EnvSpec
std::string SerializeEnvToEntryParams(const std::vector<std::string>& env);

// -- Graphics 辅助 --
void LogGraphicsBackendStateForLaunch(const char* tag);

#endif // WINE_ENV_H
