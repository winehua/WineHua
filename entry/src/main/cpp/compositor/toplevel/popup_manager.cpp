#include "popup_manager.h"
#include "toplevel_manager.h"
#include "surface_data.h"
#include "shm_frame_source.h"
#include "geometry.h"
#include <algorithm>
#include <cstring>
#include <cstdio>

#undef LOG_TAG
#undef LOG_DOMAIN
#define LOG_DOMAIN 0x0000
#define LOG_TAG "WL_Server"
#include <hilog/log.h>

PopupManager::PopupManager(ToplevelManager& tmgr, int32_t& outputW, int32_t& outputH)
    : tmgr_(tmgr), outputW_(outputW), outputH_(outputH) {
}

PopupManager::PopupCommitEvent PopupManager::UpdatePopupOnCommit(
    SurfaceData* sd, wl_resource* surfRes, SurfaceData* parentSd, ShmCommitInfo& fi) {
    PopupCommitEvent ev;
    uint32_t parentId = parentSd->toplevelId;
    /*
     * wp_viewport: Wine popup 的 shm buffer 常按 2 的幂次对齐
     * 填充, 大于真实菜单内容。真实显示尺寸 =
     * min(buffer, set_source, set_destination), 从源矩形原点裁剪
     * (与 desktop 路径 vpDst clamp / toplevel 的 window_geometry
     * 裁剪同语义)。
     *
     * 风险标注 (P2): 父 toplevel 销毁后 popup 会被级联清理
     * (OnToplevelDestroyed), 但若该 popup surface 在父窗口销毁后
     * 恰好又 commit 一帧, 会在此重新登记并向已销毁的 parentId 发
     * popup_show, ArkTS 侧因窗口不存在而积压 (竞态极小, 仅微量
     * 内存)。如需根治可在此处检查 parentId 是否仍存活。
     */
    // 父几何读点 (重构第 5A2 步): 旧读 parentSd->geoX/geoY (即时窗口几何值),
    // 新读 parentSd->committed.contentRect.x/y — 同一写入点 (xs_set_window_geometry
    // 直写快照) 的同步表达式, 逐点等价; 偏移公式语义不变 (相对父内容原点)。
    // 偏移公式收口 (重构第 5B2 步): 经 geometry.h ComputePopupOffset 单点
    // (PLAN §2.3 4 份公式), 算法逐字 (offX = subX - parentContentX)。
    const auto [offX, offY] = ComputePopupOffset(sd->subsurfaceX, sd->subsurfaceY,
                                                 parentSd->committed.contentRect.x,
                                                 parentSd->committed.contentRect.y);
    int dispW = sd->w, dispH = sd->h;
    int cropX = 0, cropY = 0;
    if (sd->vpSrcW > 0 && sd->vpSrcH > 0) {
        cropX = std::max(0, std::min(sd->vpSrcX, sd->w - 1));
        cropY = std::max(0, std::min(sd->vpSrcY, sd->h - 1));
        if (sd->vpSrcW < dispW) dispW = sd->vpSrcW;
        if (sd->vpSrcH < dispH) dispH = sd->vpSrcH;
    }
    if (sd->vpDstW > 0 && sd->vpDstW < dispW) dispW = sd->vpDstW;
    if (sd->vpDstH > 0 && sd->vpDstH < dispH) dispH = sd->vpDstH;
    dispW = std::min(dispW, sd->w - cropX);
    dispH = std::min(dispH, sd->h - cropY);
    // 防御: pixels 须为完整 w*h*4 (subsurface 若设 window_geometry
    // 则 sd->w/h 是 content 尺寸而 pixels 是全 buffer, 不成立)
    const size_t expectSz = static_cast<size_t>(sd->w) * sd->h * 4;
    if (sd->pixels.size() < expectSz) {
        OH_LOG_WARN(LOG_APP, "[MW-POPUP] pixels size mismatch: %{public}zu < %{public}zu (w=%{public}d h=%{public}d), skip frame",
                    sd->pixels.size(), expectSz, sd->w, sd->h);
        return ev;
    }
    if (dispW <= 0 || dispH <= 0) return ev;
    uint32_t popupId = 0;
    bool isNew = false;
    bool sizeChanged = false;
    bool posChanged = false;
    /*
     * 全屏主窗口的 GL client surface (war3 D3D 模式切换): wine 把客户区
     * MoveWindow 到模式尺寸 (800x600), client surface 随之缩小, 按 1:1
     * 上报会把画面缩在屏幕左上角。这里把"窗口上报尺寸"与"内容像素尺寸"
     * 解耦: 窗口按全屏输出尺寸上报, FrameData 仍按内容尺寸存 — 渲染侧
     * EglRenderer letterbox 保比例放大上屏, 输入侧 CoordTransform 按同
     * 一 letterbox 逆映射 (与 RA2 主 surface 全屏路径同构)。
     * 判定 = 父全屏 + 偏移 (0,0) + 内容尺寸等于父内容尺寸 (client
     * surface 恰好覆盖整个客户区; 菜单等小 popup 不满足, 不受影响)。
     * 本函数仅 PC 模式到达 (桌面模式走 layer 合成), 不影响 Pad 桌面。
     * 补丁来源: PLAN §2.5 "wl_core.cpp:974-995 popup 窗口/内容尺寸解耦"。
     */
    int winW = dispW, winH = dispH;
    {
        auto lk = tmgr_.Lock();
        auto* pst = tmgr_.FindToplevelLocked(parentId);
        if (pst && pst->IsFullscreen() && offX == 0 && offY == 0 &&
            dispW == pst->Width() && dispH == pst->Height() &&
            outputW_ > 0 && outputH_ > 0 &&
            (dispW < outputW_ || dispH < outputH_)) {
            winW = outputW_;
            winH = outputH_;
        }
        popupId = FindPopupBySurfaceKey(sd->surfaceKey);
        if (popupId == 0) {
            popupId = tmgr_.AllocateToplevelId();
            isNew = true;
            PopupRecord rec;
            rec.popupId = popupId;
            rec.parentToplevel = parentId;
            rec.surface = surfRes;
            rec.surfaceKey = sd->surfaceKey;
            rec.offX = offX;
            rec.offY = offY;
            RegisterPopup(popupId, rec);
        } else {
            auto* rec = FindPopup(popupId);
            if (!rec) {
                // 两表不同步 (不应发生): 清孤儿 key, 跳过本帧, 下帧重建
                popupId = 0;
            } else {
                posChanged = (rec->offX != offX || rec->offY != offY);
                rec->offX = offX;
                rec->offY = offY;
            }
        }
        if (popupId > 0) {
            auto& pbuf = tmgr_.EnsureToplevelLocked(popupId);
            auto& buf = pbuf.FrameData();
            if (cropX == 0 && cropY == 0 && dispW == sd->w && dispH == sd->h) {
                // 无裁剪: 像素双缓冲轮换 (同 desktop layer 做法)
                auto reusablePixels = std::move(buf);
                buf = std::move(sd->pixels);
                sd->pixels = std::move(reusablePixels);
            } else {
                // 裁剪出真实内容区域 (紧凑排列)
                buf.resize(static_cast<size_t>(dispW) * dispH * 4);
                for (int y = 0; y < dispH; y++) {
                    std::memcpy(buf.data() + static_cast<size_t>(y) * dispW * 4,
                                sd->pixels.data() + (static_cast<size_t>(cropY + y) * sd->w + cropX) * 4,
                                static_cast<size_t>(dispW) * 4);
                }
            }
            pbuf.SetContentSize(dispW, dispH);
            pbuf.MarkDirty();
            pbuf.BumpFrameSerial();  // 帧序列号语义: 像素轮换重写即递增
            pbuf.SetShmFormat(fi.shmFormat);
            // 尺寸上报语义 (重构第 5B2 步): 原 sizeChanged 用 PopupRecord::w/h
            // 与"窗口上报尺寸"比较, 现改经 popup 自身 ToplevelState 的尺寸上报
            // 去重通道 (5B1 收口的 HandleCommittedSizeLocked, 传自身 id/rootId=0):
            // - 判定值逐字 = winW/H (全屏父补丁后的窗口上报尺寸), 去重状态
            //   记录在 ToplevelState::lastReportedW_/H (随 popup 生命周期复位,
            //   与原 rec->w/h 同语义);
            // - popup 从不 SetToplevelFullscreen → 漂移分支 (ReassertFullscreen,
            //   war3 主窗口补丁) 不触发; isNew 首帧调用仅为播种 lastReported
            //   (等价旧"建档时 rec->w/h = winW"), 其 ResizeEvent 返回值被
            //   isNew 吞掉 — 与旧"新 popup 只发 show, 第二帧同尺寸不发 resize"
            //   逐帧一致;
            // - 旧 rec->w/h 随此通道删除 (零外部消费, 见 PopupRecord 注释)。
            const auto sizeEffect = tmgr_.HandleCommittedSizeLocked(
                popupId, 0, winW, winH, outputW_, outputH_);
            sizeChanged = !isNew &&
                sizeEffect == ToplevelManager::SizeCommitEffect::ResizeEvent;
        }
    }
    if (popupId == 0) {
        // 记录异常, 跳过本帧 (下帧按新 popup 重建)
        return ev;
    }
    if (isNew) {
        // surface 映射注册 (原事件段第一动作, 与 popup_show fire 的相对顺序
        // 不变 — fire 侧接到 isNew 后发事件)
        tmgr_.MapToplevelSurface(popupId, surfRes);
    }
    ev.isNew = isNew;
    ev.sizeChanged = sizeChanged;
    ev.posChanged = posChanged;
    ev.popupId = popupId;
    ev.parentId = parentId;
    ev.offX = offX;
    ev.offY = offY;
    ev.winW = winW;
    ev.winH = winH;
    ev.dispW = dispW;
    ev.dispH = dispH;
    ev.shmFormat = fi.shmFormat;
    return ev;
}

bool PopupManager::UpdatePopupPositionLocked(uint64_t surfaceKey, int32_t x, int32_t y,
                                             int32_t parentContentX, int32_t parentContentY,
                                             PopupMoveEvent& out) {
    // 原 subsurface_set_position popup_move 内联段 (锁内: FindPopupBySurfaceKey
    // + FindPopup + rec->offX/offY 更新); 偏移公式收口 (重构第 5B2 步): 经
    // geometry.h ComputePopupOffset, 算法逐字 (offX = x - parentContentX)。
    const uint32_t popupId = FindPopupBySurfaceKey(surfaceKey);
    auto* rec = FindPopup(popupId);
    if (!rec) return false;
    const auto [newOffX, newOffY] = ComputePopupOffset(x, y, parentContentX, parentContentY);
    rec->offX = newOffX;
    rec->offY = newOffY;
    out.popupId = rec->popupId;
    out.parentId = rec->parentToplevel;
    out.offX = rec->offX;
    out.offY = rec->offY;
    return true;
}

uint32_t PopupManager::RemovePopupBySurfaceKeyLocked(uint64_t surfaceKey, uint32_t& outPopupId) {
    auto it = popupBySurfaceKey_.find(surfaceKey);
    if (it == popupBySurfaceKey_.end()) return 0;
    outPopupId = it->second;
    uint32_t parentToplevel = 0;
    auto popupIt = popups_.find(outPopupId);
    if (popupIt != popups_.end()) parentToplevel = popupIt->second.parentToplevel;
    RemovePopupDataLocked(outPopupId);
    return parentToplevel;
}

void PopupManager::RemovePopupDataLocked(uint32_t popupId) {
    auto popupIt = popups_.find(popupId);
    if (popupIt == popups_.end()) return;

    // 清理 surfaceKey → popupId 映射
    auto keyIt = popupBySurfaceKey_.find(popupIt->second.surfaceKey);
    if (keyIt != popupBySurfaceKey_.end() && keyIt->second == popupId)
        popupBySurfaceKey_.erase(keyIt);

    // 清理 toplevels_ 中 popup 复用的帧数据 (原 ToplevelManager 方法内经
    // toplevels_ 私有表; 现经公开 EraseToplevelLocked, 语义逐字)
    tmgr_.EraseToplevelLocked(popupId);

    // 清理 toplevelSurfaceMap_ 中的 popup 条目 (经公开 UnmapToplevelSurface,
    // 内部嵌套取 toplevelSurfaceMutex_ — 锁序与迁移前逐字:
    // toplevelMutex_ → toplevelSurfaceMutex_)
    tmgr_.UnmapToplevelSurface(popupId);

    popups_.erase(popupIt);
}

std::vector<uint32_t> PopupManager::CollectPopupIdsForParentLocked(uint32_t parentToplevel) const {
    std::vector<uint32_t> ids;
    for (const auto& [pid, rec] : popups_) {
        if (rec.parentToplevel == parentToplevel) ids.push_back(pid);
    }
    return ids;
}
