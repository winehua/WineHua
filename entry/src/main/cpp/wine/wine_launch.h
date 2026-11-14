#pragma once

#include <string>
#include <vector>
#include <napi/native_api.h>
#include "display_environment.h"

struct LaunchParams {
    std::string exePath;
    std::string sockPath;
    std::string libPath;
    std::string homeDir;      // 用户 Download 目录 (Z: 映射)
    std::string sockDir;
    std::string sockName;
    std::string winehuaBin;
    std::string prefixDir;
    winehua::WineDisplayEnvironment display;
    int32_t x11DesktopWidth = 0, x11DesktopHeight = 0;
    std::string d3dBackend = "dxvk_modern_2_6";
    std::string dxvkBackend = "dxvk_legacy";
    // Wine locale 语言 ("zh_CN"/"en_US"), 来自设置页, 决定桌面会话的 LANG
    std::string wineLang = "zh_CN";
};

void LaunchThreadFunc(LaunchParams* p);
bool IsWinePrefixInitialized(const std::string& prefixDir);
bool IsWinePrefixInitialized();
