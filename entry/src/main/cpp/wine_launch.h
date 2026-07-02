#pragma once

#include <string>
#include <vector>
#include <napi/native_api.h>

struct LaunchParams {
    std::string exePath;
    std::string sockPath;
    std::string libPath;
    std::string sockDir;
    std::string sockName;
    std::string winehuaBin;
    std::vector<std::string> envStrs;
    std::vector<char*> envp;
};

void LaunchThreadFunc(LaunchParams* p);
