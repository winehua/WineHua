#include "move_grab.h"
#include "toplevel_manager.h"
#include <hilog/log.h>
#include <wayland-server-core.h>

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0x0000
#define LOG_TAG "WL_Server"

void MoveGrabHandler::StartMoveGrab(ToplevelManager& tmgr, uint32_t toplevelId, uint32_t serial,
                                    int32_t grabGlobalX, int32_t grabGlobalY) {
    auto lk = tmgr.Lock();
    toplevelId_ = toplevelId;
    serial_ = serial;
    // 固定 grab 偏移 = 按下时指针全局位置 − 窗口位置, 后续 motion 绝对定位
    auto* st = tmgr.FindToplevelLocked(toplevelId);
    if (st && st->HasPosition()) {
        grabOffX_ = grabGlobalX - st->X();
        grabOffY_ = grabGlobalY - st->Y();
    } else {
        // 窗口状态异常 (grab 建立瞬间窗口还没位置): 记录当前全局位置,
        // 第一个 motion 会落到错误位置, 但绝对定位下一帧即自收敛
        grabOffX_ = grabGlobalX;
        grabOffY_ = grabGlobalY;
    }
    OH_LOG_INFO(LOG_APP, "[MW-MOVE] start interactive move tl=%{public}u serial=%{public}u"
                " grabOff=(%{public}d,%{public}d)",
                toplevelId, serial, grabOffX_, grabOffY_);
}

void MoveGrabHandler::EndMoveGrab(ToplevelManager& tmgr) {
    OH_LOG_INFO(LOG_APP, "[MW-MOVE] end interactive move tl=%{public}u", toplevelId_);
    {
        auto lk = tmgr.Lock();
        toplevelId_ = 0;
        serial_ = 0;
        grabOffX_ = 0;
        grabOffY_ = 0;
    }
}

bool MoveGrabHandler::ProcessMoveGrabMotion(ToplevelManager& tmgr, int32_t gx, int32_t gy) {
    auto lk = tmgr.Lock();
    if (toplevelId_ == 0) return false;
    auto* st = tmgr.FindToplevelLocked(toplevelId_);
    if (!st || !st->HasPosition()) return false;

    // 绝对定位, 无累积。增量式 (每帧 st->x += 指针增量) 在双线程下会把
    // 上帧自身位移叠进下一帧位移 — InputManager 注入局部坐标与消费侧
    // 读 st->x 还原的基准漂移, 快速拖动时窗口位移按帧累积放大直至飞出屏幕
    const int32_t nx = gx - grabOffX_;
    const int32_t ny = gy - grabOffY_;
    if (nx != st->X() || ny != st->Y()) {
        st->SetPosition(nx, ny);
        OH_LOG_INFO(LOG_APP, "[MW-MOVE] grab move tl=%{public}u ptr=(%{public}d,%{public}d) newPos=(%{public}d,%{public}d)",
                    toplevelId_, gx, gy, st->X(), st->Y());
    }
    return true;
}
