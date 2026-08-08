#include "wayland_server.h"
#include "seat.h"
#include "input_manager.h"
#include "xdg_shell.h"
#include "fps_counter.h"
#include "wine_process.h"
#include "compositor/debug_assert.h"
#include "include/xdg-shell-server-protocol.h"
#include <algorithm>
#include <cstring>
#include <ctime>
#include <cmath>
#include <cerrno>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>


extern "C" void RegisterXdgShell(wl_display* display);
extern "C" void RegisterWlCoreGlobals(wl_display* display);
#undef LOG_TAG
#undef LOG_DOMAIN
#define LOG_DOMAIN 0x0000
#define LOG_TAG "WL_Server"
#include <hilog/log.h>
#include "plugin_manager.h"

// 核心协议接口表与实现已剥离到 wl_core.cpp (Phase 3 纯搬移)

// -- 单例 --
WaylandServer* WaylandServer::GetInstance() {
    static WaylandServer s;
    return &s;
}

bool WaylandServer::Start(const std::string& socketPath) {
    if (running_) {
        OH_LOG_WARN(LOG_APP, "[WL] already running");
        return true;
    }

    OH_LOG_INFO(LOG_APP, "[WL] Starting compositor, socket=%{public}s", socketPath.c_str());

    // 清理残留 socket
    unlink(socketPath.c_str());

    // 确保 socket 目录存在
    auto pos = socketPath.find_last_of('/');
    std::string dir = socketPath.substr(0, pos);
    std::string name = socketPath.substr(pos + 1);
    int rc = mkdir(dir.c_str(), 0700);
    OH_LOG_INFO(LOG_APP, "[WL] mkdir(%{public}s) = %{public}d, errno=%{public}d",
                dir.c_str(), rc, errno);

    setenv("XDG_RUNTIME_DIR", dir.c_str(), 1);

    display_ = wl_display_create();
    if (!display_) {
        OH_LOG_ERROR(LOG_APP, "[WL] wl_display_create failed, errno=%{public}d", errno);
        return false;
    }
    OH_LOG_INFO(LOG_APP, "[WL] wl_display created");

    if (wl_display_add_socket(display_, name.c_str()) != 0) {
        OH_LOG_ERROR(LOG_APP, "[WL] wl_display_add_socket(%{public}s) failed, errno=%{public}d",
                     name.c_str(), errno);
        return false;
    }
    OH_LOG_INFO(LOG_APP, "[WL] socket added: %{public}s", name.c_str());

    setenv("WAYLAND_DISPLAY", name.c_str(), 1);

    // 注册 global 对象 (核心协议实现已剥离到 wl_core.cpp)
    RegisterWlCoreGlobals(display_);
    wl_display_init_shm(display_);
    RegisterXdgShell(display_);
    Seat::GetInstance()->Register(display_);
    InputManager::GetInstance()->Initialize(display_);
    OH_LOG_INFO(LOG_APP, "[WL] globals registered (compositor+shm+xdg+subcompositor+viewporter+output+seat+input)");

    running_ = true;
    firstFrame_ = false;
    thread_ = std::thread(&WaylandServer::EventLoop, this);
    OH_LOG_INFO(LOG_APP, "[WL] compositor started OK");
    return true;
}

void WaylandServer::Stop() {
    if (!running_) return;
    running_ = false;
    InputManager::GetInstance()->Shutdown();
    Seat::GetInstance()->Unregister();
    if (display_) wl_display_terminate(display_);
    if (thread_.joinable()) thread_.join();
    if (display_) {
        wl_display_destroy(display_);
        display_ = nullptr;
    }
    firstFrame_ = false;
}

void WaylandServer::EventLoop() {
    int tick = 0;
    while (running_) {
        wl_event_loop* loop = wl_display_get_event_loop(display_);
        int ret = wl_event_loop_dispatch(loop, 50); // 50ms timeout
        if (ret < 0) {
            OH_LOG_ERROR(LOG_APP, "[WL-ERR] event loop error: %{public}s (errno=%{public}d)",
                         strerror(errno), errno);
        }
        wl_display_flush_clients(display_);  // dispatch 可能写数据, 之后 flush

        // 每 30 秒输出一次资源快照 (50ms * 600 = 30s)
        if (++tick % 600 == 0) {
            size_t renderers = PluginManager::GetInstance()->GetRendererCount();
            OH_LOG_INFO(LOG_APP, "[WL-STAT] toplevels=%{public}zu surfaces=%{public}zu renderers=%{public}zu",
                        toplevelMgr_.ToplevelResourceCount(), toplevelMgr_.ToplevelSurfaceCount(), renderers);
        }
    }
}



// -- 帧数据接口 --
bool WaylandServer::TakeFrame(std::vector<uint8_t>& out, int& w, int& h) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (!dirty_) return false;
    out = pixels_;
    w = width_;
    h = height_;
    dirty_ = false;
    OH_LOG_INFO(LOG_APP, "[MW-TAKE] global frame %{public}dx%{public}d px=%{public}zu", w, h, out.size());
    return true;
}

void WaylandServer::RaiseToplevel(uint32_t id, bool userInitiated) {
    auto lk = toplevelMgr_.Lock();
    toplevelMgr_.RemoveFromZOrder(id);
    toplevelMgr_.AddToZOrder(id);
    // 全屏优先级: 仅"用户显式 raise (任务栏/窗口点击经 ArkTS 发起) 且目标
    // 当前已 fullscreen"时重新取号 — 两个全屏窗口互相切换靠它;
    // tl_set_fullscreen 批处理里的 raise 不重新取号 (显示模式切换会批量连带
    // 标记旧窗口, 重新取号即退回到达顺序决定论); 窗口化窗口不重新取号
    // (点过 notepad 不该让它日后被连带标全屏时盖过游戏)。
    // 注意: AddToZOrder 对首次入列的窗口会取初始号 (红警2 set_fullscreen
    // 先于首帧 commit 时经此路径取号), 与"不重新取号"不冲突。
    // 见 ToplevelState::fsPriority
    if (userInitiated) {
        if (const auto* rst = toplevelMgr_.FindToplevelLocked(id); rst && rst->IsFullscreen())
            toplevelMgr_.BumpFsPriorityLocked(id);
    }
    // 任务栏始终在顶层 (app_id == "explorer.exe.taskbar");
    // 全屏窗口例外 — 游戏全屏必须压过任务栏
    bool raisedFullscreen = false;
    if (const auto* rst = toplevelMgr_.FindToplevelLocked(id)) raisedFullscreen = rst->IsFullscreen();
    if (taskbarId_ > 0 && taskbarId_ != id && !raisedFullscreen) {
        toplevelMgr_.RemoveFromZOrder(taskbarId_);
        toplevelMgr_.AddToZOrder(taskbarId_);
    }
    MarkDesktopRootDirtyLocked();
}

// -- 交互式窗口移动 (xdg_toplevel.move) --
void WaylandServer::StartMoveGrab(uint32_t toplevelId, uint32_t serial) {
    // 用最近一次注入的全局指针位置立即算固定 grab 偏移:
    // 绝对定位后窗口每帧由 全局坐标−偏移 决定, 不依赖消费时刻的 st->x
    moveGrab_.StartMoveGrab(toplevelMgr_, toplevelId, serial,
                            wl_fixed_to_int(InputManager::GetInstance()->GetLastGlobalPointerX()),
                            wl_fixed_to_int(InputManager::GetInstance()->GetLastGlobalPointerY()));
    if (Policy().OhosWindowPerToplevel()) {
        FireToplevelEvent(toplevelId, "move_start");
    }
}

void WaylandServer::EndMoveGrab() {
    uint32_t tl = moveGrab_.GetToplevelId();
    moveGrab_.EndMoveGrab(toplevelMgr_);
    if (Policy().OhosWindowPerToplevel() && tl != 0) {
        FireToplevelEvent(tl, "move_end");
    }
}

bool WaylandServer::ProcessMoveGrabMotion(wl_fixed_t wx, wl_fixed_t wy) {
    // 注意: InputManager 在 grab 激活时注入的是桌面全局坐标 (wl_fixed_t),
    // 这里截断为整数全局坐标交 MoveGrabHandler 绝对定位
    if (!moveGrab_.ProcessMoveGrabMotion(toplevelMgr_, wl_fixed_to_int(wx),
                                         wl_fixed_to_int(wy))) return false;
    MarkDesktopRootDirtyLocked();
    return true;
}

void WaylandServer::FireToplevelEvent(uint32_t id, const char* event, const char* jsonData) {
    OH_LOG_INFO(LOG_APP, "[MW] FireToplevel id=%{public}u event=%{public}s data=%{public}s", id, event, jsonData);
    if (toplevelCb_) toplevelCb_(id, event, jsonData);
    /* 桌面根出现 = 引擎消息通道的 evt:desktop-ready: LaunchPadMode 的 15s
     * root 等待超时后状态机停在 ready-degraded, 靠这个补票升级为正式 ready。
     * 挂在统一的 FireToplevelEvent 而非 LaunchPadMode 等待循环 — root 在任意
     * 时刻出现 (含慢设备超时后姗姗来迟) 都能补发; ArkTS 只在 degraded 态消费,
     * 其余情况 (正常启动 root 先到 / root 重建) 为无害空转。 */
    if (!strcmp(event, "desktop_root") && gStateTsfn) {
        napi_call_threadsafe_function(gStateTsfn, strdup("evt:desktop-ready"),
                                      napi_tsfn_blocking);
    }
}

void WaylandServer::RegisterToplevelResource(uint32_t toplevelId, wl_resource* tl) {
    toplevelMgr_.RegisterToplevelResource(toplevelId, tl);
    OH_LOG_INFO(LOG_APP, "[MW] RegisterToplevelResource id=%{public}u tl=%{public}p", toplevelId, tl);
}

void WaylandServer::UnregisterToplevelResource(uint32_t toplevelId) {
    auto* tl = toplevelMgr_.FindToplevelResource(toplevelId);
    if (tl) {
        OH_LOG_INFO(LOG_APP, "[MW] UnregisterToplevelResource id=%{public}u tl=%{public}p (Wine destroyed toplevel)",
                    toplevelId, tl);
    }
    toplevelMgr_.UnregisterToplevelResource(toplevelId);
}

void WaylandServer::OnToplevelDestroyed(uint32_t toplevelId) {
    std::vector<uint32_t> cascadePopups;
    bool wasDesktopRoot = false;
    {
        auto lk = toplevelMgr_.Lock();
        toplevelMgr_.EraseToplevelLocked(toplevelId);
        if (pendingDesktopRootToplevelId_ == toplevelId)
            pendingDesktopRootToplevelId_ = 0;
        if (taskbarId_ == toplevelId) {
            OH_LOG_INFO(LOG_APP, "[MW] taskbar toplevel #%{public}u destroyed, clearing cached id",
                        toplevelId);
            taskbarId_ = 0;
        }
        // root 本体被销毁 (xs_destroy / 客户端断连路径同样走到这里): 复位, 等待下一个 explorer
        if (desktopRootToplevelId_ == toplevelId) {
            OH_LOG_INFO(LOG_APP, "[MW] desktop root toplevel #%{public}u destroyed, clearing root",
                        toplevelId);
            desktopRootToplevelId_ = 0;
            wasDesktopRoot = true;
            // 桌面会话由 explorer 主动结束: 随后 wineserver 跟随退出属正常终结,
            // ProcMon 据此按 state:stopped 收口而非误报 failed (仅 desktop 模式
            // 有 root, PC 窗口模式不会走到这)
            MarkDesktopSessionEnded();
        }
        // 被抓取窗口销毁 → 复位 move grab, 防止悬空 grab 吞掉后续 motion
        if (moveGrab_.GetToplevelId() == toplevelId) {
            OH_LOG_INFO(LOG_APP, "[MW-MOVE] grabbed toplevel #%{public}u destroyed, reset grab",
                        toplevelId);
            moveGrab_.EndMoveGrab(toplevelMgr_);
        }
        toplevelMgr_.RemoveFromZOrder(toplevelId);
        // 级联清理该 toplevel 的全部 PC popup (帧数据 + 映射)
        for (const auto& [pid, rec] : toplevelMgr_.popups()) {
            if (rec.parentToplevel == toplevelId) cascadePopups.push_back(pid);
        }
        for (uint32_t pid : cascadePopups) toplevelMgr_.RemovePopupDataLocked(pid);
        MarkDesktopRootDirtyLocked();  // 非 desktop / root 已复位时 root=0, 自然 no-op
        // 对称清理 surface 映射 (popup 路径在 RemovePopupDataLocked 已清, toplevel 路径此前缺失):
        // xs_destroy 时 wl_surface 可能仍存活, 不清会让 GetSurfaceForToplevel(死 id) 命中
        // 已无 toplevel 身份的 surface。嵌套锁序同 RemovePopupDataLocked。
        toplevelMgr_.UnmapToplevelSurface(toplevelId);
    }
    // 桌面会话终结统一收口 (锁外): 重置进程级一次性状态, 使下次引擎启动
    // (热重启连旧 wineserver) 与冷启动同基线 — 状态生命周期按「Wine 会话」
    // 而非「进程」建模。stopClient 路径由 napi_init 显式调用同函数。
    // 注意: 不在 Wayland 线程销毁 renderer (DestroyToplevel → Shutdown →
    // join 渲染线程 — 渲染线程可能卡在 eglSwapBuffers 等不可中断点,
    // 同步 join 会永久阻塞 Wayland 事件循环, 热重启 explorer 连接无人
    // 处理, 桌面永远起不来)。root renderer 的销毁由 ArkTS DesktopLayer
    // onSurfaceDestroyed 负责 (缓存 rootId 配对销毁, 与创建对称)
    if (wasDesktopRoot) ResetSessionState();
    // 通知 ArkTS 销毁 popup 子窗口 (锁外触发)
    for (uint32_t pid : cascadePopups) {
        char json[64];
        snprintf(json, sizeof(json), "{\"popupId\":%u}", pid);
        FireToplevelEvent(toplevelId, "popup_hide", json);
    }
}

void WaylandServer::ResetSessionState() {
    // Wine 会话终结统一收口。只重置「进程级一次性/漂移状态」— 随 toplevel
    // 销毁自愈的字段 (root/pending/taskbar, OnToplevelDestroyed 锁内清理)
    // 不在这里重复, 避免锁外写非 atomic 字段与锁内读的竞态。
    firstFrame_ = false;   // 热重启不重走 Start, 不重置则新会话首帧不注入 focus
    moveGrab_.EndMoveGrab(toplevelMgr_);  // 幂等兜底 (grab 窗口非 root 时已随销毁复位)
    InputManager::GetInstance()->ResetSessionState();
    OH_LOG_INFO(LOG_APP, "[MW] session state reset (firstFrame/grab/input focus+keys)");
}

// RemovePopupDataLocked 已移至 ToplevelManager (compositor/toplevel_manager.cpp)

// RemovePopupBySurfaceKeyLocked 已移至 ToplevelManager (compositor/toplevel_manager.cpp)

bool WaylandServer::TakeWindowMask(uint32_t id, int& w, int& h, std::vector<uint8_t>& out) {
    // 收敛: 掩码消费唯一入口在 ToplevelManager::TakeWindowMask
    // (ToplevelState::TakeMask), 此处转发 (napi_init 唯一调用方)
    return toplevelMgr_.TakeWindowMask(id, w, h, out);
}

void WaylandServer::SendToplevelClose(uint32_t toplevelId) {
    wl_resource* tl = toplevelMgr_.FindToplevelResource(toplevelId);
    if (tl) {
        toplevelMgr_.UnregisterToplevelResource(toplevelId);
        OH_LOG_INFO(LOG_APP, "[MW] SendToplevelClose id=%{public}u -> xdg_toplevel_send_close", toplevelId);
        xdg_toplevel_send_close(tl);
    } else {
        OH_LOG_WARN(LOG_APP, "[MW] SendToplevelClose id=%{public}u NOT found", toplevelId);
    }
}

// IsToplevelVisibleLocked 已移至 ToplevelManager (compositor/toplevel_manager.cpp)

int32_t WaylandServer::GetWorkAreaHeight() {
    auto lk = toplevelMgr_.Lock();
    if (taskbarId_ == 0) return outputH_;
    const auto* st = toplevelMgr_.FindToplevelLocked(taskbarId_);
    if (!st) return outputH_;
    return st->Y();  // 工作区 = 任务栏上方空间
}

void WaylandServer::SetToplevelMinimized(uint32_t id) {
    auto lk = toplevelMgr_.Lock();
    // 保留 operator[] 建档语义: pre-commit 最小化同样记录状态
    toplevelMgr_.EnsureToplevelLocked(id).SetMinimized(true);
    MarkDesktopRootDirtyLocked();
}

void WaylandServer::SetToplevelRestored(uint32_t id) {
    // 清除 minimized 状态
    {
        auto lk = toplevelMgr_.Lock();
        if (auto* st = toplevelMgr_.FindToplevelLocked(id)) st->SetMinimized(false);
        MarkDesktopRootDirtyLocked();
    }
    // 发 configure 通知 Wine (如果 toplevel resource 存在)
    wl_resource* tl = toplevelMgr_.FindToplevelResource(id);
    if (!tl) return;
    auto* td = static_cast<ToplevelData*>(wl_resource_get_user_data(tl));
    if (!td || !td->xdgSurface) return;
    auto* xdg = static_cast<XdgSurface*>(wl_resource_get_user_data(td->xdgSurface));
    if (!xdg || !xdg->wlSurface) return;
    wl_array states;
    wl_array_init(&states);
    uint32_t* st = static_cast<uint32_t*>(wl_array_add(&states, sizeof(uint32_t)));
    *st = XDG_TOPLEVEL_STATE_ACTIVATED;
    // 全屏窗口从最小化还原: 维持 FULLSCREEN 状态 (尺寸 0,0 = Wine 保持当前尺寸)
    if (IsToplevelFullscreen(id)) {
        st = static_cast<uint32_t*>(wl_array_add(&states, sizeof(uint32_t)));
        *st = XDG_TOPLEVEL_STATE_FULLSCREEN;
    }
    xdg_toplevel_send_configure(tl, 0, 0, &states);
    wl_array_release(&states);
    wl_client* client = wl_resource_get_client(tl);
    wl_display* dpy = wl_client_get_display(client);
    xdg_surface_send_configure(xdg->xdgSurface, wl_display_next_serial(dpy));
}

void WaylandServer::SetToplevelMaximized(uint32_t id) {
    OH_LOG_INFO(LOG_APP, "[MW] SetToplevelMaximized id=%{public}u desktop=%{public}s",
                id, IsDesktopMode() ? "yes" : "no");
    auto lk = toplevelMgr_.Lock();
    if (auto* st = toplevelMgr_.FindToplevelLocked(id); st && st->HasPosition()) {
        st->AnchorToOrigin();
    }
    MarkDesktopRootDirtyLocked();
}

void WaylandServer::SetToplevelFullscreen(uint32_t id, bool on) {
    OH_LOG_INFO(LOG_APP, "[MW] SetToplevelFullscreen id=%{public}u on=%{public}s",
                id, on ? "yes" : "no");
    auto lk = toplevelMgr_.Lock();
    // Ensure 建档语义同 SetToplevelMinimized: pre-commit 全屏同样记录状态。
    // 状态转换 (置位 + 锚定 + preFs 快照 + 不变式断言) 收在
    // ToplevelState::ApplyFullscreen
    auto& st = toplevelMgr_.EnsureToplevelLocked(id);
    st.ApplyFullscreen(on);
    MarkDesktopRootDirtyLocked();
}

void WaylandServer::ForceToplevelRedraw(uint32_t id) {
    auto lk = toplevelMgr_.Lock();
    if (auto* st = toplevelMgr_.FindToplevelLocked(id)) st->MarkDirty();
}

void WaylandServer::NotifyToplevelResize(uint32_t toplevelId, int32_t w, int32_t h) {
    wl_resource* tl = toplevelMgr_.FindToplevelResource(toplevelId);
    if (!tl) return;

    auto* td = static_cast<ToplevelData*>(wl_resource_get_user_data(tl));
    if (!td || !td->xdgSurface) return;
    auto* xdg = static_cast<XdgSurface*>(wl_resource_get_user_data(td->xdgSurface));
    if (!xdg) return;

    auto* sd = static_cast<SurfaceData*>(wl_resource_get_user_data(xdg->wlSurface));

    OH_LOG_INFO(LOG_APP, "[MW] NotifyToplevelResize IN id=%{public}u %{public}dx%{public}d pc=%{public}s max=%{public}s",
                toplevelId, w, h,
                IsDesktopMode() ? "no" : "yes",
                (sd && sd->maximized) ? "yes" : "no");

    wl_array states;
    wl_array_init(&states);
    uint32_t* st = static_cast<uint32_t*>(wl_array_add(&states, sizeof(uint32_t)));
    *st = XDG_TOPLEVEL_STATE_ACTIVATED;
    if (sd && sd->maximized) {
        st = static_cast<uint32_t*>(wl_array_add(&states, sizeof(uint32_t)));
        *st = XDG_TOPLEVEL_STATE_MAXIMIZED;
    }
    // 全屏窗口在 OHOS 侧尺寸变化时保持 FULLSCREEN 状态, 否则 Wine 会退出全屏
    if (IsToplevelFullscreen(toplevelId)) {
        st = static_cast<uint32_t*>(wl_array_add(&states, sizeof(uint32_t)));
        *st = XDG_TOPLEVEL_STATE_FULLSCREEN;
    }
    xdg_toplevel_send_configure(tl, w, h, &states);
    wl_array_release(&states);

    wl_client* client = wl_resource_get_client(tl);
    wl_display* dpy = wl_client_get_display(client);
    xdg_surface_send_configure(xdg->xdgSurface, wl_display_next_serial(dpy));

    // 桌面 root 尺寸变化 → 同步更新 output 尺寸, 影响:
    //   - wl_output 上报的物理尺寸
    //   - xdg_toplevel_set_maximized / set_max_size 的基准值
    //   - FindToplevelAt / RaiseToplevel 的边界判断
    if (Policy().RootCompositing() && toplevelId == desktopRootToplevelId_) {
        SetOutputSize(w, h);
        OH_LOG_INFO(LOG_APP, "[MW] NotifyToplevelResize root=%{public}u → output %{public}dx%{public}d",
                    toplevelId, w, h);
    } else {
        OH_LOG_INFO(LOG_APP, "[MW] NotifyToplevelResize id=%{public}u → %{public}dx%{public}d maximized=%{public}s",
                    toplevelId, w, h, (sd && sd->maximized) ? "yes" : "no");
    }
}

// -- toplevelId -> wl_surface 映射 (供 Seat::InjectPointerEnter 查找) --
wl_resource* WaylandServer::GetSurfaceForToplevel(uint32_t toplevelId) {
    return toplevelMgr_.GetSurfaceForToplevel(toplevelId);
}
