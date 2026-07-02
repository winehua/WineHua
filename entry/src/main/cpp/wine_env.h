#ifndef WINE_ENV_H
#define WINE_ENV_H

/**
 * wine_env.h — Wine 环境变量设置
 */

#include <cstdlib>
#include <string>
#include <vector>

// -- Box64 性能调优 (static inline, 供 napi_init / wine_child 共用) --
static inline void SetBox64PerfEnv() {
    setenv("BOX64_LOG", "0", 1);
    setenv("BOX64_NOBANNER", "1", 1);
    setenv("BOX64_SHOWSEGV", "1", 1);
    setenv("BOX64_DYNAREC_SAFEFLAGS", "0", 1);
    setenv("BOX64_DYNAREC_BIGBLOCK", "3", 1);
    setenv("BOX64_DYNAREC_CALLRET", "2", 1);
    setenv("BOX64_DYNAREC_FORWARD", "1024", 1);
    setenv("BOX64_DYNAREC_WEAKBARRIER", "2", 1);
    setenv("BOX64_AVX", "0", 1);
}

inline void AppendBox64PerfStrings(std::vector<std::string>& env) {
    env.push_back("BOX64_LOG=0");
    env.push_back("BOX64_NOBANNER=1");
    env.push_back("BOX64_SHOWSEGV=1");
    env.push_back("BOX64_DYNAREC_SAFEFLAGS=0");
    env.push_back("BOX64_DYNAREC_BIGBLOCK=3");
    env.push_back("BOX64_DYNAREC_CALLRET=2");
    env.push_back("BOX64_DYNAREC_FORWARD=1024");
    env.push_back("BOX64_DYNAREC_WEAKBARRIER=2");
    env.push_back("BOX64_AVX=0");
}

// -- Wine 环境变量构建 --
std::vector<std::string> BuildWineEnv(const std::string& sockDir,
                                      const std::string& sockName,
                                      const std::string& libPath,
                                      const std::string& binDir,
                                      int audioBootstrapFd);

// -- Audio bootstrap --
int CreateAudioBootstrapFd(const std::string& runtimeDir);

// -- Graphics 辅助 --
std::string BasenameOfPath(const std::string& path);
bool IsGraphicsSmokeExePath(const std::string& path);
void LogGraphicsBackendStateForLaunch(const char* tag);

#endif // WINE_ENV_H
