#ifndef WINE_ENV_H
#define WINE_ENV_H

/**
 * wine_env.h — Wine 环境变量设置
 */

#include <cstdlib>
#include <string>
#include <vector>

#include "wine_constants.h"

// -- Box64 性能调优 (static inline, 供 napi_init / wine_child 共用) --
// 仅方案② box64+wine (arm64 设备 + x86_64 wine 全转译) 需要; 方案①③ 空实现。
#if defined(__aarch64__) && defined(WINEHUA_WINE_ARCH_IS_X86_64)
static inline void SetBox64PerfEnv() {
    setenv("BOX64_LOG", "0", 1);
    setenv("BOX64_NOBANNER", "1", 1);
    setenv("BOX64_SHOWSEGV", "1", 1);
    // Keep Box64's compatibility default. Forcing 0 breaks code that observes
    // x86 flags across translated blocks, including protected startup code.
    setenv("BOX64_DYNAREC_SAFEFLAGS", "1", 1);
    setenv("BOX64_DYNAREC_BIGBLOCK", "3", 1);
    setenv("BOX64_DYNAREC_CALLRET", "2", 1);
    setenv("BOX64_DYNAREC_FORWARD", "1024", 1);
    setenv("BOX64_DYNAREC_WEAKBARRIER", "2", 1);
    setenv("BOX64_AVX", "0", 1);
    // Box64 0.4.3 的 dynarec 对 AES-NI/PCLMULQDQ (GnuTLS AES-GCM 加速路径)
    // 的翻译有误: 解密得到乱码 (HTTPS 12152/400) 或 access violation。
    // 关闭模拟 cpuid 中的这两个特性位后, GnuTLS/nettle 回退纯 C 实现,
    // dynarec 对普通标量代码翻译正确, TLS 即恢复正常。与构建期
    // --disable-assembler/--disable-assembly 构成双保险。
    setenv("BOX64_AES", "0", 1);
    setenv("BOX64_PCLMULQDQ", "0", 1);
    // 关闭 box64 的 PE Volatile Metadata 解析 (默认开启), 原因:
    // box64 的 my_mmap64 (wrappedlibc.c) 对 Wine 每个首次 mmap 的文件调用
    // ParseVolatileMetadata (src/tools/pe_tools.c), 该函数只校验 MZ 魔数、
    // 不校验 e_lfanew 边界。DOS/16位 MZ 可执行文件 (非 PE, 如仙剑 DOS 版的
    // PAL!.EXE / DJGPP 工具 exe) 的 0x3C 处是 DOS stub 文本 (" by "/"mail"),
    // 被当作 e_lfanew 偏移 → base+~545MB 越界解引用 → 宿主侧 SIGSEGV →
    // 被 Wine SEH 当客体异常恢复, 撕裂 my_mmap64 宿主调用栈 → explorer
    // 浏览含 DOS exe 的目录 (图标提取逐个 mmap) 连炸后挂死 (2026-07 实测)。
    // 副作用≈0: 本项目 STRONGMEM=0, 该元数据唯一生效消费点是给新 MSVC
    // (2019 16.10+) 编译的 PE 标注点额外加 DMB_ISHST 屏障 (正确性增强,
    // 非性能优化); 老游戏无此元数据, lock/原子指令走独立路径不受影响。
    // 若日后 fork 内给 pe_tools.c 补上边界检查, 可移除此行重新启用。
    setenv("BOX64_DYNAREC_VOLATILE_METADATA", "0", 1);
}

inline void AppendBox64PerfStrings(std::vector<std::string>& env) {
    env.push_back("BOX64_LOG=0");
    env.push_back("BOX64_NOBANNER=1");
    env.push_back("BOX64_SHOWSEGV=1");
    env.push_back("BOX64_DYNAREC_SAFEFLAGS=1");
    env.push_back("BOX64_DYNAREC_BIGBLOCK=3");
    env.push_back("BOX64_DYNAREC_CALLRET=2");
    env.push_back("BOX64_DYNAREC_FORWARD=1024");
    env.push_back("BOX64_DYNAREC_WEAKBARRIER=2");
    env.push_back("BOX64_AVX=0");
    // 与 SetBox64PerfEnv() 保持一致: 屏蔽模拟 cpuid 的 AES-NI/PCLMULQDQ,
    // 避免 Box64 dynarec 对 GnuTLS AES-GCM 加速路径的误译 (HTTPS 乱码/崩溃)
    env.push_back("BOX64_AES=0");
    env.push_back("BOX64_PCLMULQDQ=0");
    // 关闭 Volatile Metadata 解析, 详细原因见上方 SetBox64PerfEnv() 注释
    // (box64 pe_tools.c 对 DOS MZ exe 无边界检查 → explorer 浏览目录挂死)
    env.push_back("BOX64_DYNAREC_VOLATILE_METADATA=0");
}
#else
static inline void SetBox64PerfEnv() {}
static inline void AppendBox64PerfStrings(std::vector<std::string>& env) { (void)env; }
#endif

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
size_t AppendMissingEntryParamsEnvOverrides(std::string& entryParams,
                                            const std::vector<std::string>& env);
std::string SerializeEnvToEntryParams(const std::vector<std::string>& env);

// -- Graphics 辅助 --
void LogGraphicsBackendStateForLaunch(const char* tag);

#endif // WINE_ENV_H
