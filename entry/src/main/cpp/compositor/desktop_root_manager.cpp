#include "desktop_root_manager.h"
#include "toplevel_manager.h"
#include "compositor_constants.h"
#include "compositor/surface_data.h"
#include <hilog/log.h>

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0x0000
#define LOG_TAG "WL_Server"

DesktopRootManager::DesktopRootManager(ToplevelManager& tmgr,
                                       uint32_t& desktopRootToplevelId,
                                       uint32_t& pendingDesktopRootToplevelId,
                                       bool& recognitionEnabled,
                                       FireEventFn fireEvent)
    : tmgr_(tmgr)
    , desktopRootToplevelId_(desktopRootToplevelId)
    , pendingDesktopRootToplevelId_(pendingDesktopRootToplevelId)
    , recognitionEnabled_(recognitionEnabled)
    , fireEvent_(std::move(fireEvent))
{
}

// -- desktop root 识别 --
// Wine 侧对 #32769 窗口设置 app_id="explorer.exe.desktop-shell",
// compositor 精确匹配即可识别。 空 title 的 desktop-shell 是非活跃
// 的辅助窗口, 不作为 root 候选。

void DesktopRootManager::SetRecognitionEnabled(bool enabled)
{
    auto lk = tmgr_.Lock();
    recognitionEnabled_ = enabled;
    OH_LOG_INFO(LOG_APP, "[MW] desktop root recognition %{public}s",
                enabled ? "enabled" : "disabled");
}

uint32_t DesktopRootManager::PromotePending()
{
    uint32_t id = 0;
    {
        auto lk = tmgr_.Lock();
        id = pendingDesktopRootToplevelId_;
        auto* pst = id ? tmgr_.FindToplevelLocked(id) : nullptr;
        if (id == 0 || !pst || !pst->HasFrame()) {
            if (id != 0) {
                OH_LOG_WARN(LOG_APP, "[MW] pending desktop root #%{public}u has no pixels, skip", id);
                pendingDesktopRootToplevelId_ = 0;
            }
            return 0;
        }
        if (desktopRootToplevelId_ == id) {
            pendingDesktopRootToplevelId_ = 0;
            return 0;
        }
        if (desktopRootToplevelId_ > 0) {
            tmgr_.HideToplevelLocked(desktopRootToplevelId_);
        }
        tmgr_.ShowToplevelLocked(id);
        desktopRootToplevelId_ = id;
        pendingDesktopRootToplevelId_ = 0;
        pst->MarkDirty();
    }

    OH_LOG_INFO(LOG_APP, "[MW] pending desktop root promoted: #%{public}u", id);
    if (fireEvent_) fireEvent_(id, "desktop_root", "{}");
    return id;
}

void DesktopRootManager::MarkRootDirtyLocked()
{
    tmgr_.MarkToplevelDirtyLocked(desktopRootToplevelId_);
}

DesktopRootManager::CheckRootResult
DesktopRootManager::CheckRootLocked(SurfaceData* sd, bool isFirstCommit)
{
    CheckRootResult result;
    if (!isFirstCommit) return result;

    if (sd->appId != compositor_consts::kAppIdExplorerDesktopShell) return result;

    // 空 title 的 desktop-shell 是非活跃的辅助窗口, 永远不作为 root 候选。
    // (真 desktop 总会被 set_desktop_window_title 设置非空 title)
    if (sd->title.empty()) {
        tmgr_.HideToplevelLocked(sd->toplevelId);
        OH_LOG_INFO(LOG_APP, "[MW] desktop-shell #%{public}u title empty, hide as background",
                    sd->toplevelId);
        return result;
    }

    // --- 以下 sd 是真 desktop-shell ---

    uint32_t rootId = desktopRootToplevelId_;

    if (!recognitionEnabled_) {
        pendingDesktopRootToplevelId_ = sd->toplevelId;
        tmgr_.ShowToplevelLocked(sd->toplevelId);
        OH_LOG_INFO(LOG_APP,
                    "[MW] desktop-shell #%{public}u pending (recognition disabled)",
                    sd->toplevelId);
        return result;
    }

    if (rootId == 0) {
        if (pendingDesktopRootToplevelId_ > 0 &&
            pendingDesktopRootToplevelId_ != sd->toplevelId) {
            tmgr_.HideToplevelLocked(sd->toplevelId);
            OH_LOG_INFO(LOG_APP,
                        "[MW] desktop-shell #%{public}u -> background, pending root #%{public}u exists",
                        sd->toplevelId, pendingDesktopRootToplevelId_);
        } else {
            OH_LOG_INFO(LOG_APP, "[MW] desktop root: #%{public}u appId=explorer.exe.desktop-shell",
                        sd->toplevelId);
            result.moveRendererTo = sd->toplevelId;
            desktopRootToplevelId_ = sd->toplevelId;
            pendingDesktopRootToplevelId_ = 0;
            result.fireDesktopRoot = true;
        }
        return result;
    }

    // root 已存在, 新来的真 desktop-shell 替换旧 root
    OH_LOG_INFO(LOG_APP, "[MW] root switch: #%{public}u -> #%{public}u",
                rootId, sd->toplevelId);
    tmgr_.HideToplevelLocked(rootId);
    result.moveRendererFrom = rootId;
    result.moveRendererTo = sd->toplevelId;
    desktopRootToplevelId_ = sd->toplevelId;
    result.fireDesktopRoot = true;
    return result;
}
