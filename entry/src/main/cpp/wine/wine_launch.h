#pragma once

#include "wine_startup_state.h"
#include <string>
#include <vector>
#include <napi/native_api.h>

uint64_t BeginWineLaunch();
void CancelWineLaunch();
bool CancelWineLaunch(uint64_t generation);
winehua::WineStartupSnapshot GetWineStartupSnapshot();

struct LaunchParams {
    uint64_t generation = 0;
    std::string exePath;
    std::string sockPath;
    std::string libPath;
    std::string homeDir;      // 用户 Download 目录 (Z: 映射)
    std::string sockDir;
    std::string sockName;
    std::string winehuaBin;
    std::string containerId;
    std::string prefixDir;
    std::string d3dBackend = "dxvk_legacy";
    std::string dxvkBackend = "dxvk_legacy";
    // Wine locale 语言 ("zh_CN"/"en_US"), 来自设置页, 决定桌面会话的 LANG
    std::string wineLang = "zh_CN";
    bool directNcpSession = false;
    bool desktopVulkanCompositor = false;
    int desktopStallSeconds = 0; // opt-in startup diagnostic, never persisted
    std::vector<std::string> cpuEnvironment; // restricted WoW64 and depth-storage session settings
};

void LaunchThreadFunc(LaunchParams* p);
bool IsWinePrefixInitialized(const std::string& prefixDir);
bool IsWinePrefixInitialized();
