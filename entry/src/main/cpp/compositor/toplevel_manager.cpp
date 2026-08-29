#include "toplevel_manager.h"
#include "debug_assert.h"
#include "compositor_utils.h"    // IsRestoreSizeCommit (最小化自动恢复判定)
#include "compositor_constants.h"  // 掩码阈值/FNV 常数/最小化坐标阈值
#include <hilog/log.h>

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0x0000
#define LOG_TAG "WL_Server"

// -- commit 业务段语义收口 (重构第 5B1 步) --
//
// 本节五个方法原为 wl_core.cpp UpdateToplevelFrameOnCommit 内联段 (恢复/
// ARGB 位置同步/桌面位置同步/ARGB 掩码/尺寸上报), 业务段各归其主收进本模块。
// 每方法体与原 wl_core 段逐语句等价 (判定逐字、日志文本/顺序/条件逐字、补丁
// 注释完整平移 — PLAN §2.5); 调用方已持有 toplevelMutex_, 锁域不含变化。

bool ToplevelManager::TryAutoRestoreLocked(uint32_t id, int32_t contentW, int32_t contentH) {
    auto* st = FindToplevelLocked(id);
    if (!st) return false;  // 调用点建档后必有 (防御)
    /*
     * 自动恢复最小化窗口: 判定逻辑见 IsRestoreSizeCommit (compositor_utils.h)。
     * 注意: 此处已持有 toplevelMutex_, 不能调 SetToplevelRestored。
     * justRestored: 还原帧的 geo 是 Wine 记录的"原位" — 用户拖动过窗口
     * (move grab 只改 compositor 坐标, Wine 不知道) 时原位是旧的, 下方
     * 位置跟随必须跳过 (见 SyncDesktopPositionLocked 的 wine geo sync 分支)。
     */
    if (IsRestoreSizeCommit(st->IsMinimized(), contentW, contentH)) {
        st->SetMinimized(false);
        OH_LOG_INFO(LOG_APP, "[MW] auto-restore tl=%{public}u size=%{public}dx%{public}d",
                    id, contentW, contentH);
        return true;
    }
    return false;
}

bool ToplevelManager::SyncArgbPositionLocked(uint32_t id, int32_t screenX, int32_t screenY) {
    auto* st = FindToplevelLocked(id);
    if (!st) return false;  // 调用点建档后必有 (防御)
    /*
     * ARGB 窗口: Wine 位置为权威 (桌面小部件由 Wine 决定屏幕位置)。
     * 普通 PC 窗口后续 commit 忽略 geoX/geoY (OHOS 窗口管理器为权威),
     * ARGB 窗口相反: geo 变化 → 通知 ArkTS 移动子窗口。
     * (历史字段 geoX/geoY 已于重构第 5A2 步消亡 — 其"桌面屏幕位置"义即
     * 本方法接收的 screenX/Y, 由 CommittedSurface::screenPos 命名承载)
     */
    if (st->X() != screenX || st->Y() != screenY) {
        st->SetPosition(screenX, screenY);
        return true;
    }
    return false;
}

void ToplevelManager::SyncDesktopPositionLocked(uint32_t id, int32_t screenX, int32_t screenY,
                                                bool justRestored) {
    auto* st = FindToplevelLocked(id);
    if (!st) return;  // 调用点建档后必有 (防御)
    if (screenX != st->WineX() || screenY != st->WineY()) {
        if (justRestored) {
            /*
             * 还原帧: 保持 compositor 位置 (用户可能拖动过, Wine 不知道
             * 新位置, 其 geo 是旧原位 — 实测还原回 (0,0) 而非拖动位置)。
             * 只同步 Wine 快照, 后续 commit (geo==快照) 不再误触发。
             */
            st->SetWinePosition(screenX, screenY);
            OH_LOG_INFO(LOG_APP, "[MW-MOVE] restore keep pos tl=%{public}u (%{public}d,%{public}d) wine=(%{public}d,%{public}d)",
                        id, st->X(), st->Y(), screenX, screenY);
        } else if (screenX > -compositor_consts::kMinimizedCoordThreshold &&
                   screenY > -compositor_consts::kMinimizedCoordThreshold) {
            st->SetPosition(screenX, screenY);
            st->SetWinePosition(screenX, screenY);
            OH_LOG_INFO(LOG_APP, "[MW-MOVE] wine geo sync tl=%{public}u (%{public}d,%{public}d)",
                        id, screenX, screenY);
        } else {
            st->SetWinePosition(screenX, screenY);
        }
    }
}

bool ToplevelManager::UpdateArgbMaskLocked(uint32_t id, const std::vector<uint8_t>& pixels,
                                           int32_t w, int32_t h) {
    auto* st = FindToplevelLocked(id);
    if (!st) return false;  // 调用点建档后必有 (防御)
    /*
     * ARGB 窗口: 从 alpha 通道生成 0/1 剪影掩码 (setWindowMask 用)。
     * - 阈值 128: 半透明抗锯齿边缘向内收半像素, 避免灰边外扩
     * - 形状哈希没变就不重建: 时钟类静态形状零开销,
     *   动画类 (桌面宠物) 每帧变形才按帧重算
     * - 掩码是帧分辨率 (Wine 逻辑像素); setWindowMask 要求等于
     *   窗口物理尺寸, ArkTS 侧按 effectiveScale 最近邻放大
     */
    const size_t pixCount = static_cast<size_t>(w) * h;
    uint64_t hash = compositor_consts::kFnv1aOffsetBasis;
    for (size_t i = 3; i < pixCount * 4; i += 4) {
        hash ^= (pixels[i] >= compositor_consts::kArgbMaskAlphaThreshold) ? 1 : 0;
        hash *= compositor_consts::kFnv1aPrime;
    }
    auto& m = st->MutableMask();
    if (hash != m.hash || m.w != w || m.h != h) {
        m.hash = hash;
        m.w = w;
        m.h = h;
        m.bits.resize(pixCount);
        for (size_t i = 0; i < pixCount; i++) {
            m.bits[i] = (pixels[i * 4 + 3] >= compositor_consts::kArgbMaskAlphaThreshold) ? 1 : 0;
        }
        m.dirty = true;
        return true;
    }
    return false;
}

ToplevelManager::SizeCommitEffect ToplevelManager::HandleCommittedSizeLocked(
    uint32_t id, uint32_t rootId, int32_t contentW, int32_t contentH,
    int32_t outputW, int32_t outputH) {
    auto* st = FindToplevelLocked(id);
    if (!st) return SizeCommitEffect::None;  // 调用点建档后必有 (防御)
    // 检测尺寸变化 -> 通知 ArkTS 调整子窗口
    if (!st->CheckAndUpdateLastReportedSize(contentW, contentH))
        return SizeCommitEffect::None;
    /*
     * fullscreen 纠偏: D3D 游戏的显示模式切换会把窗口 MoveWindow 到
     * 模式尺寸 (war3: 1560x1040 → 800x600), 内容几何随之缩小。此时
     * resize 转发无意义 (系统本就拒绝 fullscreen 窗口 resize), 真正
     * 需要的是把 wine 窗口拉回 configure 尺寸 — 否则 GL client
     * surface 按 800x600 客户区出帧, 经 popup 路径原样上屏, 画面
     * 缩到左上。重发 fullscreen configure 后 wine 客户区恢复全屏,
     * client surface 跟随, wined3d 内部把模式尺寸 backbuffer 拉伸
     * 出帧 (与 RA2 的 GDI 主 surface viewport 拉伸殊途同归)。
     * 非 fullscreen / 尺寸不小于输出 / 桌面 root: 维持原转发语义。
     * 补丁来源: PLAN §2.5 "wl_core.cpp:766-795 全屏尺寸漂移重发 configure"
     * 自死锁修复记录: 重发必须由调用方在锁外执行 — NotifyToplevelResize
     * 内部 IsToplevelFullscreen 会再取 toplevelMutex_ (非递归 std::mutex),
     * 持锁调用 = 同线程自死锁, wayland 事件循环卡死, 输入/帧派发全停
     * (APP_INPUT_BLOCK, 2026-08-15 war3 全屏黑屏整机卡死的根因)。解锁后
     * 不再触碰本 toplevel 状态。
     */
    if (st->IsFullscreen() && id != rootId && (contentW < outputW || contentH < outputH)) {
        OH_LOG_INFO(LOG_APP, "[MW] toplevel #%{public}u fullscreen size drift %{public}dx%{public}d < output %{public}dx%{public}d -> re-assert configure",
                    id, contentW, contentH, outputW, outputH);
        return SizeCommitEffect::ReassertFullscreen;
    }
    return SizeCommitEffect::ResizeEvent;
}

// -- ToplevelState 语义方法 --

void ToplevelManager::ToplevelState::ApplyFullscreen(bool on) {
    fullscreen_ = on;
    // 全屏窗口锚定桌面原点: 合成按保比例缩放铺满, 不再使用浮动位置
    if (on && hasPosition_) AnchorToOrigin();
    // 不变式守卫 (类注释): 全屏 toplevel 锚定 (0,0)
    MW_ASSERT(!on || !hasPosition_ || (x_ == 0 && y_ == 0),
              "fullscreen toplevel must be anchored at (0,0)");
}

bool ToplevelManager::ToplevelState::TakeMask(WindowMask& out) {
    if (mask_.w == 0 || !mask_.dirty) return false;
    out.w = mask_.w;
    out.h = mask_.h;
    out.bits = std::move(mask_.bits);
    mask_.dirty = false;
    return true;
}

// -- 渲染/输入共用的 toplevel 可见性检查 --

bool ToplevelManager::IsToplevelVisibleLocked(uint32_t id, uint32_t desktopRootId) {
    // 桌面 root 是合成目标而非内容, 永远不作为可见 toplevel 参与合成/命中。
    // RaiseToplevel 不排斥 root (点击桌面时 ArkTS 会 raise rootId), root 可能
    // 进入 z-order 栈顶 — 若此处不排除, 整屏不透明的 root 会盖住所有窗口和
    // 任务栏, 且命中判定全部落在 root 上 (桌面"仅剩背景"回归的根因)
    if (id == desktopRootId) return false;
    auto it = toplevels_.find(id);
    if (it == toplevels_.end()) return false;
    const auto& st = it->second;
    // 被切换掉的旧 root (isBackground) 对渲染和输入都不可见
    return !st.IsBackground() && st.HasFrame() && !st.IsMinimized();
}

// -- popup 数据清理已迁至 PopupManager (compositor/popup_manager.{h,cpp},
//    重构第 5B2 步: popup 表/清理方法/级联收集随 PopupManager 拆出) --

// -- toplevel 状态查询 --

bool ToplevelManager::IsToplevelMinimized(uint32_t id) {
    auto lk = Lock();
    auto it = toplevels_.find(id);
    return it != toplevels_.end() && it->second.IsMinimized();
}

bool ToplevelManager::IsToplevelFullscreen(uint32_t id) {
    auto lk = Lock();
    auto it = toplevels_.find(id);
    return it != toplevels_.end() && it->second.IsFullscreen();
}

// -- resource 映射 --

void ToplevelManager::RegisterToplevelResource(uint32_t id, wl_resource* tl) {
    std::lock_guard<std::mutex> lk(toplevelResMutex_);
    toplevelResources_[id] = tl;
}

void ToplevelManager::UnregisterToplevelResource(uint32_t id) {
    std::lock_guard<std::mutex> lk(toplevelResMutex_);
    toplevelResources_.erase(id);
}

wl_resource* ToplevelManager::FindToplevelResource(uint32_t id) {
    std::lock_guard<std::mutex> lk(toplevelResMutex_);
    auto it = toplevelResources_.find(id);
    return it != toplevelResources_.end() ? it->second : nullptr;
}

void ToplevelManager::MapToplevelSurface(uint32_t id, wl_resource* surf) {
    std::lock_guard<std::mutex> lk(toplevelSurfaceMutex_);
    toplevelSurfaceMap_[id] = surf;
}

void ToplevelManager::UnmapToplevelSurface(uint32_t id) {
    std::lock_guard<std::mutex> lk(toplevelSurfaceMutex_);
    toplevelSurfaceMap_.erase(id);
}

wl_resource* ToplevelManager::GetSurfaceForToplevel(uint32_t id) {
    std::lock_guard<std::mutex> lk(toplevelSurfaceMutex_);
    auto it = toplevelSurfaceMap_.find(id);
    return it != toplevelSurfaceMap_.end() ? it->second : nullptr;
}

uint32_t ToplevelManager::FindToplevelBySurface(wl_resource* surf) {
    if (!surf) return 0;
    std::lock_guard<std::mutex> lk(toplevelSurfaceMutex_);
    for (const auto& [id, s] : toplevelSurfaceMap_)
        if (s == surf) return id;
    return 0;
}

bool ToplevelManager::TakeWindowMask(uint32_t id, int& w, int& h, std::vector<uint8_t>& out) {
    auto lk = Lock();
    auto it = toplevels_.find(id);
    if (it == toplevels_.end()) return false;
    WindowMask m;
    if (!it->second.TakeMask(m)) return false;
    w = m.w;
    h = m.h;
    out = std::move(m.bits);
    return true;
}

// -- 诊断数据访问 --

void ToplevelManager::MarkToplevelDirtyLocked(uint32_t id) {
    if (id == 0) return;
    auto it = toplevels_.find(id);
    if (it != toplevels_.end()) it->second.MarkDirty();
}

int ToplevelManager::GetToplevelX(uint32_t id) {
    auto lk = Lock();
    auto it = toplevels_.find(id);
    return it != toplevels_.end() ? it->second.X() : 0;
}

int ToplevelManager::GetToplevelY(uint32_t id) {
    auto lk = Lock();
    auto it = toplevels_.find(id);
    return it != toplevels_.end() ? it->second.Y() : 0;
}

int ToplevelManager::GetToplevelW(uint32_t id) {
    auto lk = Lock();
    auto it = toplevels_.find(id);
    return it != toplevels_.end() ? it->second.Width() : 0;
}

int ToplevelManager::GetToplevelH(uint32_t id) {
    auto lk = Lock();
    auto it = toplevels_.find(id);
    return it != toplevels_.end() ? it->second.Height() : 0;
}

ToplevelManager::ToplevelGeometrySnapshot ToplevelManager::GetToplevelGeometrySnapshot(uint32_t id) {
    auto lk = Lock();
    ToplevelGeometrySnapshot s;
    auto it = toplevels_.find(id);
    if (it != toplevels_.end()) {
        s.x = it->second.X();
        s.y = it->second.Y();
        s.w = it->second.Width();
        s.h = it->second.Height();
        s.shmFormat = it->second.ShmFormat();
    }
    return s;
}
