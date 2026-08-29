// zorder_policy.h (层序显式化: ZOrderGroupFor 分组 / ZOrderSeq 排序键 /
// ZOrderPinSuppressed pin 例外) 的宿主机单元测试 (make test)。
// 覆盖: 分组 8 组合边界 (唯一 InZOrder = (非 root, 非 external, 在 z-order));
// 真实层序场景排序全表断言 (3 窗口 + 每窗口 2 subsurface + 菜单/任务栏/游离层,
// 含 root 在 z-order 首位的 lane 偏移情形); pin 例外谓词。
#include "compositor/toplevel/zorder_policy.h"
#include <algorithm>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

using namespace winehua;

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond, msg) do { \
    ++g_checks; \
    if (!(cond)) { \
        ++g_failures; \
        std::printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
    } \
} while (0)

// -- 1. ZOrderGroupFor 分组 8 组合 --
static int TestGroupFor()
{
    struct GroupCase { bool root, ext, inZ; ZOrderGroup expect; };
    const GroupCase cases[] = {
        // root=true (4 组合): 挂桌面 root 恒 TopAnchored
        { true,  false, false, ZOrderGroup::TopAnchored },
        { true,  false, true,  ZOrderGroup::TopAnchored },
        { true,  true,  false, ZOrderGroup::TopAnchored },
        { true,  true,  true,  ZOrderGroup::TopAnchored },
        // isExternal (root=false 2 组合): 弹出式菜单恒 TopAnchored
        { false, true,  false, ZOrderGroup::TopAnchored },
        { false, true,  true,  ZOrderGroup::TopAnchored },
        // 父不在 z-order (root=false 且 non-external 1 组合): 游离层 TopAnchored
        { false, false, false, ZOrderGroup::TopAnchored },
        // 唯一 InZOrder: 跟父窗口且非 external 且父在 z-order
        { false, false, true,  ZOrderGroup::InZOrder },
    };
    for (const auto& c : cases) {
        const bool got = ZOrderGroupFor(c.root, c.ext, c.inZ) == c.expect;
        // 与 ZOrderTopAnchored 谓词互斥划分一致 (非 TA 即 InZOrder)
        const bool ta = ZOrderTopAnchored(c.root, c.ext, c.inZ);
        const bool consistent =
            (ZOrderGroupFor(c.root, c.ext, c.inZ) == (ta ? ZOrderGroup::TopAnchored
                                                         : ZOrderGroup::InZOrder));
        CHECK(got && consistent, "ZOrderGroupFor boundary");
    }
    return 0;
}

// -- 2. 排序键模拟真实层序 --
// 镜像 BuildLayerListLocked 的密钥生成 (Toplevel laneOf 映射 + subsurface
// li 计数), 用 ZOrderGroupFor/ZOrderSeq 排布后断言最终序列与逐项键。
struct Sl { const char* name; uint32_t parent; bool ext; };
struct Item { ZOrderSeq seq; const char* name; };

static int TestLayerOrder()
{
    const uint32_t rootId = 999;
    // z-order: root 占位在列首位 (生产时序可能: 先 AddToZOrder 后 CheckRoot)
    const std::vector<uint32_t> zorder = { rootId, 100, 101, 102 };

    // subsurfaceLayers_ (列表顺序即 li)
    const std::vector<Sl> sls = {
        { "z0-subA",   100, false },
        { "menu",      101, true  },  // 弹出式菜单 (owner z1): TopAnchored
        { "z2-subA",   102, false },
        { "taskbar",   rootId, false }, // 挂 root: TopAnchored
        { "z1-subA",   101, false },
        { "z0-subB",   100, false },
        { "floating",  777, false },  // 父不在 z-order: TopAnchored
        { "z1-subB",   101, false },
        { "z2-subB",   102, false },
    };

    std::vector<Item> items;
    // --- Toplevel 待排项 (laneSeq = emit 序号, 不计 root) ---
    std::unordered_map<uint32_t, int64_t> laneOf;
    int64_t zi = 0;
    for (uint32_t id : zorder) {
        if (id == rootId) continue;  // root 由 Root 层表示
        laneOf[id] = zi;
        const char* name = (id == 100) ? "z0-parent" : (id == 101) ? "z1-parent" : "z2-parent";
        items.push_back({ { ZOrderGroup::InZOrder, zi, 0 }, name });
        ++zi;
    }
    // --- Subsurface 待排项 (镜像生产密钥生成) ---
    int64_t li = 0;
    for (const auto& sl : sls) {
        const bool parentIsRoot = sl.parent == rootId;
        const bool parentInZ = std::find(zorder.begin(), zorder.end(), sl.parent) != zorder.end();
        ZOrderSeq seq;
        seq.group = ZOrderGroupFor(parentIsRoot, sl.ext, parentInZ);
        if (seq.group == ZOrderGroup::InZOrder) {
            seq.laneSeq = laneOf.at(sl.parent);
            seq.itemSeq = li + 1;
        } else {
            seq.laneSeq = 0;
            seq.itemSeq = li;
        }
        items.push_back({ seq, sl.name });
        ++li;
    }

    std::sort(items.begin(), items.end(),
              [](const Item& a, const Item& b) { return a.seq < b.seq; });

    // 期望序列 (Root 恒首不在 pending; InZOrder 块序 → TopAnchored li 序):
    const std::vector<std::pair<const char*, ZOrderSeq>> expected = {
        { "z0-parent", { ZOrderGroup::InZOrder, 0, 0 } },
        { "z0-subA",   { ZOrderGroup::InZOrder, 0, 1 } },  // li0+1
        { "z0-subB",   { ZOrderGroup::InZOrder, 0, 6 } },  // li5+1
        { "z1-parent", { ZOrderGroup::InZOrder, 1, 0 } },
        { "z1-subA",   { ZOrderGroup::InZOrder, 1, 5 } },  // li4+1
        { "z1-subB",   { ZOrderGroup::InZOrder, 1, 8 } },  // li7+1
        { "z2-parent", { ZOrderGroup::InZOrder, 2, 0 } },
        { "z2-subA",   { ZOrderGroup::InZOrder, 2, 3 } },  // li2+1
        { "z2-subB",   { ZOrderGroup::InZOrder, 2, 9 } },  // li8+1
        { "menu",      { ZOrderGroup::TopAnchored, 0, 1 } },  // li1
        { "taskbar",   { ZOrderGroup::TopAnchored, 0, 3 } },  // li3
        { "floating",  { ZOrderGroup::TopAnchored, 0, 6 } },  // li6
    };
    CHECK(items.size() == expected.size(), "layer order count");
    const size_t n = std::min(items.size(), expected.size());
    for (size_t i = 0; i < n; ++i) {
        const ZOrderSeq& s = items[i].seq;
        const ZOrderSeq& e = expected[i].second;
        CHECK(items[i].name == expected[i].first, "layer order name");
        CHECK(s.group == e.group && s.laneSeq == e.laneSeq && s.itemSeq == e.itemSeq,
              "layer order key");
    }

    // 键全序性质: 全序键无相等冲突 (两两互异) — std::sort 无需 stable
    bool unique = true;
    for (size_t i = 0; i < items.size(); ++i)
        for (size_t j = i + 1; j < items.size(); ++j)
            if (!(items[i].seq < items[j].seq) && !(items[j].seq < items[i].seq)) unique = false;
    CHECK(unique, "keys pairwise unique");

    // 组间顺序: Root < InZOrder < TopAnchored (Root 层恒首)
    CHECK((ZOrderSeq{ ZOrderGroup::Root, 0, 0 } <
           ZOrderSeq{ ZOrderGroup::InZOrder, 0, 0 }), "group Root<InZOrder");
    CHECK((ZOrderSeq{ ZOrderGroup::InZOrder, 0, 0 } <
           ZOrderSeq{ ZOrderGroup::TopAnchored, 0, 0 }), "group InZOrder<TopAnchored");
    return 0;
}

// -- 3. PinToTop 全屏例外谓词 --
static int TestPinSuppressed()
{
    CHECK(ZOrderPinSuppressed(true), "pin suppressed when raised fullscreen");
    CHECK(!ZOrderPinSuppressed(false), "pin not suppressed when raised windowed");
    // 原 PinToTop 条件 !raisedFullscreen 与谓词组合等价
    CHECK((true && !ZOrderPinSuppressed(true)) == false, "pin withhold fullscreen");
    CHECK((true && !ZOrderPinSuppressed(false)) == true, "pin proceeds windowed");
    return 0;
}

int main()
{
    TestGroupFor();
    TestLayerOrder();
    TestPinSuppressed();

    std::printf("zorder_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
