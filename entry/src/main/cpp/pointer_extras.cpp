#include "pointer_extras.h"

#include "include/pointer-constraints-unstable-v1-server-protocol.h"
#include "include/pointer-warp-v1-server-protocol.h"
#include "include/relative-pointer-unstable-v1-server-protocol.h"
#include "input_manager.h"
#include "wayland_server.h"

#include <algorithm>
#include <chrono>

#include <window_manager/oh_window.h>

#undef LOG_TAG
#define LOG_TAG "WL_PtrExt"
#include <hilog/log.h>

// ========================================================================
//  单例 / 注册
// ========================================================================

PointerExtras* PointerExtras::GetInstance() {
    static PointerExtras s;
    return &s;
}

void PointerExtras::Register(wl_display* display) {
    // constraints + warp + relative_pointer_manager 一并注册 (职责见头注释)。
    // warp 的发送只依赖 wp_pointer_warp_v1 全局存在
    // (wayland_pointer.c: pending_warp && wp_pointer_warp_v1 才发出请求);
    // relative_pointer 对象由 wine 按相对模式需要自行创建。
    wl_global_create(display, &zwp_pointer_constraints_v1_interface, 1,
                     this, constraints_bind);
    wl_global_create(display, &wp_pointer_warp_v1_interface, 1,
                     this, warp_bind);
    wl_global_create(display, &zwp_relative_pointer_manager_v1_interface, 1,
                     this, relmgr_bind);
    OH_LOG_INFO(LOG_APP, "[PtrExt] constraints+warp+relative registered");
}

// ========================================================================
//  zwp_pointer_constraints_v1 (lock / confine)
// ========================================================================

static const struct zwp_pointer_constraints_v1_interface kConstraintsImpl = {
    .destroy = PointerExtras::constr_destroy,
    .lock_pointer = PointerExtras::constr_lock_pointer,
    .confine_pointer = PointerExtras::constr_confine_pointer,
};

static const struct zwp_locked_pointer_v1_interface kLockedImpl = {
    .destroy = PointerExtras::locked_destroy,
    .set_cursor_position_hint = PointerExtras::locked_set_cursor_position_hint,
    .set_region = PointerExtras::locked_set_region,
};

static const struct zwp_confined_pointer_v1_interface kConfinedImpl = {
    .destroy = PointerExtras::confined_destroy,
    .set_region = PointerExtras::confined_set_region,
};

void PointerExtras::constraints_bind(wl_client* client, void* data,
                                     uint32_t version, uint32_t id) {
    wl_resource* res = wl_resource_create(client, &zwp_pointer_constraints_v1_interface,
                                          std::min(version, 1u), id);
    wl_resource_set_implementation(res, &kConstraintsImpl, data, nullptr);
}

void PointerExtras::constr_lock_pointer(wl_client*, wl_resource*, uint32_t id,
                                        wl_resource* surface, wl_resource* pointer,
                                        wl_resource* region, uint32_t lifetime) {
    auto* self = GetInstance();
    wl_resource* res = wl_resource_create(wl_resource_get_client(pointer),
                                          &zwp_locked_pointer_v1_interface, 1, id);
    wl_resource_set_implementation(res, &kLockedImpl, nullptr,
        [](wl_resource* r) { OnConstraintResourceDestroyed(r); });
    {
        std::lock_guard<std::mutex> lk(self->mutex_);
        // 同 surface 旧约束直接替换 (协议本应报错, 宽容处理: Wine 重建前会先销毁)
        auto& v = self->constraints_;
        v.erase(std::remove_if(v.begin(), v.end(),
                               [&](const Constraint& c) { return c.surface == surface; }),
                v.end());
        Constraint c;
        c.type = ConstraintType::Lock;
        c.surface = surface;
        c.res = res;
        v.push_back(c);
    }
    // Wine 总在 surface 有焦点时请求 → 立即激活 (参照 weston: focus 满足即激活)
    zwp_locked_pointer_v1_send_locked(res);
    OH_LOG_INFO(LOG_APP, "[PtrExt] LOCK pointer on surf=%{public}p lifetime=%{public}u",
                static_cast<void*>(surface), lifetime);
    // Lock 约束 = 游戏进入相对模式 (隐藏光标无限转视角) — 同步冻结+隐藏
    // host 系统光标 (见 pointer_extras.h 注释)
    self->ApplyHostCursorLock(true);
}

void PointerExtras::constr_confine_pointer(wl_client*, wl_resource*, uint32_t id,
                                           wl_resource* surface, wl_resource* pointer,
                                           wl_resource* region, uint32_t lifetime) {
    auto* self = GetInstance();
    wl_resource* res = wl_resource_create(wl_resource_get_client(pointer),
                                          &zwp_confined_pointer_v1_interface, 1, id);
    wl_resource_set_implementation(res, &kConfinedImpl, nullptr,
        [](wl_resource* r) { OnConstraintResourceDestroyed(r); });
    {
        std::lock_guard<std::mutex> lk(self->mutex_);
        auto& v = self->constraints_;
        v.erase(std::remove_if(v.begin(), v.end(),
                               [&](const Constraint& c) { return c.surface == surface; }),
                v.end());
        Constraint c;
        c.type = ConstraintType::Confine;
        c.surface = surface;
        c.res = res;
        v.push_back(c);
    }
    zwp_confined_pointer_v1_send_confined(res);
    OH_LOG_INFO(LOG_APP, "[PtrExt] CONFINE pointer on surf=%{public}p lifetime=%{public}u",
                static_cast<void*>(surface), lifetime);
}

void PointerExtras::locked_set_cursor_position_hint(wl_client*, wl_resource* r,
                                                    wl_fixed_t sx, wl_fixed_t sy) {
    auto* self = GetInstance();
    std::lock_guard<std::mutex> lk(self->mutex_);
    for (auto& c : self->constraints_) {
        if (c.res == r) {
            c.hasHint = true;
            c.hintX = wl_fixed_to_double(sx);
            c.hintY = wl_fixed_to_double(sy);
            return;
        }
    }
}

void PointerExtras::locked_destroy(wl_client*, wl_resource* r) { wl_resource_destroy(r); }
void PointerExtras::confined_destroy(wl_client*, wl_resource* r) { wl_resource_destroy(r); }

void PointerExtras::OnConstraintResourceDestroyed(wl_resource* r) {
    auto* self = GetInstance();
    Constraint gone;
    bool found = false;
    {
        std::lock_guard<std::mutex> lk(self->mutex_);
        auto& v = self->constraints_;
        auto it = std::find_if(v.begin(), v.end(),
                               [&](const Constraint& c) { return c.res == r; });
        if (it != v.end()) {
            gone = *it;
            found = true;
            v.erase(it);
        }
    }
    if (!found) return;
    // 解锁时若游戏给过 cursor position hint (它自己画的光标位置),
    // 把逻辑指针移到 hint, 避免解锁瞬间光标跳回锁定点
    // (协议: set_cursor_position_hint 的既定用途)
    if (gone.type == ConstraintType::Lock && gone.hasHint) {
        InputManager::GetInstance()->OnPointerWarp(gone.surface, gone.hintX, gone.hintY);
    }
    // Lock 约束销毁 (游戏退出相对模式 / wine 断连) → 还原 host 系统光标
    if (gone.type == ConstraintType::Lock) {
        self->ApplyHostCursorLock(false);
    }
    OH_LOG_INFO(LOG_APP, "[PtrExt] constraint destroyed type=%{public}d surf=%{public}p hint=%{public}d",
                static_cast<int>(gone.type), static_cast<void*>(gone.surface),
                gone.hasHint ? 1 : 0);
}

// ========================================================================
//  wp_pointer_warp_v1
// ========================================================================

static const struct wp_pointer_warp_v1_interface kWarpImpl = {
    .destroy = PointerExtras::warp_destroy,
    .warp_pointer = PointerExtras::warp_warp_pointer,
};

void PointerExtras::warp_bind(wl_client* client, void* data,
                              uint32_t version, uint32_t id) {
    wl_resource* res = wl_resource_create(client, &wp_pointer_warp_v1_interface,
                                          std::min(version, 1u), id);
    wl_resource_set_implementation(res, &kWarpImpl, data, nullptr);
}

void PointerExtras::warp_warp_pointer(wl_client*, wl_resource*, wl_resource* surface,
                                      wl_resource* pointer, wl_fixed_t x, wl_fixed_t y,
                                      uint32_t serial) {
    // 协议建议校验 enter serial; 宽容处理只记日志 — 拒绝会让游戏输入彻底卡死
    const double dx = wl_fixed_to_double(x);
    const double dy = wl_fixed_to_double(y);
    static uint32_t sWarpLogN = 0;
    if (++sWarpLogN % 120 == 1)  // warp 是游戏每帧高频路径, 抽样 120:1
        OH_LOG_INFO(LOG_APP, "[PtrExt] warp surf=%{public}p → (%{public}.1f,%{public}.1f) serial=%{public}u n=%{public}u",
                    static_cast<void*>(surface), dx, dy, serial, sWarpLogN);
    InputManager::GetInstance()->OnPointerWarp(surface, dx, dy);
}

// ========================================================================
//  zwp_relative_pointer_manager_v1
// ========================================================================

static const struct zwp_relative_pointer_v1_interface kRelativeImpl = {
    .destroy = PointerExtras::relmgr_destroy,  // 对象本身只有 destroy
};

void PointerExtras::relmgr_bind(wl_client* client, void* data,
                                uint32_t version, uint32_t id) {
    wl_resource* res = wl_resource_create(client, &zwp_relative_pointer_manager_v1_interface,
                                          std::min(version, 1u), id);
    static const struct zwp_relative_pointer_manager_v1_interface kRelMgrImpl = {
        .destroy = PointerExtras::relmgr_destroy,
        .get_relative_pointer = PointerExtras::relmgr_get_relative_pointer,
    };
    wl_resource_set_implementation(res, &kRelMgrImpl, data, nullptr);
}

void PointerExtras::relmgr_get_relative_pointer(wl_client* client, wl_resource*,
                                                uint32_t id, wl_resource* pointer) {
    auto* self = GetInstance();
    wl_resource* res = wl_resource_create(client, &zwp_relative_pointer_v1_interface, 1, id);
    wl_resource_set_implementation(res, &kRelativeImpl, nullptr,
        [](wl_resource* r) { OnRelativePointerDestroyed(r); });
    {
        std::lock_guard<std::mutex> lk(self->mutex_);
        self->relativePointers_.push_back(res);
    }
    OH_LOG_INFO(LOG_APP, "[PtrExt] relative_pointer created (bind ptr=%{public}p, total=%{public}zu)",
                static_cast<void*>(pointer), self->relativePointers_.size());
}

void PointerExtras::OnRelativePointerDestroyed(wl_resource* r) {
    auto* self = GetInstance();
    std::lock_guard<std::mutex> lk(self->mutex_);
    auto& v = self->relativePointers_;
    v.erase(std::remove(v.begin(), v.end(), r), v.end());
    OH_LOG_INFO(LOG_APP, "[PtrExt] relative_pointer destroyed (remaining=%{public}zu)", v.size());
}

bool PointerExtras::HasRelativePointer() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return !relativePointers_.empty();
}

void PointerExtras::SendRelativeMotion(double dx, double dy) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (relativePointers_.empty()) return;
    // 系统性链路日志 (断点 5): 相对增量广播 — 确认增量实际发到 wine (对象数
    // >0), 与 input_manager 差分 (断点 4) 配对; 高频抽样 120:1 (拖动只看趋势,
    // 防刷爆 hilog — 测试需全量时改这里)
    static uint32_t sRelMvLogN = 0;
    if (++sRelMvLogN % 120 == 0)
        OH_LOG_INFO(LOG_APP, "[PtrExt] rel_motion d=(%{public}.1f,%{public}.1f) objs=%{public}zu",
                    dx, dy, relativePointers_.size());
    // 无加速输入设备: unaccel = accel 同值; utime 用单调时钟微秒 (wine 侧
    // 只读增量, 不读时间戳, 发 0 亦可 — 保留时间供诊断)
    const uint64_t us = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    const wl_fixed_t fdx = wl_fixed_from_double(dx);
    const wl_fixed_t fdy = wl_fixed_from_double(dy);
    for (auto* res : relativePointers_) {
        zwp_relative_pointer_v1_send_relative_motion(res,
            static_cast<uint32_t>(us >> 32), static_cast<uint32_t>(us & 0xffffffffu),
            fdx, fdy, fdx, fdy);
    }
}

// ========================================================================
//  查询接口
// ========================================================================

PointerExtras::ConstraintType PointerExtras::ConstraintFor(wl_resource* surface) {
    if (!surface) return ConstraintType::None;
    std::lock_guard<std::mutex> lk(mutex_);
    for (const auto& c : constraints_) {
        if (c.surface != surface) continue;
        // 惰性失效: surface 已销毁的约束视为不存在
        if (!WaylandServer::GetInstance()->IsSurfaceAlive(surface))
            return ConstraintType::None;
        return c.type;
    }
    return ConstraintType::None;
}

// ========================================================================
//  Host 光标锁定 (OH_WindowManager_LockCursor + ets 隐藏光标)
// ========================================================================

void PointerExtras::RegisterHostWindow(int32_t windowId) {
    if (windowId <= 0) return;
    auto* self = GetInstance();
    std::lock_guard<std::mutex> lk(self->mutex_);
    auto& v = self->hostWindowIds_;
    if (std::find(v.begin(), v.end(), windowId) == v.end()) {
        v.push_back(windowId);
        OH_LOG_INFO(LOG_APP, "[PtrExt] host window registered: %{public}d (total=%{public}zu)",
                    windowId, v.size());
    }
}

void PointerExtras::SetPointerLockCallback(std::function<void(bool)> cb) {
    std::lock_guard<std::mutex> lk(mutex_);
    lockCallback_ = std::move(cb);
}

void PointerExtras::ApplyHostCursorLock(bool lock) {
    std::function<void(bool)> cb;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        if (lock) {
            // 重入守卫: 同 surface 重建约束会再次走 lock 路径, 已锁则跳过
            if (lockedWindowId_ != 0) return;
            // LockCursor 仅对获焦窗口生效 (失焦窗口返回 STATE_ABNORMAL),
            // 逐个尝试已注册窗口, 成功即停并记下窗口 id 供解锁用
            for (int32_t id : hostWindowIds_) {
                const int32_t ret = OH_WindowManager_LockCursor(id, false);  // false = 光标冻结不跟随
                if (ret == 0) {  // WM_ERROR_OK
                    lockedWindowId_ = id;
                    OH_LOG_INFO(LOG_APP, "[PtrExt] host cursor LOCKED win=%{public}d", id);
                    break;
                }
                OH_LOG_WARN(LOG_APP, "[PtrExt] LockCursor win=%{public}d failed ret=%{public}d", id, ret);
            }
            // 全部失败 (无获焦窗口/系统 <API22) 不阻断: rawDelta 相对位移
            // 通道 (InputManager) 不依赖冻结仍工作; 光标照常隐藏 (相对模式下
            // 游戏自绘光标, 可见的系统光标只剩干扰)
        } else {
            if (lockedWindowId_ == 0) return;
            const int32_t ret = OH_WindowManager_UnlockCursor(lockedWindowId_);
            OH_LOG_INFO(LOG_APP, "[PtrExt] host cursor UNLOCKED win=%{public}d ret=%{public}d",
                        lockedWindowId_, ret);
            lockedWindowId_ = 0;
        }
        cb = lockCallback_;
    }
    // tsfn 回调放锁外触发, 防 ets 侧重入
    if (cb) cb(lock);
}
