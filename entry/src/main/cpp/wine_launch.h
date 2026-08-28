#pragma once

#include <string>
#include <vector>
#include <napi/native_api.h>

struct LaunchParams {
    std::string exePath;
    std::string sockPath;
    std::string libPath;
    std::string homeDir;      // 用户 Download 目录 (Z: 映射)
    std::string sockDir;
    std::string sockName;
    std::string winehuaBin;
    std::string prefixDir;
    std::string d3dBackend = "vkd3d_limited_500k";
    std::string dxvkBackend = "dxvk_legacy";
    // Wine locale 语言 ("zh_CN"/"en_US"), 来自设置页, 决定桌面会话的 LANG
    std::string wineLang = "zh_CN";
    // Box64 dynarec 全局档位 env 行串 ("K=V;K=V;..."), 来自设置页"兼容模式";
    // 空 = 出厂基线不注入。native 零表: 只放行 BOX64_DYNAREC_* 行,
    // 会话 env 经 UpsertEnvLine 压过基线, wineboot/wineserver 追加 __env= 段
    std::string compatEnvStr;
};

void LaunchThreadFunc(LaunchParams* p);
bool IsWinePrefixInitialized(const std::string& prefixDir);
bool IsWinePrefixInitialized();
