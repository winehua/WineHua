#include "pointer_extras.h"

#include "protocols/pointer-constraints-unstable-v1-server-protocol.h"
#include "protocols/pointer-warp-v1-server-protocol.h"
#include "protocols/relative-pointer-unstable-v1-server-protocol.h"
// 解环 (重构第 4C1 步): 本文件不再 include input_manager.h — warp 位置同步
// 经 SetPointerWarpSink 装配 (见头注释"warp 回调装配"), 装配点在
// wl_core.cpp RegisterWlCoreGlobals。6A: 也不再 include wayland_server.h —
// surface→toplevel 反查与 root 判定经 BindWaylandRefs 注入引用 (见头注释)。
#include "compositor/toplevel/toplevel_manager.h"
#include "compositor/input/input_resolver.h"

#include <algorithm>
#include <chrono>
#include <thread>

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

// warp 回调装配 (4C1 解环): 注入 InputManager::OnPointerWarp 的转发 lambda。
// 无锁 — 装配总在 Server Start 阶段 (wl 事件循环启动前) 做一次, 之后回调
// 只在 Wayland 线程读 warpSink_ (见 pointer_extras.h"warp 回调装配"注释)。
void PointerExtras::SetPointerWarpSink(PointerWarpSink sink) {
    warpSink_ = std::move(sink);
}

void PointerExtras::SetRelativeBaselineSink(RelativeBaselineSink sink) {
    relativeBaselineSink_ = std::move(sink);
}

// 6A 会话引用装配: 见头注释。装配于 Server Start 阶段 (wl 事件循环启动前)
// 一次性, 之后只在 Wayland 线程读 tmgr_/rootId 引用 — 无锁。
void PointerExtras::BindWaylandRefs(ToplevelManager* tmgr,
                                    const uint32_t* desktopRootToplevelId,
                                    InputResolver* resolver,
                                    const bool* desktopMode) {
    tmgr_ = tmgr;
    resolver_ = resolver;
    desktopRootToplevelId_ = desktopRootToplevelId;
    desktopMode_ = desktopMode;
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
    // surface → toplevelId (锁外算: FindToplevelBySurface 自持 toplevelSurfaceMutex_,
    // 与 self->mutex_ 是两把独立锁, 但不要嵌套持锁)
    // 6A: 直呼注入的 ToplevelManager (原 WaylandServer::FindToplevelIdBySurface 转发;
    // 本函数是 static, 经单例取装配引用 — 与 self->mutex_ 同访问模式)
    const uint32_t tl = self->tmgr_->FindToplevelBySurface(surface);
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
        c.toplevelId = tl;
        v.push_back(c);
    }
    // Wine 总在 surface 有焦点时请求 → 立即激活 (参照 weston: focus 满足即激活)
    zwp_locked_pointer_v1_send_locked(res);
    OH_LOG_INFO(LOG_APP, "[PtrExt] LOCK pointer on surf=%{public}p lifetime=%{public}u",
                static_cast<void*>(surface), lifetime);
    // Lock 约束 ≠ 相对模式 (红警2 主菜单光标可见+挂约束, 原在此冻结
    // 误冻 6.5 分钟实锤) — 冻结已迁移到 relmgr_get_relative_pointer
    // (relative_pointer 创建 = wine 真相对模式), 此处不冻结。
}

void PointerExtras::constr_confine_pointer(wl_client*, wl_resource*, uint32_t id,
                                           wl_resource* surface, wl_resource* pointer,
                                           wl_resource* region, uint32_t lifetime) {
    auto* self = GetInstance();
    // 6A: 直呼注入的 ToplevelManager (原 WaylandServer::FindToplevelIdBySurface 转发;
    // static 函数经单例取装配引用, 同 self->mutex_ 模式)
    const uint32_t tl = self->tmgr_->FindToplevelBySurface(surface);
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
        c.toplevelId = tl;
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
        // 4C1 解环: 经装配的回调同步 (原直接调 InputManager::OnPointerWarp)
        // static 成员函数经 GetInstance() 取 sink (warpSink_ 是实例成员)
        const auto sink = self->warpSink_;
        if (sink) sink(gone.surface, gone.hintX, gone.hintY);
    }
    // 注: Lock 约束销毁不再触发解锁 — 宿主冻结只挂在 relative_pointer
    // 创建上, 约束销毁 (wine 退出相对的同一批里先于 relative 销毁, 且
    // 红警2 主菜单"可见光标+挂 Lock"本就不冻结) 在此解锁只会有两种结果:
    // 未锁 (无操作) 或提前解 (relative 对象仍存活, 光标提前恢复)。
    // 解锁统一在 OnRelativePointerDestroyed (剩余 0 个时)。
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
    // 4C1 解环: 经装配的回调同步 (原直接调 InputManager::OnPointerWarp);
    // static 成员函数经 GetInstance() 取 sink (warpSink_ 是实例成员)
    const auto sink = GetInstance()->warpSink_;
    if (sink) sink(surface, dx, dy);
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
    uint32_t tl = 0;
    size_t total = 0;
    {
        std::lock_guard<std::mutex> lk(self->mutex_);
        self->relativePointers_.push_back({res, pointer, client});
        self->cursorLockClient_ = client;
        total = self->relativePointers_.size();
        for (const auto& c : self->constraints_)
            if (c.type == ConstraintType::Lock && wl_resource_get_client(c.surface) == client) {
                tl = c.toplevelId;
                break;
            }
    }
    OH_LOG_INFO(LOG_APP, "[PtrExt] relative_pointer created (bind ptr=%{public}p, total=%{public}zu) tl=%{public}u",
                static_cast<void*>(pointer), total, tl);
    if (self->relativeBaselineSink_) self->relativeBaselineSink_("relative-pointer-created");
    // 冻结/隐藏 host 光标改挂在这里 (原挂 constr_lock_pointer — Lock 约束
    // 存在 ≠ 相对模式: 红警2 主菜单光标可见也挂约束, 误冻结 6.5 分钟)。
    // relative_pointer 对象是 wine 侧 needs_relative 判定 (光标隐藏+约束+
    // 焦点一致) 的产物, 只有真相对模式 (war3/PAL2 全屏) 才会创建 → 冻结
    // 判据正确; 红警2 主菜单不建对象 → 不冻结。
    self->ApplyHostCursorLock(true, tl);
}

void PointerExtras::OnRelativePointerDestroyed(wl_resource* r) {
    auto* self = GetInstance();
    size_t remaining = 0;
    bool release = false;
    {
        std::lock_guard<std::mutex> lk(self->mutex_);
        auto& v = self->relativePointers_;
        v.erase(std::remove_if(v.begin(), v.end(),
                               [r](const RelativePointer& entry) {
                                   return entry.resource == r;
                               }), v.end());
        remaining = v.size();
        // A desktop/Steam relative object can outlive the active game's
        // object. Only the active client's remaining objects retain its lock.
        if (self->cursorLockClient_ && std::none_of(v.begin(), v.end(),
                [&](const RelativePointer& entry) { return entry.client == self->cursorLockClient_; })) {
            self->cursorLockClient_ = nullptr;
            release = true;
        }
    }
    OH_LOG_INFO(LOG_APP, "[PtrExt] relative_pointer destroyed (remaining=%{public}zu)", remaining);
    if (self->relativeBaselineSink_) self->relativeBaselineSink_("relative-pointer-destroyed");
    // 同一 client 的多个对象共享锁；其他客户端不会阻止该游戏恢复光标。
    if (release) {
        self->ApplyHostCursorLock(false, 0);
    }
}

bool PointerExtras::HasRelativePointerForSurface(wl_resource* surface) const {
    if (!surface || !resolver_ || !resolver_->IsSurfaceAlive(surface)) return false;
    wl_client* client = wl_resource_get_client(surface);
    std::lock_guard<std::mutex> lk(mutex_);
    return std::any_of(relativePointers_.begin(), relativePointers_.end(),
                       [client](const RelativePointer& entry) {
                           return entry.client == client;
                       });
}

bool PointerExtras::HasRelativePointer() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return !relativePointers_.empty();
}

void PointerExtras::SendRelativeMotion(wl_resource* surface, double dx, double dy) {
    if (!surface || !resolver_ || !resolver_->IsSurfaceAlive(surface)) return;
    wl_client* client = wl_resource_get_client(surface);
    std::lock_guard<std::mutex> lk(mutex_);
    // 无加速输入设备: unaccel = accel 同值; utime 用单调时钟微秒 (wine 侧
    // 只读增量, 不读时间戳, 发 0 亦可 — 保留时间供诊断)
    const uint64_t us = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    const wl_fixed_t fdx = wl_fixed_from_double(dx);
    const wl_fixed_t fdy = wl_fixed_from_double(dy);
    for (const auto& entry : relativePointers_) {
        if (entry.client != client) continue;
        zwp_relative_pointer_v1_send_relative_motion(entry.resource,
            static_cast<uint32_t>(us >> 32), static_cast<uint32_t>(us & 0xffffffffu),
            fdx, fdy, fdx, fdy);
    }
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

void PointerExtras::SetPointerLockCallback(std::function<void(bool, uint32_t)> cb) {
    std::lock_guard<std::mutex> lk(mutex_);
    lockCallback_ = std::move(cb);
}

void PointerExtras::ApplyHostCursorLock(bool lock, uint32_t toplevelId) {
    // isShell 在调用线程 (wl 事件循环) 算好再捕获进工作线程 — 避免工作线程
    // 与 wl 线程并发读这些共享引用 (数据竞争)。
    // 6A: 直读装配注入的 id 共享引用 (与 WaylandServer 的 session 字段同源)。
    //
    // 桌面模式下 root 尚未确立 = 桌面未就绪: 此刻能发起相对指针的只可能是
    // 桌面壳自身 (游戏要先有桌面), 不能按游戏冻结 — 否则宿主锁死系统光标,
    // 表现为"可见但动不了", 且 relative 对象不销毁则永不解锁。
    // toplevelId==0 (约束 surface 未映射成 toplevel) 与 root==0 同属身份未知,
    // 一并按不冻结处理。
    const bool desktopNotReady = desktopMode_ && desktopRootToplevelId_ &&
        *desktopMode_ && *desktopRootToplevelId_ == 0;
    const bool isShell = desktopNotReady || toplevelId == 0 ||
        (desktopRootToplevelId_ && toplevelId == *desktopRootToplevelId_);
    // IPC 挪入独立线程执行。只启动一个 worker，始终收敛到最新请求。
    // 旧方案为每个请求另开线程并在解锁排队时清 actual window；晚到的
    // LockCursor 会在解锁后重新冻结光标。actual window 现在只由 worker
    // 修改，过期的锁定成功会在下一轮立即解除，不回报旧的 locked=true。
    {
        std::lock_guard<std::mutex> lk(mutex_);
        etsLockNotified_ = lock;
        lockedToplevelId_ = lock ? toplevelId : 0;
        cursorLockIsShell_ = isShell;
        ++cursorLockGeneration_;
        if (cursorLockWorkerRunning_) return;
        cursorLockWorkerRunning_ = true;
    }
    std::thread([this]() {
        for (;;) {
            bool requested, shell;
            uint32_t tl;
            uint64_t generation;
            int32_t actual;
            std::vector<int32_t> ids;
            std::function<void(bool, uint32_t)> cb;
            {
                std::lock_guard<std::mutex> lk(mutex_);
                requested = etsLockNotified_;
                shell = cursorLockIsShell_;
                tl = lockedToplevelId_;
                generation = cursorLockGeneration_;
                actual = lockedWindowId_;
                ids = hostWindowIds_;
                cb = lockCallback_;
            }
            // Do not hold mutex_ over window-service IPC or the JS callback.
            if (actual && (!requested || shell)) {
                const int32_t ret = OH_WindowManager_UnlockCursor(actual);
                OH_LOG_INFO(LOG_APP, "[PtrExt] host cursor UNLOCKED win=%{public}d ret=%{public}d", actual, ret);
                std::lock_guard<std::mutex> lk(mutex_);
                lockedWindowId_ = 0;
                actual = 0;
            }
            if (requested && !shell && !actual) {
                for (int32_t id : ids) {
                    const int32_t ret = OH_WindowManager_LockCursor(id, false);
                    if (ret == 0) {
                        std::lock_guard<std::mutex> lk(mutex_);
                        lockedWindowId_ = id;
                        OH_LOG_INFO(LOG_APP, "[PtrExt] host cursor LOCKED win=%{public}d", id);
                        break;
                    }
                    OH_LOG_WARN(LOG_APP, "[PtrExt] LockCursor win=%{public}d failed ret=%{public}d", id, ret);
                }
            }
            {
                std::lock_guard<std::mutex> lk(mutex_);
                if (generation != cursorLockGeneration_) continue;
            }
            // Notify on owner changes even when the same host window stays
            // locked. Lock failure still needs a paired visibility reset.
            if (cb) cb(requested, tl);
            {
                std::lock_guard<std::mutex> lk(mutex_);
                if (generation != cursorLockGeneration_) continue;
                cursorLockWorkerRunning_ = false;
                return;
            }
        }
    }).detach();
}

void PointerExtras::ReleaseLockForToplevel(uint32_t toplevelId) {
    bool release = false;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        // 未通知过 ets / 锁定来源不是本 toplevel → 不是本次要清的锁定
        release = etsLockNotified_ && lockedToplevelId_ == toplevelId;
    }
    if (!release) return;
    OH_LOG_INFO(LOG_APP, "[PtrExt] release lock for destroyed toplevel %{public}u", toplevelId);
    // 锁外调用 (ApplyHostCursorLock 自持 mutex_): 走正常解锁路径 —
    // UnlockCursor(已销毁窗口 id 返回错误无害) + cb(false) 通知 ets 恢复光标
    ApplyHostCursorLock(false, 0);
}
