#ifndef WINE_SPAWNER_H
#define WINE_SPAWNER_H

/**
 * spawner.h — SpawnRequest/Spawner: 进程启动的意图与机制分离 (重构第 4 步)
 *
 * 调用方只声明 "启动什么 + 带什么 env 增量"; 机制细节由 kind 收口推导:
 *   - 路由: 全部 kind 统一走 broker SPAWN (第 5 步起; broker 在主进程内
 *     以线程运行, 启动不依赖 wineserver, 无先后环)。broker 服务端补
 *     homeDir 前缀、WINEPREFIX 会话权威、audio bootstrap fd、进程登记。
 *   - token 布局: __winehua_desktop__ / wine 加载器前缀按方案判定
 *     (方案② aarch64+box64 不带 — wine_child Main 自注 box64+wine ELF;
 *      方案①③ 原生 __wine_main 带 "wine" argv[0], 见 spawner.cpp)
 *   - wineserver 由 wine_child Main 截获 argv[0]=="wineserver" 转入本体
 *     (纯 Unix ELF 不能走 wine loader 的 PE 解析)
 *
 * 刻意不在请求里的: homeDir/prefixDir (会话单例, broker 服务端权威),
 * fd (broker 自动挂 audio bootstrap)。
 */

#include <string>
#include <vector>

namespace winehua {

enum class SpawnKind {
    Wineserver,    // broker → Main 截获转入 wineserver 本体; argv 固定, 极简基线
    Wineboot,      // broker → Main: argv 固定 "wineboot --init", 极简 env (省 entryParams 长度)
    DesktopShell,  // broker Main: explorer + argv (桌面 shell)
    WineExe,       // broker Main: argv = [exePath, args...]
};

struct SpawnRequest {
    SpawnKind kind;
    // DesktopShell: explorer 的参数; WineExe: [exePath, args...];
    // Wineserver/Wineboot: 忽略 (argv 由 kind 固定)
    std::vector<std::string> argv;
    // K=V 增量行 (BuildSessionEnv 成品或极简集); 经 SpawnViaBroker 序列化
    // 为 __env= 段 (fd 变量/不可编码条目由 EnvSpec 契约过滤)
    std::vector<std::string> env;
    // __winehua_desktop__ token (explorer 桌面 / wineboot 首启的桌面 surface 路由)
    bool desktopSurface = false;
    // 空 = 会话默认 (ConfigureSession 的 binDir); RunWineExe 等 ArkTS 显式
    // 传 binDir 的路径在此透传
    std::string binDir;
};

class Spawner {
public:
    // 会话上下文: binDir 默认由此取;
    // homeDir/WINEPREFIX 权威在 broker 服务端 (gBroker*), homeDir 仅备查。
    static void ConfigureSession(std::string homeDir, std::string binDir);

    // 返回子进程 pid, <= 0 表示失败 (失败原因在内部已记日志)
    static pid_t Spawn(const SpawnRequest& req);
};

} // namespace winehua

#endif // WINE_SPAWNER_H
