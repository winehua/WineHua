#pragma once
#include <wayland-server-core.h>
#include <cstdint>
#include <unordered_map>
#include <vector>

class ToplevelManager;
struct SurfaceData;
struct ShmCommitInfo;

// PopupManager — PC 模式 popup (subsurface 菜单/子窗口) 的登记/裁剪/状态管理。
//
// 出处 (PLAN 索引): docs/COMPOSITOR_REFACTOR_PLAN.md §三 "PopupManager popup
// 登记/裁剪/事件 (吸收 wl_core.cpp:931-1075, popup 偏移公式单点化)" + §四
// 阶段5 第 2 条; 实施 = 重构第 5B2 步。
//
// -- 模块边界 (本段范围) --
// 吸收 wl_core.cpp 的 popup 全部状态管理 (popup 表 + 登记/更新/清理路径 +
// 级联收集 + 偏移公式收口调用)。popup 事件 (popup_show/resize/move/hide) 的
// fire 调用点保持现形态 (还在 wayland_server/wl_core — 红线), 本类事件段
// 只产出"事件描述" (PopupCommitEvent / PopupMoveEvent), JSON/文本/顺序由
// 调用方按重构前逐字恢复。
//
// -- 锁纪律 (PLAN §七: 拆分保持 tmgr 锁为唯一外层锁, 新增模块不引入独立锁) --
// 本类的 popup 表 (popups_/popupBySurfaceKey_) 由调用方持有的
// ToplevelManager::toplevelMutex_ 保护 — 本类方法是 ToplevelManager 原 popup
// 方法的迁移 (原方法名 Locked = 调用方已持锁的契约不变), 方法名 Locked 后缀
// = 调用方须已持有该锁。RemovePopupDataLocked 内部嵌套取 toplevelSurfaceMutex_
// (清 popup 的 surface 映射), 锁序与迁移前逐字 (toplevelMutex_ →
// toplevelSurfaceMutex_; 经 ToplevelManager::UnmapToplevelSurface 公开路径)。
// UpdatePopupOnCommit 与 Tmgr 的 HandleCommittedSizeLocked 均在方法自持的
// 同一锁段内调用 (锁边界与迁移前相同)。
//
// -- popup 帧状态存储 --
// popup 复用 ToplevelManager::ToplevelState (popupId 来自 tmgr 的 toplevel id
// 取号器, 帧数据/内容尺寸/格式存其中); 清理时经 EraseToplevelLocked /
// UnmapToplevelSurface 对称清除 (原 RemovePopupDataLocked 的 toplevels_ /
// toplevelSurfaceMap_ 段, 行为逐字)。
//
// -- popup 窗口/内容尺寸解耦补丁 (PLAN §2.5, war3 PC 模式 GL client surface
//    缩左上) 随 UpdatePopupOnCommit 正文平移, 见 cpp 定义处 --
class PopupManager {
public:
    PopupManager(ToplevelManager& tmgr, int32_t& outputW, int32_t& outputH);

    // popup 记录 (原 ToplevelManager::PopupRecord 字段, 其中 w/h 已随"尺寸
    // 上报去重改经 ToplevelManager::HandleCommittedSizeLocked 通道迁出 —
    // 看 UpdatePopupOnCommit 的尺寸语义说明, 该通道记录在 popup 的
    // ToplevelState::lastReportedW_/H, 清理时同随 ToplevelState 复位)
    struct PopupRecord {
        uint32_t popupId = 0;
        uint32_t parentToplevel = 0;
        wl_resource* surface = nullptr;  // popup 的 wl_surface (pointer enter 目标)
        uint64_t surfaceKey = 0;
        int32_t offX = 0, offY = 0;      // 相对父窗口内容原点 (geometry.h ComputePopupOffset 产出)
    };

    // popup 帧 commit 的事件描述 (原 WaylandServer::UpdatePopupOnCommit 事件段
    // 的输入)。json/日志/事件名由调用方按弹点逐字恢复:
    //   isNew       → popup_show {popupId,x,y,w,h,argb} (随后 return, 不发 resize/move)
    //   sizeChanged → popup_resize {popupId,w,h} (winW/winH = 窗口上报尺寸)
    //   posChanged  → popup_move {popupId,x,y}
    struct PopupCommitEvent {
        bool isNew = false;
        bool sizeChanged = false;
        bool posChanged = false;
        uint32_t popupId = 0;
        uint32_t parentId = 0;
        int32_t offX = 0, offY = 0;
        int32_t winW = 0, winH = 0;      // 窗口上报尺寸 (全屏父补丁后)
        int32_t dispW = 0, dispH = 0;    // 裁剪后内容显示尺寸 (仅 show 日志用)
        uint32_t shmFormat = 1;          // 0=ARGB8888 (show 日志 argb 位用)
    };

    // subsurface_set_position 移动的事件描述 (调用方据此锁外发 popup_move)
    struct PopupMoveEvent {
        uint32_t popupId = 0;
        uint32_t parentId = 0;
        int32_t offX = 0, offY = 0;
    };

    // -- popup 帧登记 (原 WaylandServer::UpdatePopupOnCommit 状态段整体迁入) --
    // PC 模式 subsurface commit: 裁剪计算 (vpSrc/vpDst 源矩形 + 显示尺寸封顶)
    // → 建档/更新 popup 记录 + 帧像素归档 (双缓冲轮换/紧凑裁剪) + 尺寸上报去重
    // (经 ToplevelManager::HandleCommittedSizeLocked)。自身持有 tmgr 锁段
    // (锁边界与迁移前相同: 无锁计算段 → 锁段 → 事件描述返回)。
    // 调用方 (wl_core UpdateSubsurfaceOnCommit) 按返回值恢复事件段并 fire。
    PopupCommitEvent UpdatePopupOnCommit(SurfaceData* sd, wl_resource* surfRes,
                                         SurfaceData* parentSd, ShmCommitInfo& fi);

    // 移动更新 (原 subsurface_set_position 内联段: FindPopupBySurfaceKey +
    // FindPopup + rec->offX/offY 更新)。调用方须已持有 tmgr 锁; 父内容原点
    // (parentContentX/Y) 由调用方从 parentSd->committed.contentRect 读出 (父
    // surface 几何属协议壳领域, 本模块不涉)。偏移计算经 ComputePopupOffset
    // (公式单点, 见 geometry.h)。返回 false = 该 surface 无 popup 记录 (调用
    // 方不发事件); true = out 已填 (调用方据 out 锁外 fire popup_move)。
    bool UpdatePopupPositionLocked(uint64_t surfaceKey, int32_t x, int32_t y,
                                   int32_t parentContentX, int32_t parentContentY,
                                   PopupMoveEvent& out);

    // -- 清理 (原 ToplevelManager::RemovePopupBySurfaceKeyLocked /
    //    RemovePopupDataLocked 迁移; 调用方须已持有 tmgr 锁) --
    // 按 surfaceKey 移除 popup 记录, 返回 parentToplevel (0 = 该 surface 非
    // popup); outPopupId 输出被移除的 popupId。调用方按其返回值锁外 fire
    // popup_hide (原弹点条件逐字)。
    uint32_t RemovePopupBySurfaceKeyLocked(uint64_t surfaceKey, uint32_t& outPopupId);
    // 按 popupId 移除记录 + 对称清理 popup 复用的 ToplevelState/surface 映射
    void RemovePopupDataLocked(uint32_t popupId);
    // 级联收集 (OnToplevelDestroyed): 遍历 popup 表找属于给定父 toplevel 的
    // 全部 popup id (返回顺序 = popups_ 遍历顺序, 与原 toplevelMgr_.popups()
    // 遍历一致 — 同一表同一 unordered_map 顺序)。清理与事件 fire 仍由调用方
    // 编排 (与原"锁内收集→锁内删除→锁外 popup_hide"逐字)。
    std::vector<uint32_t> CollectPopupIdsForParentLocked(uint32_t parentToplevel) const;

private:
    uint32_t FindPopupBySurfaceKey(uint64_t key) const {
        auto it = popupBySurfaceKey_.find(key);
        return it != popupBySurfaceKey_.end() ? it->second : 0;
    }
    PopupRecord* FindPopup(uint32_t popupId) {
        auto it = popups_.find(popupId);
        return it != popups_.end() ? &it->second : nullptr;
    }
    void RegisterPopup(uint32_t popupId, const PopupRecord& rec) {
        popups_[popupId] = rec;
        popupBySurfaceKey_[rec.surfaceKey] = popupId;
    }

    // popup 表 (调用方持有的 ToplevelManager 锁保护, 见类注释锁纪律)。
    // 原 toplevel_manager.h "以下成员由自己的 mutex 保护" 注释段下的 popup 表
    // 实为 toplevelMutex_ 保护 (所有调用点均持锁、方法名 Locked), 迁移时修正。
    std::unordered_map<uint32_t, PopupRecord> popups_;
    std::unordered_map<uint64_t, uint32_t> popupBySurfaceKey_;

    ToplevelManager& tmgr_;
    int32_t& outputW_;  // 与 DesktopCompositor 同款注入: 全屏父窗口尺寸补丁读点
    int32_t& outputH_;
};
