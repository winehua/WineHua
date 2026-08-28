#pragma once

#include <cstdint>

// ============================================================================
// zorder_policy: 场景层序政策的单一权威 (重构第 3 步, 行为平价)
//
// 收敛的散点 (此前各处手写):
//   1) BuildLayerListLocked 尾部置顶循环 (desktop_compositor.cpp):
//      parent==root 或 isExternal 或 父不在 z-order 的层恒置顶。
//   2) zc_bridge GetOccluders 遮挡扫描 (zc_bridge.cpp): subsurface 层是否
//      应挡住 ZC 层 — 父==root/ZC 窗口恒遮挡, 否则父 z-order 位置须 >= ZC 位置。
//   3) PickFullscreenLayerLocked 的 fsPriority 取最大 (desktop_compositor.cpp)
//      — 全屏前台选取号次, 见 ToplevelState::fsPriority 注释。
//
// 本文件提供可读谓词; 层序"生成有序列表"仍由 DesktopCompositor::BuildLayerListLocked
// 编排 (它是层序单一数据源, zc_/tmgr_/subsurfaceLayers_ 访问需锁), 谓词封装
// "哪类层应压在上"的比较规则 — 消费方只调谓词, 不手写条件。
//
// 行为平价: 所有断言逐字复现原有 if 条件, 只换知识归属 (散点 → 此处)。
// ============================================================================

namespace winehua {

// 判定"该层应恒置顶" — BuildLayerListLocked 尾部置顶循环的条件。
//   parentIsRoot  = 层挂桌面 root (任务栏等外部层);
//   isExternal    = 弹出式菜单 (isExternal, 跨窗口 offset);
//   parentInZOrder= 父窗口是否在 toplevelZOrder_ (不在列的旧外部层也置顶)。
inline bool ZOrderTopAnchored(bool parentIsRoot, bool isExternal,
                              bool parentInZOrder)
{
    return parentIsRoot || isExternal || !parentInZOrder;
}

// 判定"subsurface 层遮挡 ZC 层时是否需要检查父窗口 z-order 位置" —
// zc_bridge GetOccluders 的防护条件。返回 false = 恒遮挡 (父==ZC 窗口或
// 父==root, 不走 z-order 位置比较); true = 需检查父 z-order 位置 >= ZC 位置。
//   parentIsZcOwner = 层父窗口 == ZC 层所在的 toplevel;
//   parentIsRoot    = 层挂桌面 root。
inline bool ZOrderNeedsParentPosCheck(bool parentIsZcOwner, bool parentIsRoot)
{
    return !parentIsZcOwner && !parentIsRoot;
}

// 判定"新的全屏候选是否应取代当前已选中的全屏前台" —
// PickFullscreenLayerLocked 的选取法则 (收口的第 3 个散点)。
// 规则: 可见全屏窗口 (可多个同时 fullscreen) 中取 FsPriority 最大者当全屏
// 前台, 原因/局限见 ToplevelState::fsPriority 注释。candPriority 为候选
// 序号, bestPriority 为当前已选序号, bestValid=当前是否已有选中 (无选中
// 时恒取当前候选)。行为等价断言: !bestValid || cand > best。
inline bool ZOrderFullscreenCandidateBeats(uint64_t candPriority, uint64_t bestPriority,
                                           bool bestValid)
{
    return !bestValid || candPriority > bestPriority;
}

// ============================================================================
// 层序显式化 (重构第 3B 步): BuildLayerListLocked 的排布分组/排序键/谓词。
// 旧实现用"嵌套循环隐式排布" (main 循环按 z-order 逐窗口块 + 尾部置顶循环)
// 产生层序; 此处把同一排布表达为显式排序键 (ZOrderGroup + ZOrderSeq +
// ZOrderGroupFor), 供 BuildLayerListLocked 组待排项后 std::sort — 输出序列
// 与旧实现逐元素一致 (行为平价)。
// ============================================================================

// 层序分组: Root = 恒首 (不参与排序, 建层时直接先出); InZOrder = 随父窗口
// z-order (父层 + 其非 external 子层); TopAnchored = 恒置顶段
// (parent==root / isExternal / 父不在 z-order)。
enum class ZOrderGroup { Root = 0, InZOrder = 1, TopAnchored = 2 };

// 排序键全序: (group, laneSeq, itemSeq) 字典序。
//   InZOrder:   laneSeq = 父窗口在 z-order 的组内序号; itemSeq = 父层 0 /
//               子层 listIdx+1 (父层恒在同组同 lane 首位; 同父子层按
//               subsurfaceLayers_ 顺序稳定)。
//   TopAnchored: laneSeq = 0; itemSeq = 层在 subsurfaceLayers_ 的序号
//               (列表顺序)。
struct ZOrderSeq {
    ZOrderGroup group = ZOrderGroup::Root;
    int64_t laneSeq = 0;
    int64_t itemSeq = 0;
};
inline bool operator<(const ZOrderSeq& a, const ZOrderSeq& b) {
    if (a.group != b.group) return a.group < b.group;
    if (a.laneSeq != b.laneSeq) return a.laneSeq < b.laneSeq;
    return a.itemSeq < b.itemSeq;
}

// 分组建键依据: 组归属 = !ZOrderTopAnchored → InZOrder, 否则 TopAnchored
// (与旧 main 循环 / 尾部循环的互斥划分逐字对应: main = 跟父且非 external,
//  尾部 = parent==root | isExternal | 父不在 z-order)。
inline ZOrderGroup ZOrderGroupFor(bool parentIsRoot, bool isExternal, bool parentInZOrder) {
    return ZOrderTopAnchored(parentIsRoot, isExternal, parentInZOrder)
        ? ZOrderGroup::TopAnchored : ZOrderGroup::InZOrder;
}

// 任务栏 pin 全屏例外收口 (原 ToplevelManager::PinToTop 手写 !raisedFullscreen):
// raisedId 窗口被 raise 后把 pinId 压回栈顶; 被 raise 窗口全屏时不压
// (游戏全屏必须压过任务栏)。
inline bool ZOrderPinSuppressed(bool raisedIsFullscreen) { return raisedIsFullscreen; }

} // namespace winehua
