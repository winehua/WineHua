// env_spec (EnvSpec / entryParams 序列化契约) 的宿主机单元测试 (make test)。
// 不依赖 OHOS SDK, 用宿主 g++ 编译。序列化规则收口前, "|'/'\n' 过滤、
// fd 变量禁入"曾散在 wine_env.cpp / ohos_broker.c 多处各自实现。
#include "env_spec.h"

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

using winehua::EnvSpec;

int main()
{
    // 1. set: upsert, 保首次插入位置, 后写值生效
    {
        EnvSpec e;
        e.set("A", "1");
        e.set("B", "2");
        e.set("A", "3");
        CHECK(e.size() == 2, "upsert keeps unique keys");
        CHECK(e.entries()[0].first == "A" && e.entries()[1].first == "B", "first-insert order kept");
        CHECK(*e.get("A") == "3", "last write wins");
        CHECK(e.get("MISSING") == nullptr, "get missing returns nullptr");
        CHECK(e.has("B") && !e.has("C"), "has()");
    }

    // 2. setLine: 非法行忽略, 空值合法
    {
        EnvSpec e;
        e.setLine("K=V");
        e.setLine("EMPTY=");
        e.setLine("NO_EQUALS");
        e.setLine("=NOKEY");
        CHECK(e.size() == 2, "malformed lines ignored");
        CHECK(e.has("EMPTY") && e.get("EMPTY")->empty(), "empty value kept");
    }

    // 3. mergeFrom: other 胜出, 新 key 追加在尾部
    {
        EnvSpec a = EnvSpec::fromLines({"A=1", "B=2"});
        EnvSpec b = EnvSpec::fromLines({"B=9", "C=3"});
        a.mergeFrom(b);
        CHECK(*a.get("A") == "1" && *a.get("B") == "9" && *a.get("C") == "3", "mergeFrom overrides");
        CHECK(a.entries()[2].first == "C", "merged new key appended");
    }

    // 4. 序列化: 格式 / fd 变量禁入 / 不可编码条目丢弃
    {
        EnvSpec e = EnvSpec::fromLines({
            "HOME=/home/x",
            "WINESERVERSOCKET=7",
            "WINE_OHOS_AUDIO_ENABLE=1",
            "WINE_OHOS_AUDIO_BOOTSTRAP_FD=12",
            "WINE_OHOS_AUDIO_PROTOCOL_VERSION=3",
            "BAD|KEY=v",
            "BADNL=v\nw",
            "LANG=zh_CN.UTF-8",
        });
        const std::string s = e.serializeEntryParams();
        CHECK(s == "|__env=HOME=/home/x|__env=LANG=zh_CN.UTF-8", "serialize format + filters");
    }

    // 5. fromLines 同 key 多行: 最后值生效 (等价旧链式 setenv 覆盖语义)
    {
        EnvSpec e = EnvSpec::fromLines({"K=first", "X=1", "K=last"});
        const std::string s = e.serializeEntryParams();
        CHECK(s == "|__env=K=last|__env=X=1", "dup keys collapse to last value at first position");
    }

    // 6. 过滤谓词本身 (与 ohos_broker.c env_forwardable 的镜像契约)
    {
        CHECK(winehua::IsPerProcessFdEnvKey("WINESERVERSOCKET"), "fd var: WINESERVERSOCKET");
        CHECK(winehua::IsPerProcessFdEnvKey("WINE_OHOS_AUDIO_ENABLE"), "fd var: AUDIO_ENABLE");
        CHECK(winehua::IsPerProcessFdEnvKey("WINE_OHOS_AUDIO_BOOTSTRAP_FD"), "fd var: AUDIO_FD");
        CHECK(winehua::IsPerProcessFdEnvKey("WINE_OHOS_AUDIO_PROTOCOL_VERSION"), "fd var: AUDIO_VER");
        CHECK(!winehua::IsPerProcessFdEnvKey("WINESERVERSOCKET2"), "prefix must be exact key");
        CHECK(!winehua::IsPerProcessFdEnvKey("HOME"), "normal key not fd var");
        CHECK(!winehua::IsEntryParamsEncodable("A=1|2"), "pipe rejected");
        CHECK(!winehua::IsEntryParamsEncodable("A=1\n2"), "newline rejected");
        CHECK(winehua::IsEntryParamsEncodable("A=1"), "normal line encodable");
    }

    // 7. toLines roundtrip
    {
        EnvSpec e = EnvSpec::fromLines({"A=1", "B=2"});
        std::vector<std::string> lines = e.toLines();
        CHECK(lines.size() == 2 && lines[0] == "A=1" && lines[1] == "B=2", "toLines roundtrip");
    }

    std::printf("%s: %d checks, %d failures\n",
                g_failures == 0 ? "PASS" : "FAIL", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
