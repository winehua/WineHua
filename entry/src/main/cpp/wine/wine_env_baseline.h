#ifndef WINE_ENV_BASELINE_H
#define WINE_ENV_BASELINE_H

/**
 * wine_env_baseline.h — Wine 环境基线单源 (header-only)
 *
 * 主进程 (wine_env.cpp BuildWineEnv, vector<string> 形态) 与子进程
 * (wine_child.cpp setup_wine_env, setenv 形态) 从同一张表生成公共键,
 * 各自只保留真分歧键:
 *   - XDG_RUNTIME_DIR / WAYLAND_DISPLAY (主: 合成器 socket 参数; 子: prefix/固定名)
 *   - LD_LIBRARY_PATH 系 (主: 按图形后端拼 runtimeLibPath; 子: 系统原生路径)
 *   - 仅主进程: LANG/LC_ALL, GST_PLUGIN_PATH, WINEDEBUG=-all 基线
 *   - 仅子进程: WINEBINDIR/WINEUNIXDIR (dladdr 路径修正), WINEDEBUG profile 选择
 *
 * header-only 的原因: wine_child 是独立 lib (libwine_child.so, 不链 entry
 * 的 obj), 只能 include 纯头文件; 与 wine_constants.h 同一约定。
 */

#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include "wine_constants.h"

namespace winehua {

// -- Box64 性能调优表 (单源, 仅 ARM64 有内容) --
// 历史: 曾是 SetBox64PerfEnv (setenv 版) 与 AppendBox64PerfStrings (字符串版)
// 两份手写拷贝, 增删键时容易只改一侧。现在键值只在这张表出现一次。
// 定位: 本表只是"出厂默认值需要偏离 box64 编译默认"的安全底 (automation/
// broker 等无 ArkTS 参与路径也靠它); 兼容档位的键清单与取值唯一来源是
// ArkTS Box64Dynarec.ets, DYNAREC 可调键默认不进本表。
#ifdef __aarch64__
inline const std::vector<std::pair<std::string, std::string>>& Box64PerfTable() {
    static const std::vector<std::pair<std::string, std::string>> kTable = {
        {"BOX64_LOG", "0"},
        {"BOX64_NOBANNER", "1"},
        {"BOX64_SHOWSEGV", "1"},
        // Keep Box64's compatibility default. Forcing 0 breaks code that
        // observes x86 flags across translated blocks, including protected
        // startup code.
        {"BOX64_DYNAREC_SAFEFLAGS", "1"},
        {"BOX64_DYNAREC_BIGBLOCK", "3"},
        {"BOX64_DYNAREC_CALLRET", "2"},
        {"BOX64_DYNAREC_FORWARD", "1024"},
        {"BOX64_DYNAREC_WEAKBARRIER", "2"},
        {"BOX64_AVX", "0"},
        // Box64 0.4.3 的 dynarec 对 AES-NI/PCLMULQDQ (GnuTLS AES-GCM 加速路径)
        // 的翻译有误: 解密得到乱码 (HTTPS 12152/400) 或 access violation。
        // 关闭模拟 cpuid 中的这两个特性位后, GnuTLS/nettle 回退纯 C 实现,
        // dynarec 对普通标量代码翻译正确, TLS 即恢复正常。与构建期
        // --disable-assembler/--disable-assembly 构成双保险。
        {"BOX64_AES", "0"},
        {"BOX64_PCLMULQDQ", "0"},
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
        {"BOX64_DYNAREC_VOLATILE_METADATA", "0"},
    };
    return kTable;
}
#endif

// setenv emitter: 子进程 (wine_child) 侧用
inline void SetBox64PerfEnv() {
#ifdef __aarch64__
    for (const auto& kv : Box64PerfTable())
        setenv(kv.first.c_str(), kv.second.c_str(), 1);
#endif
}

// K=V 行 emitter: 主进程 (BuildWineEnv) 侧用
inline void AppendBox64PerfStrings(std::vector<std::string>& env) {
#ifdef __aarch64__
    for (const auto& kv : Box64PerfTable())
        env.push_back(kv.first + "=" + kv.second);
#else
    (void)env;
#endif
}

// -- 公共基线 --

struct WineBaselinePaths {
    std::string binDir;
    std::string homeDir;    // 空 = 不产生 HOME 行 (子进程允许不传 homeDir)
    std::string prefixDir;  // 空 = 回落 WINE_PREFIX
};

// 公共基线 K=V 行 (有序)。调用方各自追加分歧键; 覆盖语义由调用方保证
// (主进程 UpsertEnvLine 后写胜出 / 子进程 __env overrides 最后 apply)。
inline std::vector<std::string> BuildWineBaselineLines(const WineBaselinePaths& p) {
    const std::string& binDir = p.binDir;
    const std::string shareDir = binDir + "/../share";
    const std::string prefix = p.prefixDir.empty() ? std::string(WINE_PREFIX) : p.prefixDir;
    std::string dllPath = binDir + "/" WINE_PE_SUBDIR ":" + binDir + "/i386-windows:" + binDir;
#if !defined(__aarch64__) || !defined(WINEHUA_WINE_ARCH_IS_X86_64)
    // 方案①③: bundled libs 加入 WINEDLLPATH, load_unixlib_by_name() 从此搜索
    // .so (wineohos.so 等)。方案② (box64 转译) 不加: el1 arm64 原生库不走
    // wine PE/unixlib 搜索 (与 wine_env.cpp BuildWineEnv 的 Layer 2 一致)。
#ifdef __aarch64__
    dllPath += ":/data/storage/el1/bundle/libs/arm64";
#else
    dllPath += ":/data/storage/el1/bundle/libs/x86_64";
#endif
#endif

    std::vector<std::string> lines = {
        "WINEPREFIX=" + prefix,
        "WINEDATADIR=" + shareDir + "/wine",
        "WINEDLLDIR=" + binDir + "/" WINE_UNIX_SUBDIR,
        "WINEDLLDIR0=" + binDir + "/" WINE_PE_SUBDIR,
        "WINEDLLDIR1=" + binDir + "/i386-windows",
        "WINEDLLDIR2=" + binDir,
        "WINEDLLPATH=" + dllPath,
        "XKB_CONFIG_ROOT=" + shareDir + "/X11/xkb",
        "PATH=/usr/local/bin:/data/app/bin:/usr/bin:/vendor/bin:" + binDir +
            "/" WINE_PE_SUBDIR ":" + binDir + "/i386-windows:" + binDir,
        "TMPDIR=" WINE_TMPDIR,
        "MIDI_SOUNDFONT_PATH=" + binDir + "/../audio/winehua-gm.sf2",
    };
    if (!p.homeDir.empty())
        lines.insert(lines.begin(), "HOME=" + p.homeDir);
    return lines;
}

// setenv emitter: 把 K=V 行逐条写入当前进程 environ (后者胜出靠调用顺序保证)
inline void ApplyEnvLinesToEnviron(const std::vector<std::string>& lines) {
    for (const std::string& line : lines) {
        const size_t sep = line.find('=');
        if (sep == std::string::npos || sep == 0) continue;
        setenv(line.substr(0, sep).c_str(), line.substr(sep + 1).c_str(), 1);
    }
}

} // namespace winehua

#endif // WINE_ENV_BASELINE_H
