#ifndef X11_TOPLEVEL_H
#define X11_TOPLEVEL_H
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct wlr_xwayland_surface;
struct wlr_buffer;
struct NativeWindow; /* 真身 SDK 侧 typedef OHNativeWindow; 文件作用域前置
                      * 声明 —— 原型作用域声明会在 .c 侧报 conflicting
                      * types (build 实测) */

/* M4a: xs 生命周期 → toplevel_event_bus 映射 (spec §3.1)。
 * 只做映射不做策略 —— 策略在 ArkTS 承载层 (与 wayland 路线同构)。id 复用
 * ToplevelManager::AllocateToplevelId; 事件名字符串逐字走
 * ToplevelEventName 红线 (toplevel_event_bus.h)。仅多窗模式激活
 * (x11_toplevel_set_active, chain_start multiwindow 位驱动)。 */
void x11_toplevel_set_active(bool on);
bool x11_toplevel_active(void);

/* 模态阻断判定 (任务 #167): target 是活 modal 对话框的 owner/祖先 → true。
 * 判据单点在 x11_toplevel.c owned_dialog_is_modal (对话框声明 ∧ owner
 * 禁用; owner 禁用态 = wine EnableWindow 写进 WM_HINTS.input=0, X 侧可达
 * 的 win32 语义镜像)。WM 职责 (EWMH §5.11): 模态由客户端声明, WM 在
 * 授焦/改变堆叠时执行。查询时无状态扫描; 事件面由 set_parent/
 * set_window_type/set_hints 三信号驱动 modal 事件上报 (ArkTS 承载)。
 * 重入安全 (trylock fail-open)。消费方: display_input FocusClient (授焦
 * 门) + 注入 dispatch (restack 门)。 */
bool x11_toplevel_modal_blocks(const struct wlr_xwayland_surface *target);

/* ohos_output HandleNewSurface 的 multiwindow 分支调用: 登记并挂 per-xs
 * 监听 (destroy/associate/set_title/request_configure)。 */
void x11_toplevel_notify_new_surface(struct wlr_xwayland_surface *xs);

/* wine 停止链调用 (display_compositor fail 收尾): 清 id 表、摘全部监听、
 * 对仍登记的 toplevel 补发 destroyed (ArkTS 侧窗口清理依赖它)。 */
void x11_toplevel_session_reset(void);

/* 输入路由查表 (Task 5); id_of_xs 返回 0 = 未登记。 */
struct wlr_xwayland_surface *x11_toplevel_xs_of(uint32_t toplevelId);
uint32_t x11_toplevel_id_of_xs(struct wlr_xwayland_surface *xs);

/* ── 任务 #135: host→wine 几何回写 (M4a 模型缺失半边, §2.14) ──
 * x11 fusion 下承载窗几何权威在 ArkTS (系统拉伸/系统拖拽直接改承载窗),
 * guest 几何权威在 wine 的 X 窗 —— 反向通道原先结构性缺失 (尺寸半边被
 * NotifyToplevelResize 的 xdg resource 查询挡死, 位置半边无任何通道)。
 * 回写 = X 协议原语 wlr_xwayland_surface_configure (与 wine→host 应答同
 * 函数, WM 主导 configure 语义), 两半各自只改自己那半 (尺寸半边保留
 * xs 当前位置 / 位置半边保留 xs 当前尺寸), 由调用方分拍上报。
 * 纪律 (与 pointer/key/close 同款): napi 线程只入队, loop 线程 dispatch
 * 执行 (xcb 连接专用); 与 xs 现值全等 → 跳过 (防回声第一道门); 最小化
 * 期间跳过 (镜像 xdg 侧门禁); 写入值记 entry (handle_request_configure
 * 的 echo 抑制基准)。 */
/* 目标 id 是否本映射层登记 (wayland_server.cpp 的 x11 分支门: 防路线旗标
 * 与 id 空间错配时误劫 wayland toplevel —— 双隔离红线)。 */
bool x11_toplevel_owns(uint32_t toplevelId);
/* w/h = guest 逻辑尺寸 (X 窗坐标, = 承载窗 surface 物理尺寸 / scale);
 * w/h <= 0 = 仅复位语义 (notifyToplevelResizeEnd 的 0 尺寸), x11 侧无
 * size-move 态 → 视为无操作。任意线程可调 (内部入队)。 */
void x11_toplevel_geometry_size(uint32_t toplevelId, int w, int h);
/* x/y = guest 逻辑坐标 (X 窗坐标 = 承载窗全局原点 / scale; 创建期摆放
 * FWM.ensure `pos = gx×scale` 的逆式, strip 在两侧抵消不进换算)。任意
 * 线程可调 (内部入队)。 */
void x11_toplevel_geometry_move(uint32_t toplevelId, int x, int y);
/* loop 线程执行端 (InjectQueueDrain 分派, display_input.c 调)。 */
void x11_toplevel_geometry_size_dispatch(uint32_t toplevelId, int w, int h);
void x11_toplevel_geometry_move_dispatch(uint32_t toplevelId, int x, int y);

/* ── Task 5: 按窗输入路由 ── */
/* napi 线程入口 (C1): 只入队, 实际执行在 loop 线程的 *_dispatch。
 * px/py = OHOS 承载窗局部物理像素 (SendPointerEvent 契约); action:
 * ArkTS MouseAction Press=1 Release=2 Move=3; button: evdev 码。 */
void x11_toplevel_input_pointer(uint32_t toplevelId, int px, int py,
                                int action, uint32_t button);
void x11_toplevel_input_key(uint32_t toplevelId, uint32_t keycode,
                            bool press);
/* M4d-review I1: 按窗轴注入 (滚轮)。px/py = OHOS 承载窗局部物理像素,
 * which: 0=纵向 1=横向, steps = ±N 格。映射层在 axis 前补 motion (axis
 * 不携带位置, 8a1e975 同类处理)。 */
void x11_toplevel_input_axis(uint32_t toplevelId, int px, int py,
                             int which, int steps);
/* 任务 #129 RF#3: 按窗关闭请求 (sendToplevelClose 的 x11 落地)。napi 线程
 * 入队, loop 线程 close_dispatch 执行 wlr_xwayland_surface_close —— 即
 * ICCCM WM_DELETE_WINDOW client message (wine 收到 → WM_CLOSE → 应用自行
 * 退出, win32 关窗语义; 协议不支持时 wlroots 回退 xcb_kill_client)。
 * 与 wayland 路线的 xdg_toplevel_send_close 同语义同层。 */
void x11_toplevel_close(uint32_t toplevelId);
void x11_toplevel_close_dispatch(uint32_t toplevelId);
/* loop 线程执行端 (InjectQueueDrain 分派, display_input.c 调): 查表 +
 * 物理像素→X 逻辑坐标换算 + 缝隙丢弃 + 纪律投递。勿在其他线程调。 */
void x11_toplevel_input_pointer_dispatch(uint32_t toplevelId, int px, int py,
                                         int action, uint32_t button);
void x11_toplevel_input_key_dispatch(uint32_t toplevelId, uint32_t keycode,
                                     bool press);
void x11_toplevel_input_axis_dispatch(uint32_t toplevelId, int px, int py,
                                      int which, int steps);

/* ── Task 3: per-xs 呈现 ── */
struct wlr_allocator;
struct wlr_renderer;
/* chain_start 后注入 (render_tick 画 buffer 用); 均为合成器线程对象。 */
void x11_toplevel_set_render_ctx(struct wlr_allocator *alloc,
                                 struct wlr_renderer *renderer);
/* ArkTS createRenderer 回绑 (plugin_manager x11 分支): 窗口 ←→ toplevel。
 * 返回 false = toplevel 已死 (竞态), 调用方须销毁 win (I5)。 */
bool x11_toplevel_attach_window(uint32_t toplevelId, struct NativeWindow *win,
                                int w, int h);
void x11_toplevel_detach_window(uint32_t toplevelId);
void x11_toplevel_resize_window(uint32_t toplevelId, int w, int h);
/* 帧时钟驱动 (ohos_output FrameTick 的 multiwindow 分支): 遍历已 attach 的
 * entry, 有新 commit 则画进该窗队列 buffer 并呈现; 未 attach 跳帧 (skip
 * 计数采样日志); xs 首帧到达时补发 created 事件。合成器线程调用。 */
void x11_toplevel_render_tick(void);

/* MW-GPU: fusion 承载窗挂 guest 零拷贝帧 (display_guest_frames PullFrame →
 * ohos_output_client_frame_set 的 multiwindow 分支路由入口)。返回 1 = 已
 * 收下 (本 entry 持一份引用, 替换/销毁时释放); 0 = 无该在册窗, 调用方自
 * 行释放。合成器 loop 线程调用。 */
int x11_toplevel_set_guest_frame(uint32_t xwindow, struct wlr_buffer *buffer,
                                 int flip);

/* MW-GPU: guest 帧路由 (display_guest_frames) 的窗口查询面 —— fusion 无
 * g_clients, ohos_output 的 client_frame_anchor / client_xwindow_generation
 * 在 multiwindow 下转接至此。几何 = X 全局坐标; 身份 = 记录序号。返回 0 =
 * 该 X 窗不在册。合成器 loop 线程调用 (与帧路由同拍)。 */
int x11_toplevel_guest_anchor(uint32_t xwindow, int *x, int *y, int *w, int *h);
int x11_toplevel_guest_generation(uint32_t xwindow, uint64_t *generation);

#ifdef __cplusplus
}
#endif

#endif /* X11_TOPLEVEL_H */
