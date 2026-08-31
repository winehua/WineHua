// wine_env_baseline (公共基线表 / Box64 单表 / setenv emitter) 的宿主机单元
// 测试 (make test)。不依赖 OHOS SDK, 用宿主 g++ 编译 (x86_64, __aarch64__
// 未定义 — Box64 表为空、WINEDLLPATH 带 bundle libs 尾巴, 即 x86_64 形态)。
// 基线收口前, 这些键在 wine_env.cpp / wine_child.cpp 两处各自手写维护。
#include "wine_env_baseline.h"

#include <cstdlib>
#include <cstdio>
#include <string>
#include <vector>

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond, msg) do { \
    ++g_checks; \
    if (!(cond)) { \
        ++g_failures; \
        std::printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
    } \
} while (0)

static const std::string* findValue(const std::vector<std::string>& lines, const std::string& key)
{
    for (const std::string& line : lines) {
        if (line.compare(0, key.size(), key) == 0 &&
            line.size() > key.size() && line[key.size()] == '=')
            return &line;
    }
    return nullptr;
}

int main()
{
    const std::string binDir = "/data/storage/el2/base/files/wine/bin";

    // 1. 公共键齐全且值正确
    {
        std::vector<std::string> lines = winehua::BuildWineBaselineLines(
            {binDir, "/home/u", "/pfx"});
        CHECK(findValue(lines, "HOME") && *findValue(lines, "HOME") == "HOME=/home/u",
              "HOME from homeDir");
        CHECK(findValue(lines, "WINEPREFIX") && *findValue(lines, "WINEPREFIX") == "WINEPREFIX=/pfx",
              "WINEPREFIX from prefixDir");
        CHECK(findValue(lines, "WINEDATADIR") &&
              *findValue(lines, "WINEDATADIR") == "WINEDATADIR=" + binDir + "/../share/wine",
              "WINEDATADIR derives from binDir");
        CHECK(findValue(lines, "WINEDLLDIR") &&
              *findValue(lines, "WINEDLLDIR") == "WINEDLLDIR=" + binDir + "/x86_64-unix",
              "WINEDLLDIR");
        CHECK(findValue(lines, "WINEDLLDIR0") && findValue(lines, "WINEDLLDIR1") &&
              findValue(lines, "WINEDLLDIR2"), "WINEDLLDIR0..2 present");
        const std::string* dllPath = findValue(lines, "WINEDLLPATH");
        CHECK(dllPath && dllPath->find(binDir + "/x86_64-windows:" + binDir +
              "/i386-windows:" + binDir) != std::string::npos,
              "WINEDLLPATH has wine PE dirs");
#ifdef __aarch64__
        CHECK(dllPath && dllPath->find("bundle/libs") == std::string::npos,
              "WINEDLLPATH no bundle-libs tail on aarch64");
#else
        CHECK(dllPath && dllPath->find(":/data/storage/el1/bundle/libs/x86_64") != std::string::npos,
              "WINEDLLPATH has bundle-libs tail on x86_64");
#endif
        CHECK(findValue(lines, "XKB_CONFIG_ROOT") &&
              *findValue(lines, "XKB_CONFIG_ROOT") == "XKB_CONFIG_ROOT=" + binDir + "/../share/X11/xkb",
              "XKB_CONFIG_ROOT");
        CHECK(findValue(lines, "PATH") &&
              findValue(lines, "PATH")->find("/vendor/bin:" + binDir) != std::string::npos,
              "PATH contains system dirs + binDir");
        CHECK(findValue(lines, "TMPDIR") && *findValue(lines, "TMPDIR") == "TMPDIR=" WINE_TMPDIR,
              "TMPDIR = WINE_TMPDIR");
        CHECK(findValue(lines, "MIDI_SOUNDFONT_PATH") &&
              findValue(lines, "MIDI_SOUNDFONT_PATH")->find("winehua-gm.sf2") != std::string::npos,
              "MIDI_SOUNDFONT_PATH");
    }

    // 2. 空 homeDir → 无 HOME 行 (子进程允许不传); 空 prefixDir → 回落 WINE_PREFIX
    {
        std::vector<std::string> lines = winehua::BuildWineBaselineLines({binDir, "", ""});
        CHECK(!findValue(lines, "HOME"), "empty homeDir yields no HOME line");
        CHECK(findValue(lines, "WINEPREFIX") &&
              *findValue(lines, "WINEPREFIX") == "WINEPREFIX=" WINE_PREFIX,
              "empty prefixDir falls back to WINE_PREFIX");
    }

    // 3. 分歧键不在公共表内 (XDG_RUNTIME_DIR / WAYLAND_DISPLAY / LD_LIBRARY_PATH /
    //    WINEDEBUG / LANG / WINEBINDIR 等由各调用点自行处理)
    {
        std::vector<std::string> lines = winehua::BuildWineBaselineLines(
            {binDir, "/home/u", "/pfx"});
        CHECK(!findValue(lines, "XDG_RUNTIME_DIR"), "XDG_RUNTIME_DIR stays per-site");
        CHECK(!findValue(lines, "WAYLAND_DISPLAY"), "WAYLAND_DISPLAY stays per-site");
        CHECK(!findValue(lines, "LD_LIBRARY_PATH"), "LD_LIBRARY_PATH stays per-site");
        CHECK(!findValue(lines, "WINEDEBUG"), "WINEDEBUG stays per-site");
        CHECK(!findValue(lines, "LANG"), "LANG stays per-site");
        CHECK(!findValue(lines, "WINEBINDIR"), "WINEBINDIR stays per-site");
    }

    // 4. 同一次构建内 key 唯一 (重复 key 会让 "后写胜出" 依赖调用顺序, 违反表意)
    {
        std::vector<std::string> lines = winehua::BuildWineBaselineLines(
            {binDir, "/home/u", "/pfx"});
        std::vector<std::string> keys;
        for (const std::string& line : lines)
            keys.push_back(line.substr(0, line.find('=')));
        for (size_t i = 0; i < keys.size(); ++i)
            for (size_t j = i + 1; j < keys.size(); ++j)
                CHECK(keys[i] != keys[j], "baseline keys unique");
    }

    // 5. ApplyEnvLinesToEnviron: 逐条 setenv, 跳过非法行, 后写胜出
    {
        winehua::ApplyEnvLinesToEnviron({
            "WINEHUA_BASELINE_TEST_A=1",
            "WINEHUA_BASELINE_TEST_B=x=y",   // 值含 '=' 合法 (按首个 '=' 切分)
            "no-equals-sign",                // 非法行忽略
            "=empty-key",                    // 空 key 忽略
            "WINEHUA_BASELINE_TEST_A=2",     // 后写胜出
        });
        const char* a = getenv("WINEHUA_BASELINE_TEST_A");
        const char* b = getenv("WINEHUA_BASELINE_TEST_B");
        CHECK(a && std::string(a) == "2", "apply: last write wins");
        CHECK(b && std::string(b) == "x=y", "apply: value keeps '=' tail");
    }

    // 6. Box64 单表双 emitter 一致性 (aarch64): 两 emitter 同源同序
    //    x86_64 宿主上两 emitter 均为空操作, 仅验证不崩
    {
        std::vector<std::string> env;
        winehua::AppendBox64PerfStrings(env);
#ifdef __aarch64__
        CHECK(!env.empty(), "Box64 perf lines non-empty on aarch64");
        CHECK(env.front() == "BOX64_LOG=0", "Box64 first entry");
        bool hasVolatile = false;
        for (const std::string& line : env)
            if (line == "BOX64_DYNAREC_VOLATILE_METADATA=0") hasVolatile = true;
        CHECK(hasVolatile, "Box64 table carries VOLATILE_METADATA=0");
#else
        CHECK(env.empty(), "Box64 perf lines empty on x86_64 host");
#endif
        winehua::SetBox64PerfEnv();  // 不崩即可 (x86_64 为空操作)
    }

    std::printf("env_baseline_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
