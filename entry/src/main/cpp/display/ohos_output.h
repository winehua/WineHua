/*
 * ohos_output.h — T8 出图链的 C 接口
 *
 * wlr_output.h 在 C++ 下不安全 (color.h 的 C99 [static n] 语法, 实测编译
 * 报错), output/commit/present 逻辑整体放在 ohos_output.c (C 编译)。
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

struct wlr_backend;
struct wlr_renderer;
struct wl_display;
struct wl_event_loop;
struct wlr_xwayland;
struct wlr_xwayland_surface;
#include <native_window/external_window.h> /* OHNativeWindow (tag=NativeWindow) */

#ifdef __cplusplus
extern "C" {
#endif

// 当前跟踪的 client xwayland surface 取数口 (display_input 注入用)。
// client_xs = 最上层已映射窗口 (T1 自动注入目标); client_topmost_at =
// 帧坐标命中测试 (T2 多窗口); frame_size = 归一化注入坐标的基准。
// activate 需 xs 本体: wlr_xwayland_surface_activate 是合成器侧 API,
// 不调则 X server 焦点永不设置, 键事件无投递目标 (2026-09-29 gate5 实测)。
struct wlr_xwayland_surface *wl_ohos_output_client_xs(void);
struct wlr_xwayland_surface *wl_ohos_output_client_topmost_at(int fx, int fy);
void wl_ohos_output_frame_size(int *w, int *h);

// 窗口置前 (D36): g_clients 链序移尾 + scene 两节点 (X 面/帧) 提顶。
// 触发点 = request_activate 监听 (wine 激活请求) 与 button press (点击
// 置前)。xs 不在 g_clients (子窗/已销毁) 时为无操作。
void wl_ohos_output_client_raise(struct wlr_xwayland_surface *xs);

// x11 桌面 shell 是否已映射 (M3a): 首个 client XMapWindow 置位, 每轮
// chain_start 复位。LaunchPadMode 的桌面根等待谓词在 x11 路线用它同源判定
// (桌面根 toplevel 是 wayland 私有概念, X 路线没有, 不接这根线状态机永远
// 停在 ready-degraded —— 实测 2026-10-04, smoke runner 永不启动)。
int WineHua_DisplayRoute_DesktopShellMapped(void);

// X 窗口可见性谓词: surface->buffer (最后有效像素) 存在即视为有内容。
// 不得用 surface->mapped —— Xwayland 例行空 commit 会翻转它 (0.20
// surface_commit_state: NULL buffer commit → unmap), T2 实测闪断。
struct wlr_surface;
int wl_ohos_surface_has_content(struct wlr_surface *surf);

/* ── M2-T5: guest Vulkan 帧 (Venus 私有 present) 的落点 ─────────────────────
 * 私有 present 的 SURFACE_ID 是 X 顶层窗 id, 帧按 clientPid<<32|id 路由进来,
 * 落点就是这个 id 对应的 X 窗。scene 侧由本文件持有帧节点 (wlr_scene_buffer):
 * 挂在 X 面节点同一父树 (scene 根) 并 place_above 到该面之上。**所有权在本
 * 文件**: wlroots 只回收它自己那个 X 面节点, 帧节点随窗记录 (dissociate /
 * destroy) 显式销毁。 */
struct wlr_buffer; /* 前置声明必须在文件作用域: 只出现在原型里的 tag 会被
                    * 当成原型作用域的新类型, 定义处就报 conflicting types */

// 锚可用判据 + 几何回填 (入参可为 NULL): 1 = 窗口在册且 X 面已 associate
// (帧可以挂了); 0 = 不可用 (窗口不在册, 或 wl_surface 还没到) —— 调用方
// 等下一轮再试。
int wl_ohos_output_client_frame_anchor(uint32_t xwindow, int *x, int *y, int *w, int *h);

// X window id 是否仍在册。销毁即失效判据: 窗口销毁后同 id 会随复用的新窗
// 重新在册, 老帧必须在这条缝里丢弃 (不能投进新窗)。
int wl_ohos_output_client_xwindow_alive(uint32_t xwindow);

// 记录身份查询: 窗口 id 会被 X 复用, 只比 id 的失效判据会被"同 id 新窗"骗过
// (§2.7)。绑定方在挂接时存下 generation, 之后每拍比对 —— 不一致即该 id 已换
// 记录, 绑定失效。返回 0 = 该 id 当前不在册。
int wl_ohos_output_client_xwindow_generation(uint32_t xwindow, uint64_t *generation);

// 把一帧挂到该窗 (首次调用建节点并置顶于 X 面之上; 旧帧随节点解锁归还)。
// 返回 0 = 窗口不可挂 (不在册 / 锚不可用) —— 调用方必须自行丢弃 buffer。
// flip_vertical: 帧内容行序修正, 按 presenter 目标类型定 (实测依据:
//   wayland 渲染器 (graphics/egl_renderer.cpp ComposeZeroCopySamplingTransform)
//   的采样变换是 flipY = vulkanSource —— 即 venus(Vulkan) 面直取, GL(virgl) 面
//   要翻一次; wlroots 消费不带任何采样变换, 所以本侧要显式补: GL 面 = 1,
//   Vulkan 面 = 0。判反的症状: 图像上下颠倒)。
// surface_key: 该帧的归属键 ((pid<<32)|surface_id), 随显示序列发布给 guest
//   做"上屏的是不是我"的归属判定 (见 common/display_fps.h)。
int wl_ohos_output_client_frame_set(uint32_t xwindow, struct wlr_buffer *buffer,
                                    int flip_vertical, uint64_t surface_key);

// 摘掉该窗的帧 (解绑/收尾): 清 buffer + 停用节点。节点本身留到窗口记录
// 销毁时回收 (DestroyFrameNode), 届时一并归还它持有的队列 buffer。
// 窗口已销毁时是 no-op (那种情况节点已经没了)。
void wl_ohos_output_client_frame_clear(uint32_t xwindow);

// D23: 子窗 face 注册表巡检 —— 查询失败的窗口 (已销毁) 或同 id 重建
// (generation 变化) 的帧节点在此销毁归还。周期调用 (帧 tick 侧), 必须
// 在合成器事件循环线程。
void wl_ohos_output_child_faces_sweep(void);

// scene 里还挂着本模块队列 buffer 的节点数 (回归仪器, 判据: 窗口全部销毁后
// = 0)。数的是 scene 真实状态而不是本文件的记录 —— 帧节点一旦漏销毁, 窗口
// 记录早已 free, 只有 scene 上还留着它: 鬼影 + 队列槽位泄漏。
uint32_t wl_ohos_output_frames_in_scene(void);

/* 输出 present 序号 (每帧一次自增) —— 不变量检查器用 (known-issues §2.6) */
uint32_t wl_ohos_output_present_seq(void);

/* 当前显示周期 (ns): VSync 已接入时取系统上报值, 兜底节拍/未接入时 = 33ms。
 * guest 帧的生产节奏 (presenter 的 framePeriod) 必须跟这个走 —— 宿主按它
 * 回压 guest, 硬编码 30fps 会把 guest 钳在 30fps (M2 帧率债)。 */
uint64_t wl_ohos_output_frame_period_ns(void);

// 建 headless output (800x600) + 自定义 OHOS allocator + 帧定时器:
// 每帧绘制渐变+边框测试图案 → commit → NativeWindow 直推 (Attach+Flush)。
// xwayland 非 NULL 时监听其 new_surface, 已映射的 X client 窗口优先于测试
// 图案合成上屏 (T9 端到端)。
// display 用于建 wlr_output_layout (wl_output global 的载体): Xwayland
// rootless 的 X 屏幕尺寸来自它镜像的 wl_output global —— 没有 layout 时
// X 屏幕 0x0, wine xinerama 枚举到 0x0 显示器, is_window_rect_mapped 恒
// FALSE, 所有 wine 窗口永不 XMapWindow (T5 t5l trace 实测)。须在 output
// 创建后调用。
// window 为 NULL 合法 (D8, 2026-10-05): output/wl_output 照常建立 (X 屏幕
// 尺寸立刻正确, wine 桌面窗口的 map 判定不依赖画布时机), present 挂起,
// 画布后到经 wl_ohos_output_attach_window 挂载。
// out_w/out_h: 输出尺寸 (M3a 参数化, x11 台架 = surface 实际尺寸); ≤0 =
// 未指定, 回退 800x600 (smoke 台架口径, 证据链在该尺寸上校准)。
// canvas_egl_present: 画布 (DesktopAbility 全屏窗) 绑定时为 true —— 该
// surface 的消费侧对手工 FlushBuffer 冻结 (vd12/红背景/A-B 三重实测),
// 场景改经「默认 swapchain 渲染 → EGLImage 导入 → blit → eglSwapBuffers」
// 呈现 (vd22c 实测: 与零拷贝共用水式队列时 EGL 占格导致零拷贝 frame 2 起
// 静默回退拷贝路径, FlushBuffer 又不显示 ⇒ 画面冻在首帧)。fusion 预览窗
// 传 false, 保持已证零拷贝 + FlushBuffer 路径, 两模式互不干扰。
// 返回 0 = 链路建立; 非 0 = 失败 (hilog 已打点, 具体步骤看 "ohos-output" 标签)。
int wl_ohos_output_chain_start(struct wlr_backend *backend,
                               struct wlr_renderer *renderer,
                               struct wl_event_loop *loop,
                               struct wl_display *display,
                               OHNativeWindow *window,
                               struct wlr_xwayland *xwayland,
                               int out_w, int out_h,
                               bool canvas_egl_present);

// 画布晚到挂载 (D8): 在已启动的链的 output 上补跑窗口 present 配置, 下一
// 帧起出屏。仅允许 NULL→窗口 的一次转移 (运行中换窗不存在合法场景, surface
// 重建走 stop/start 全链); 已绑定返回非 0, 调用方负责销毁被拒窗口。
// loop 线程调用。canvas_egl_present 语义同 chain_start。
int wl_ohos_output_attach_window(OHNativeWindow *window, bool canvas_egl_present);

// 输出几何动态响应 (D10): 折叠/旋转使画布尺寸变化时同步 output mode /
// 背景 rect / buffer geometry。输入归一化与呈现 blit 随 g_out 自愈。
// loop 线程调用。返回 0 = 已应用 (含同尺寸 no-op)。
int wl_ohos_output_resize(int w, int h);

#ifdef __cplusplus
}
#endif
