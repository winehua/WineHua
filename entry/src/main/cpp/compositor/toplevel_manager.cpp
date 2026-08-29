#include "toplevel_manager.h"
#include "debug_assert.h"
#include "compositor_utils.h"    // IsRestoreSizeCommit (最小化自动恢复判定)
#include "compositor_constants.h"  // 掩码阈值/FNV 常数/最小化坐标阈值
#include <hilog/log.h>

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0x0000
#define LOG_TAG "WL_Server"


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

// -- popup 数据清理 --

void ToplevelManager::RemovePopupDataLocked(uint32_t popupId) {
    auto popupIt = popups_.find(popupId);
    if (popupIt == popups_.end()) return;

    // 清理 surfaceKey → popupId 映射
    auto keyIt = popupBySurfaceKey_.find(popupIt->second.surfaceKey);
    if (keyIt != popupBySurfaceKey_.end() && keyIt->second == popupId)
        popupBySurfaceKey_.erase(keyIt);

    // 清理 toplevels_ 中 popup 复用的帧数据
    auto tlIt = toplevels_.find(popupId);
    if (tlIt != toplevels_.end()) toplevels_.erase(tlIt);

    // 清理 toplevelSurfaceMap_ 中的 popup 条目
    {
        std::lock_guard<std::mutex> lk(toplevelSurfaceMutex_);
        auto surfIt = toplevelSurfaceMap_.find(popupId);
        if (surfIt != toplevelSurfaceMap_.end()) toplevelSurfaceMap_.erase(surfIt);
    }

    popups_.erase(popupIt);
}

uint32_t ToplevelManager::RemovePopupBySurfaceKeyLocked(uint64_t surfaceKey, uint32_t& outPopupId) {
    auto it = popupBySurfaceKey_.find(surfaceKey);
    if (it == popupBySurfaceKey_.end()) return 0;
    outPopupId = it->second;
    uint32_t parentToplevel = 0;
    auto popupIt = popups_.find(outPopupId);
    if (popupIt != popups_.end()) parentToplevel = popupIt->second.parentToplevel;
    RemovePopupDataLocked(outPopupId);
    return parentToplevel;
}

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
