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

} // namespace winehua
