#include "spawner.h"
#include "wine/wine_constants.h"
#include "wine/wine_exe.h"  // SpawnViaBroker

#include <sys/types.h>

#undef LOG_TAG
#undef LOG_DOMAIN
#define LOG_DOMAIN 0x0000
#define LOG_TAG "WL_SPAWN"
#include <hilog/log.h>

namespace winehua {
namespace {

// 会话上下文 (见 spawner.h): binDir 默认由此取;
// homeDir 前缀 / WINEPREFIX 权威由 broker 服务端追加 (broker.cpp)
std::string gBinDir;

// wine 加载器 token ("wine" 作为 argv[0]) 的方案判定:
//   方案② (aarch64 宿主 + x86_64 wine, box64 转译): 不带 — box64 会把
//     entryParams argv[0] 放到 guest main_argv[1] (guest argv[0]=wine 二进制
//     路径), 带 "wine" 会被 wine loader 当成程序名 → start.exe 兜底 →
//     ShellExecute("wine") 失败 → explorer exit(1) 猝死 (cb4739b 修复的根因)。
//   方案① (x86_64 原生) / 方案③ (arm64 原生): 带 — wine_child Main dlopen
//     ntdll.so __wine_main 直启, 需要 argv[0]="wine" 作 loader 名。
#if defined(__aarch64__) && defined(WINEHUA_WINE_ARCH_IS_X86_64)
constexpr bool kNeedsWineLoaderToken = false;
#else
constexpr bool kNeedsWineLoaderToken = true;
#endif

pid_t SpawnLogged(const SpawnRequest& req, const std::string& params) {
    const pid_t pid = SpawnViaBroker(params, req.env);
    if (pid <= 0)
        OH_LOG_ERROR(LOG_APP, "[Spawner] broker spawn FAILED kind=%{public}d params=%{public}s",
                     (int)req.kind, params.c_str());
    else
        OH_LOG_INFO(LOG_APP, "[Spawner] broker spawn kind=%{public}d pid=%{public}d params=%{public}s",
                    (int)req.kind, (int)pid, params.c_str());
    return pid;
}

} // namespace

void Spawner::ConfigureSession(std::string homeDir, std::string binDir) {
    (void)homeDir;  // broker 服务端权威 (gBrokerHomeDir), 此处仅记录备查
    gBinDir = std::move(binDir);
}

// 全部 kind 统一走 broker (重构第 5 步): broker 服务端补 homeDir 前缀、
// WINEPREFIX 会话权威 (尾部 __env 追加, 后写胜出)、audio bootstrap fd;
// broker 本身在主进程内以线程运行, 启动不依赖 wineserver, 无先后环。
pid_t Spawner::Spawn(const SpawnRequest& req) {
    // 一次性日志: 确认宏分支命中 (hvigor daemon 缓存曾致方案错配, 见 package.sh)
    static bool schemeLogged = [] {
        OH_LOG_INFO(LOG_APP, "[Spawner] wine loader token=%{public}s",
                    kNeedsWineLoaderToken ? "ON (方案①/③ __wine_main)" : "OFF (方案② box64)");
        return true;
    }();
    (void)schemeLogged;
    const std::string& binDir = req.binDir.empty() ? gBinDir : req.binDir;

    // token 布局: binDir|[desktop]|[wine]|argv... (broker 收到后再补
    // homeDir 前缀; __env 段由 SpawnViaBroker 序列化追加)。
    // wine_child Main 按此解析; wineserver 由 Main 截获转入本体。
    std::string params = binDir;
    switch (req.kind) {
    case SpawnKind::Wineserver:
        params += "|wineserver|-f|-p";
        break;
    case SpawnKind::Wineboot:
        if (req.desktopSurface) params += "|__winehua_desktop__";
        if (kNeedsWineLoaderToken) params += "|wine";
        params += "|wineboot|--init";
        break;
    case SpawnKind::DesktopShell:
        params += "|__winehua_desktop__";
        if (kNeedsWineLoaderToken) params += "|wine";
        params += "|explorer";
        break;
    case SpawnKind::WineExe:
        if (kNeedsWineLoaderToken) params += "|wine";
        break;
    }
    for (const std::string& arg : req.argv) {
        params += "|";
        params += arg;
    }

    // 全路径 env 收口打点: 所有 kind (wineserver/wineboot/desktop shell/wine exe)
    // 都经过本节唯一 spawn 通道, 此处打出送入 broker 的最终 env (基线+overlay+
    // extraEnv 已合流), 与 wine_exe.cpp 的 "parsed options" 原样注入对照。
    if (!req.env.empty()) {
        std::string envJoined;
        for (const std::string& line : req.env) {
            if (!envJoined.empty()) envJoined += ";";
            envJoined += line;
        }
        OH_LOG_INFO(LOG_APP, "[Spawner] env kind=%{public}d count=%{public}zu [%{public}s]",
                    (int)req.kind, req.env.size(), envJoined.c_str());
    }

    return SpawnLogged(req, params);
}

} // namespace winehua
