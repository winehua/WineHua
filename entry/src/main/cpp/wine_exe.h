#pragma once

#include <napi/native_api.h>

#include <string>
#include <vector>

// 与 Index.ets runWineProgram 参数一一对应。自动拉起路径
// (LaunchPadMode 非桌面分支的 explorer) 也复用同一结构, 保证自动启动
// 与手动启动走完全相同的 broker 启动路径。
struct ProgramOptions {
    std::string windowsExePath;
    std::vector<std::string> argv;
    std::vector<std::string> environment;
    std::string workingDirectory;
    std::string d3dBackend = "vkd3d_limited_500k";
    std::string dxvkBackend = "dxvk_legacy";
    std::string presentBackend = "virgl_compositor";
};

// 经 broker 通道启动一个 Wine 程序 (手动 runWineProgram 与自动拉起共用)。
// 返回子进程 pid, <= 0 表示启动失败。
int SpawnWineProgram(const ProgramOptions& options);

// 经 broker Unix socket 发送 SPAWN 请求, 返回子进程 pid, <= 0 表示失败。
// 实现位于 wine_exe.cpp; 新代码一般不直接调用 — 走 winehua::Spawner
// (spawner.h) 声明 SpawnKind 由它收口路由与 token 布局。
pid_t SpawnViaBroker(const std::string& entryParams,
                     const std::vector<std::string>& environment);

napi_value RunWineExe(napi_env env, napi_callback_info info);
napi_value RunWineProgram(napi_env env, napi_callback_info info);
napi_value QueryWineProcess(napi_env env, napi_callback_info info);
napi_value TerminateWineProcess(napi_env env, napi_callback_info info);
