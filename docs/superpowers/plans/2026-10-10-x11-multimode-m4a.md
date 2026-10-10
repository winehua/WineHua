# M4a: x11 多窗口 PC 最小闭环 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** x11 路线（winex11 + Xwayland + wlroots）跑通多窗口 PC 最小闭环：两个 wine 窗各自呈现在两个 OHOS 窗口，点击按窗路由。

**Architecture:** 新增 x11 toplevel 映射层（xs 生命周期 → toplevel_event_bus 22 事件语义，id 复用 ToplevelManager），映射层内建 per-xs 渲染循环（wlr render pass 把 xs texture 画进各窗 buffer，帧时钟复用 ohos_output VSync）；ArkTS 承载整链复用 wayland 路线（created→startWineWindowAbility→CreateRenderer，x11 分支只换 renderer 回绑目标）；输入按 toplevelId 路由（display_input 多窗分支关闭合成器命中）。模式位 = route(x11) × mode(fusion)，经 chain_start 新参数贯通到 native。

**Tech Stack:** C/C++ (wlroots 0.20 render pass API, libwayland), ArkTS (WineWindowManager/WineWindowAbility 整链复用), mingw host 探针 (fusion_probe), smoke 自动化。

**Spec:** `docs/superpowers/specs/2026-10-07-display-route-x11-multimode-m4-design.md`（本计划实现其 §6 M4a 行 + §2/§3.1-3.4/§4 设计；§3.5 弹出层是 M4c 不在本计划）

## Global Constraints

- 虚拟桌面零回退：每阶段提交后 `displayroute-win32-interactive` 前缀组维持 11/13（keyboard 两项为已知 XIM 缺口）
- 22 事件名字符串逐字红线：`toplevel_event_bus.h:75-100 ToplevelEventName()` 为唯一来源，映射层不得自造字符串
- id 空间复用：`ToplevelManager::AllocateToplevelId()`（toplevel_manager.h:340，uint32 从 1 原子递增）；surfaceKey = `(clientPid << 32) | xs->window_id`
- 判定只读归档（§2.4）：设备端只跑不判；x11-fusion 套件显式声明 route=x11 + multiwindow 档位
- 产品不为测试让步（原则 #23）：fusion_probe 只进 smoke 载荷，产品代码不引用
- 消费点白名单：DisplayRouteService.isX11() 的消费点新增须更新其头注释白名单（DisplayRouteService.ets:12-13）
- 改 `entry/src/main/cpp/` → `make NATIVE_ARCH=arm64-v8a hap`；不动 `thirdparty/wine/`（本计划零 wine submodule 改动）
- 验收纪律：验收判据先于实现确定（spec §7 已定，本计划不新增判据）

## Review Focus

按 spec 隐含但各 task 测试不直接覆盖的输入类，最可能咬人的排前：

1. **route 切换后的会话残留**：fusion 会话 → 虚拟桌面会话切换时，映射层 id 表/渲染循环/attach 的 NativeWindow 必须随会话重置清空（spec §3.4「会话重置对齐」）——Task 2 的 stop 路径测试钉住（x11_toplevel_session_reset() 在 wine 停止链被调）。
2. **xs 销毁竞态**：dissociate/destroy 与渲染循环并发（渲染循环正画着一个刚死的 xs texture）——Task 3 的 destroy 监听摘除 + has_content 谓词测试钉住（spec §5 第 2 行）。
3. **首帧早于 NativeWindow 回绑**：created 事件先于 ArkTS createRenderer 回绑，渲染循环必须跳帧不崩（spec §5 第 1 行）——Task 3 的 skip 计数日志测试钉住。
4. **点击落在窗间缝隙/未覆盖区域**：按窗命中的坐标反推边界（OHOS 窗局部 → X 全局），缝隙点击必须不投递也不崩——Task 5 的越界用例钉住。
5. **fusion_probe 双窗 z 序与遮挡**：两窗重叠时 rgba-quadrants 按窗裁剪的 region 必须对应各自可见区域——Task 6 的布局设计（并排不重叠）规避而非依赖 z 序。

---

### Task 1: 模式位贯通（chain_start multiwindow 参数 + ArkTS 联合决策 + 门禁）

**Files:**
- Modify: `entry/src/main/cpp/display/display_compositor.cpp`（StartWithSurface 签名与 DeferredOutputChainStart）
- Modify: `entry/src/main/cpp/display/ohos_output.c`（chain_start 签名 + g_out.multiwindow 位）
- Modify: `entry/src/main/cpp/display/ohos_output.h`（chain_start/attach_window 签名）
- Modify: `entry/src/main/cpp/display_route/display_route_napi.cpp`（NAPI startDisplayRouteChain 第 6 参）
- Modify: `entry/src/main/ets/service/WineWindowManager.ets`（getEffectiveScale 附近新增 shouldUseMultiwindow()）
- Modify: `entry/src/main/ets/entryability/DesktopAbility.ets` 与 `entry/src/main/ets/pages/DesktopWindow.ets`（调用点透传）

**Interfaces:**
- Consumes: `DisplayRouteService.isX11()`（DisplayRouteService.ets:35）、`DesktopModeService.getInstance().mode`（DesktopModeService.ets:30，'virtual'|'fusion'）
- Produces: `wl_ohos_output_chain_start(..., bool multiwindow)`（ohos_output.h 尾部追加参数，默认 false 语义不变）；`g_out.multiwindow`（bool，全文件可读）；ArkTS `WineWindowManager.shouldUseMultiwindow(): boolean`（= isX11 && mode==='fusion'）

- [x] **Step 1: ArkTS 联合决策函数（写实现前先写调用点使编译失败）**

在 `WineWindowManager.ets` 的 `isDesktopMode` 字段定义（:64）附近加：

```typescript
  // M4a: x11 多窗模式 = route(x11) × mode(fusion)。消费点: 画布启动参数
  // (DesktopAbility/DesktopWindow) 与 openX11Canvas 门禁。模式位只在会话
  // 启动时定值, 会话内不切换 (spec §3.4)。
  shouldUseMultiwindow(): boolean {
    return DisplayRouteService.getInstance().isX11() &&
      DesktopModeService.getInstance().mode === 'fusion';
  }
```

文件头 import 补 `DesktopModeService`（若无）。

- [x] **Step 2: NAPI 与 chain_start 参数贯通**

`display_route_napi.cpp` 的 `StartDisplayRouteChain`（现为 `(surfaceId, script_enabled, w, h, canvas_egl_present)` 5 参）：追加第 6 可选参 `multiwindow`（napi 可选参数取法照抄第 5 参）。透传至 `WineHua_DisplayRoute_StartWithSurface(surface_id, script_enabled, out_w, out_h, canvas_egl_present, multiwindow)`；`display_compositor.cpp:612` 签名同步扩参，存入 `DeferredOutputChainStart`（:268 附近结构体加字段）与 `ohos_output_chain_start` 调用（:286）；`ohos_output.c` chain_start 实现签名扩参 + `g_out.multiwindow = multiwindow;`（memset 之后、任何使用之前）。struct 加 `bool multiwindow;`。

ArkTS 调用点：`DesktopWindow.ets:70`（onSurfaceCreated 的 startDisplayRouteChain）与 `DesktopAbility.ets` 的同款调用点，末尾追加第 6 参 `this.multiwindow`（两文件各自从 `WineWindowManager.getInstance().shouldUseMultiwindow()` 取，构造字段缓存）。注意 DesktopWindow/DesktopAbility 在多窗模式**不应被打开**（Task 4 的门禁管），本 task 只透传参数保证语义完整。

- [x] **Step 3: 构建验证**

Run: `make NATIVE_ARCH=arm64-v8a hap`
Expected: 构建绿（虚拟桌面行为不变——multiwindow 缺省 false 时所有路径与现状逐字节等价）

- [x] **Step 4: 虚拟桌面冒烟（零回退门禁）**

Run: 部署后 `python3 automation/smoke.py run --suite core --device <DEV>` 
Expected: 3/4（opengl-x86 已知 D49 FAIL，其余 PASS）——虚拟桌面路径未受影响

- [x] **Step 5: Commit**

```bash
git add -A entry/src/
git commit -m "feat(display): M4a-T1 模式位贯通——chain_start multiwindow 参数 + ArkTS 联合决策"
```

---

### Task 2: x11_toplevel 映射层骨架（xs 事件 → bus 事件 + 会话重置）

**Files:**
- Create: `entry/src/main/cpp/display/x11_toplevel.cpp` / `x11_toplevel.h`
- Modify: `entry/src/main/cpp/display/ohos_output.c`（HandleNewSurface 模式分叉：multiwindow 时调映射层而非建 ohos_client_surface）
- Modify: `entry/src/main/cpp/display/display_compositor.cpp`（fail 收尾调 session_reset；StartWithSurface multiwindow 时初始化映射层）
- Modify: `entry/src/main/cpp/bridge/napi_init.cpp`（WaylandServer sink 已存在——映射层用同一 bus，无需新 TSFN）

**Interfaces:**
- Consumes: `ToplevelManager::AllocateToplevelId()`（toplevel_manager.h:340）、`WaylandServer::PostToplevelEvent(id, evt, json)`（wayland_server.h:93）、`ToplevelEventType::Created/Destroyed/Resize/Title`（toplevel_event_bus.h:42-72）、`JsonCreated(w,h)`（toplevel_event_bus.h:132）、`struct wlr_xwayland_surface`（associate/map/unmap/set_title/destroy 事件，wlroots xwayland.h）
- Produces（x11_toplevel.h 全部公开面）:
  - `void x11_toplevel_notify_new_surface(struct wlr_xwayland_surface *xs);`（ohos_output HandleNewSurface 的 multiwindow 分支调用；内部挂 per-xs 监听）
  - `void x11_toplevel_session_reset(void);`（wine 停止链调用：清 id 表/摘全部监听/通知 ArkTS destroyed×N）
  - `struct wlr_xwayland_surface *x11_toplevel_xs_of(uint32_t toplevelId);`（输入路由查表）
  - `uint32_t x11_toplevel_id_of_xs(struct wlr_xwayland_surface *xs);`（0 = 未登记）
  - `bool x11_toplevel_active(void);`（g_multiwindow 位镜像，供 plugin_manager 分支判断）

- [x] **Step 1: 写映射层骨架（头文件 + 三个生命周期函数体）**

`x11_toplevel.h`：

```c
#ifndef X11_TOPLEVEL_H
#define X11_TOPLEVEL_H
#include <stdbool.h>
struct wlr_xwayland_surface;
/* M4a: xs 生命周期 → toplevel_event_bus 映射 (spec §3.1)。
 * 只做映射不做策略; id 复用 ToplevelManager; 事件名逐字走
 * ToplevelEventName 红线。多窗模式才激活 (x11_toplevel_active)。 */
void x11_toplevel_set_active(bool on);
bool x11_toplevel_active(void);
void x11_toplevel_notify_new_surface(struct wlr_xwayland_surface *xs);
void x11_toplevel_session_reset(void);
struct wlr_xwayland_surface *x11_toplevel_xs_of(uint32_t toplevelId);
uint32_t x11_toplevel_id_of_xs(struct wlr_xwayland_surface *xs);
#endif
```

`x11_toplevel.cpp` 骨架（完整实现 created/destroyed，title/resize 同构后补到 Task 3）：

```cpp
#include "x11_toplevel.h"
#include <wlr/xwayland/xwayland.h>
#include <wayland-server-core.h>
#include "compositor/wayland_server.h"
#include "compositor/toplevel/toplevel_event_bus.h"
#include "compositor/toplevel/toplevel_manager.h"

namespace {
struct XsEntry {
    struct wlr_xwayland_surface *xs;
    uint32_t toplevelId;
    struct wl_listener destroy;      // xs->events.destroy
    struct wl_listener associate;    // 首帧判定入口 (Task 3 用)
    struct wl_listener set_title;    // Task 3
    struct wl_listener configure;    // Task 3
};
std::vector<XsEntry *> g_entries;
bool g_active = false;

XsEntry *entry_of_xs(struct wlr_xwayland_surface *xs) { /* 线性查 g_entries */ }

void handle_xs_destroy(struct wl_listener *l, void *) {
    XsEntry *e = wl_container_of(l, e, destroy);
    WaylandServer::GetInstance()->PostToplevelEvent(
        e->toplevelId, ToplevelEventType::Destroyed);
    // 摘其余监听、从 g_entries 摘链、free
}
}  // namespace

void x11_toplevel_set_active(bool on) { g_active = on; }
bool x11_toplevel_active(void) { return g_active; }

void x11_toplevel_notify_new_surface(struct wlr_xwayland_surface *xs) {
    if (!g_active || !xs) return;
    XsEntry *e = new XsEntry{xs, 0, {}, {}, {}, {}};
    e->toplevelId = WaylandServer::GetInstance()->ToplevelManagerRef().AllocateToplevelId();
    e->destroy.notify = handle_xs_destroy;
    wl_signal_add(&xs->events.destroy, &e->destroy);
    g_entries.push_back(e);
    // created 事件: M4a 首版用 associate+buffer 判定 (spec §3.1);
    // 骨架期先在 associate 回调里发 Created(JsonCreated(w,h))
}
```

注意 `ToplevelManagerRef()` 若 WaylandServer 无此访问器，用现有公开口（侦察确认 `GetDesktopCompositor()` 同级有 toplevelMgr_ 的访问需求时在 wayland_server.h 加 `ToplevelManager &ToplevelManagerRef()` 一行转发——最小改动）。

- [x] **Step 2: ohos_output 模式分叉 + 会话重置挂点**

`ohos_output.c` `HandleNewSurface`（原 :1354，D45 修复后行号 +20 左右）入口加：

```c
    if (x11_toplevel_active()) {
        x11_toplevel_notify_new_surface(xs);
        return;   /* 多窗模式: 不建 ohos_client_surface/scene 节点 (spec §3.2) */
    }
```

`display_compositor.cpp` fail 收尾（`wl_ohos_output_shutdown();` 之后）加 `x11_toplevel_session_reset();`；`StartWithSurface` 的非 already-started 分支在起线程前加 `x11_toplevel_set_active(multiwindow);`。`ohos_output.c` include `x11_toplevel.h`。

- [x] **Step 3: 构建验证**

Run: `make NATIVE_ARCH=arm64-v8a hap`
Expected: 绿。虚拟桌面（active=false）行为逐字节不变。

- [x] **Step 4: 真机冒烟——多窗模式 created/destroyed 事件到达**

部署后，临时验证通道（不进产品）：`aa start --ps winehua.displayRoute x11 --ps winehua.desktopMode fusion --ps winehua.autoStart 1`（managed 模式，无 explorer desktop——Task 1 的门禁未完成前 desktop-shell 分支仍会跑，此处只验证映射层事件），随后 `winehua.program` 直启 notepad：

```bash
hdc -t <DEV> shell "hilog -x | grep -E 'FireToplevel|created'" # 期待 [MW] FireToplevel id=N created
```

Expected: notepad 窗口创建时 `created` 事件日志（含 w/h），关闭时 `destroyed`。虚拟桌面模式（无 desktopMode fusion 参数）回归：notepad 走旧 scene 路径，无 FireToplevel。

- [x] **Step 5: Commit**

```bash
git add entry/src/main/cpp/display/x11_toplevel.* entry/src/main/cpp/display/ohos_output.c entry/src/main/cpp/display/display_compositor.cpp
git commit -m "feat(display): M4a-T2 x11 toplevel 映射层骨架——created/destroyed 事件 + 会话重置"
```

---

### Task 3: per-xs 渲染循环（texture 采样 + buffer 直推 + 跳帧）

**Files:**
- Modify: `entry/src/main/cpp/display/x11_toplevel.cpp`（渲染循环 + attach/detach window 接口）
- Modify: `entry/src/main/cpp/display/x11_toplevel.h`（`x11_toplevel_attach_window(uint32_t toplevelId, OHNativeWindow *win, int w, int h)` / `x11_toplevel_detach_window(uint32_t toplevelId)`）
- Modify: `entry/src/main/cpp/display/ohos_output.c`（帧时钟驱动口：FrameTick 内 multiwindow 分支调 `x11_toplevel_render_tick()`）

**Interfaces:**
- Consumes: `wlr_renderer_begin_buffer_pass` / `wlr_render_pass_add_texture` / `wlr_texture_from_buffer`（wlroots 0.20 render/pass.h:57、wlr_compositor.h:363——spec §3.1 已源码验证）、`wlr_surface_get_texture(xs->surface)`、ohos_output 的 VSync 帧时钟（FrameTick，ohos_output.c）
- Produces: `x11_toplevel_render_tick(void)`（每帧调，遍历 entries：已 attach 且 surface 有新 buffer → 画进该窗 buffer 并 Flush；未 attach → skip 计数并 hilog 采样）；attach 的窗走 ohos_buffer 的 Request/Lock/Flush 纪律（`wl_ohos_buffer_*` 现有口，ohos_buffer.h）

- [x] **Step 1: texture 缓存与跳帧骨架**

XsEntry 增字段：`struct wl_listener commit; struct wlr_texture *tex; uint64_t last_commit_seq; OHNativeWindow *win; int win_w, win_h; uint32_t skip_count;`。commit 回调置 dirty 位（记录 `xs->surface->commit_seq`）。`x11_toplevel_render_tick()` 遍历：

```cpp
for (XsEntry *e : g_entries) {
    if (!e->win) { if (++e->skip_count % 60 == 1) OH_LOG_WARN(...,"skip no-window id=%u n=%u", ...); continue; }
    if (!e->dirty) continue;
    e->dirty = false;
    render_entry(e);   // buffer request → pass begin → add_texture(xs->surface) → submit → Flush
}
```

`render_entry` 的 buffer 获取复用 `wl_ohos_buffer` 现有 Request/Slot 纪律（ohos_buffer.h 公开口；每窗独立 slot 组——若现有 slot 结构是全局单窗形态，本 task 内参数化：`wl_ohos_buffer_request_for(uintptr_t key)` 以 toplevelId 为 key 分组）。texture 创建缓存于 e->tex（`wlr_texture_from_buffer`，buffer 变更时销毁重建）。

- [x] **Step 2: destroy/resize 竞态防御**

`handle_xs_destroy`（Task 2）扩展：先 `wl_list_remove(&e->commit.link)` + 若 e->tex `wlr_texture_destroy` + detach window（不销毁 ArkTS 窗，只解绑）。resize：ArkTS `ResizeRenderer` 的 x11 分支（Task 4）调 `x11_toplevel_resize_window(id, w, h)` 置 e->win_w/h + buffer geometry。

- [x] **Step 3: 构建验证**

Run: `make NATIVE_ARCH=arm64-v8a hap`
Expected: 绿。虚拟桌面路径不变（render_tick 在 FrameTick 的 `if (g_out.multiwindow)` 分支内）。

- [x] **Step 4: 真机验证——双窗呈现（临时通道，无 ArkTS 承载前的裸证）**

融合模式起会话，直启 notepad + 第二程序（或 notepad 内开新窗）：两窗的 xs 各自 created；**宿主侧无窗呈现不可见**——本步只验证 render_tick 的 skip 计数日志与 commit dirty 日志（`hilog -x | grep -E 'skip no-window|render tick'`）。真呈现验证在 Task 4 承载接通后。

- [x] **Step 5: Commit**

```bash
git add entry/src/main/cpp/display/x11_toplevel.cpp entry/src/main/cpp/display/x11_toplevel.h entry/src/main/cpp/display/ohos_output.c
git commit -m "feat(display): M4a-T3 per-xs 渲染循环——texture 缓存 + buffer 直推 + 跳帧/竞态防御"
```

---

### Task 4: ArkTS 承载接线（created → WineWindowAbility → renderer 回绑 x11 分支）

**Files:**
- Modify: `entry/src/main/cpp/bridge/plugin_manager.cpp`（CreateRenderer 的 x11 分支）
- Modify: `entry/src/main/ets/service/WineWindowManager.ets`（'created' case 的 x11 门 + openX11Canvas 门禁）
- Modify: `entry/src/main/ets/service/WineEnvService.ets`（ready 时 openX11Canvas 调用点 :502/:518 加多窗门禁）
- Modify: `entry/src/main/ets/entryability/WineWindowAbility.ets`（onSurfaceCreated→createRenderer 透传，onSurfaceDestroyed→detach）

**Interfaces:**
- Consumes: Task 2 的 created 事件（ArkTS `onToplevelEvent` 'created' case → `startWineWindowAbility(id, data)` 现链 :581-588）、Task 3 的 `x11_toplevel_attach_window`、`PluginManager::CreateRenderer(uint32_t, int64_t)`（plugin_manager.h:33）
- Produces: `PluginManager::CreateRenderer` x11 分支——`x11_toplevel_active()` 时：`OH_NativeWindow_CreateNativeWindowFromSurfaceId(surfaceId, &win)` 后调 `x11_toplevel_attach_window(toplevelId, win, w, h)`（不建 EglRenderer，不进 toplevelRenderers_）；`ResizeRenderer`/`DestroyToplevel` 同样分支（resize→`x11_toplevel_resize_window`，destroy→`x11_toplevel_detach_window` + `sendToplevelClose` 语义不变）

- [x] **Step 1: plugin_manager x11 分支**

`plugin_manager.cpp` `CreateRenderer`（:39-80 绑定链）入口加：

```cpp
    if (x11_toplevel_active()) {
        OHNativeWindow *win = nullptr;
        if (OH_NativeWindow_CreateNativeWindowFromSurfaceId((uint64_t)surfaceId, &win) == 0 && win) {
            x11_toplevel_attach_window(toplevelId, win, 1, 1);
        }
        return;   /* 不建 EglRenderer: x11 per-xs 呈现归映射层 (spec §4) */
    }
```

`ResizeRenderer`（:89-108）与 `DestroyToplevel` 同样入口分支（后者先 detach 再走既有清理）。

- [x] **Step 2: ArkTS 门禁与事件流**

`WineEnvService.ets` :502/:518 的 `openX11Canvas()` 调用点加门：`if (!WineWindowManager.getInstance().shouldUseMultiwindow()) openX11Canvas();`——多窗模式不开虚拟桌面画布（spec §3.4「openX11Canvas 仅虚拟桌面分支调用」）。`WineWindowManager.onToplevelEvent` 的 'created' case（:581-588）：现状 `isDesktopMode` 跳过——多窗模式下 `isDesktopMode=false`（mode=fusion 非 virtual），case 自然落入 `startWineWindowAbility` ✓；但 `desktopRootId` 相关的 'desktop_root' case 在多窗模式不会触发（无 explorer desktop）✓ 零改动。

`WineWindowAbility.ets`：`onSurfaceCreated` 现有 `createRenderer(toplevelId, surfaceId)` 调用不动（plugin_manager 内部分支）；确认 `onSurfaceDestroyed` 走 `destroyToplevel` → x11 分支 detach。

- [x] **Step 3: 构建部署验证——双窗真实呈现**

Run: `make NATIVE_ARCH=arm64-v8a hap` + 部署 + 融合模式会话 + `winehua.program` 直启 notepad，notepad 内开第二窗（或直启两个程序）。

Expected（截屏）：两个独立 OHOS 窗口各自呈现对应 X 窗内容；关闭一窗另一窗存活；`hilog` 无 skip no-window 持续刷屏（回绑后追上）。截屏归档 `.temp/m4a-t4-two-windows.jpeg`。

- [x] **Step 4: 虚拟桌面回归**

Run: core 套件 + 手动虚拟桌面模式起 notepad
Expected: 3/4 基线；虚拟桌面 notepad 走单画布（无 WineWindowAbility 弹窗）。

- [x] **Step 5: Commit**

```bash
git add entry/src/
git commit -m "feat(ets+bridge): M4a-T4 ArkTS 承载接线——created→WineWindowAbility→renderer x11 分支回绑"
```

---

### Task 5: 按窗输入路由（display_input 多窗分支 + NAPI + ArkTS 事件）

**Files:**
- Modify: `entry/src/main/cpp/display/display_input.c`（多窗分支：关闭合成器命中，开放按窗路由口）
- Modify: `entry/src/main/cpp/display/x11_toplevel.cpp/.h`（`x11_toplevel_input_pointer(uint32_t id, int x, int y, uint32_t action)` / `x11_toplevel_input_key(uint32_t id, uint32_t keycode, bool press)`）
- Modify: `entry/src/main/cpp/bridge/napi_init.cpp`（新 NAPI `x11SendPointer(toplevelId, x, y, action)` / `x11SendKey(toplevelId, keycode, press)`，注册表 :1130 附近）
- Modify: `entry/src/main/ets/entryability/WineWindowAbility.ets`（触摸/按键事件 → 新 NAPI，手势面复用 WineWindow.ets 现有状态机）

**Interfaces:**
- Consumes: Task 2 的 `x11_toplevel_xs_of(id)`、wlroots seat（`wl_ohos_input_seat_create` 已建，display_input.c）、`wlr_seat_pointer_notify_enter/motion/button`、`wlr_xwayland_surface_activate`
- Produces: 坐标反推——ArkTS 窗局部 (x,y) + xs 偏移 → X 全局 = `xs->x + x_local, xs->y + y_local`（x_local 越界 [0,xs->width) 不投递）；按键先 `wlr_seat_keyboard_notify_enter(xs->surface)` 再 notify_key

- [x] **Step 1: 映射层路由实现**

```cpp
void x11_toplevel_input_pointer(uint32_t id, int lx, int ly, uint32_t action) {
    XsEntry *e = entry_of_id(id);
    if (!e || !e->xs) return;
    struct wlr_xwayland_surface *xs = e->xs;
    if (lx < 0 || ly < 0 || lx >= (int)xs->width || ly >= (int)xs->height) return;  /* 缝隙不投递 */
    int gx = xs->x + lx, gy = xs->y + ly;
    wlr_xwayland_surface_activate(xs);           /* activate 先行 (focus 纪律) */
    wl_ohos_input_pointer_notify(e->xs->surface, gx, gy, action);  /* display_input 现有 seat 口封装 */
}
```

display_input.c 暴露 `wl_ohos_input_pointer_notify(struct wlr_surface*, int gx, int gy, uint32_t action)`（内部 enter/motion/button 序列，焦点 settle 纪律沿用 :358 FOCUS_SETTLE_MS 机制）；多窗模式下合成器命中路径（`client_topmost_at`）天然不活跃（无 g_clients）。

- [x] **Step 2: NAPI + ArkTS**

napi_init.cpp 注册 `x11SendPointer`/`x11SendKey`（照抄现有 testNapi 注入类函数的参数解析模板）。WineWindowAbility 触摸回调（现走 testNapi 注入的调用点）加 x11 分支：`shouldUseMultiwindow ? x11SendPointer(this.toplevelId, x, y, action) : 现有调用`。按键同构。

- [x] **Step 3: 构建部署验证——点击命中**

直启 notepad + 第二窗，uitest 点击窗 A 文本区（物理坐标换算 OHOS 窗局部）：光标出现于窗 A；点击窗 B 标题栏：窗 B 激活置前。归档截屏 `.temp/m4a-t5-click-routing.jpeg`。

- [x] **Step 4: Commit**

```bash
git add entry/src/
git commit -m "feat(display+ets): M4a-T5 按窗输入路由——pointer/key 按 toplevelId 直达 + 缝隙不投递"
```

---

### Task 6: fusion_probe 探针 + x11-fusion 套件 + 判定

**Files:**
- Create: `smoke/programs/win/fusion_probe.c`（mingw host 探针）
- Create: `smoke/tests/fusion-probe/test.json`
- Create: `smoke/suites/fusion.json`
- Create: `smoke/jobs/x11-fusion.json`
- Modify: `automation/smoke.py`（build_case 的 fusion_probe 编译条目，:279-317 附近）
- Modify: `automation/checks/__init__.py` 或复用 `visual` 判定器（按窗 region 双窗判定——用既有 `visual:rgba-quadrants` 的 region 机制，checks/__init__.py:93-136）

**Interfaces:**
- Consumes: mingw 编译链（smoke.py:41 MINGW）、`visual` 判定器 region 裁剪（frame.py:38 `validate_rgba_quadrants(path, step, region)`）、`presented-route` 判定器
- Produces: fusion_probe 的 result JSON 含 `clickCounts: {"a": N, "b": M}`（窗内 WM_LBUTTONDOWN 计数，经 winehua_smoke_protocol 的 result 文件上报）；两窗并排布局（窗 A 左 50% 纯色 #FF0000 + 白色十字标，窗 B 右 50% 四象限 rgba-quadrants-v1 同款标记）——**并排不重叠**（Review Focus #5）

- [x] **Step 1: fusion_probe.c**

双窗 Win32 程序（`smoke/programs/win/winehua_dns_probe.c` 为模板——同款 result 协议 + 消息循环）：`WinMain` 创建两个 WNDCLASS 窗（A: `CreateWindowExA` 800x600 @ (100,100) 背景 RGB(255,0,0)，WM_PAINT FillRect 纯色+中心白十字；B: 800x600 @ (920,100) 四象限 FillRect：左上 R/右上 G/左下 B/右下 白，各带 2px 黑边）；WM_LBUTTONDOWN 计数入 `g_clicks[wnd]`；定时（2s）把 `{"clickCounts":{"a":N,"b":M},"fixedFrame":"fusion-two-window-v1"}` 写 result 文件（协议头复用 `winehua_t_check.h`/protocol 模式——smoke/programs/common/winehua_t_check.h）。两窗同时显示。

- [x] **Step 2: 套件与判定声明**

`smoke/tests/fusion-probe/test.json`：

```json
{ "id": "fusion-probe", "title": "M4a 双窗呈现+按窗输入",
  "exe": "fusion_probe.exe", "arch": ["x64"], "from_wine": null,
  "seconds": 12, "timeoutMs": 120000,
  "checks": ["result-json", "presented-route",
             "visual:fusion-window-a", "visual:fusion-window-b",
             "wine-trace:fusion-click-a"] }
```

`smoke/suites/fusion.json`：`{"name":"fusion","title":"x11 fusion 多窗","tests":[{"case":"fusion-probe","id":"fusion-probe","backend":{"d3d":"wined3d"},"env":{"WINEHUA_DISPLAY_ROUTE":"x11"}}]}`；`smoke/jobs/x11-fusion.json`：`{"displayroute":true,"suite":"fusion","params":{"env":{"WINEHUA_DISPLAY_ROUTE":"x11","multiwindow":"1"}}}`（multiwindow env → SmokeRunner/ArkTS 消费，Task 4 门禁的套件侧等价——**执行时核实**：若 mode 决策只走 DesktopModeService，则 job 需带 `winehua.desktopMode=fusion` 的等价下发口，照 displayroute-suite.json 的 params 形态补）。

判定实现：`visual:fusion-window-a/b` 在 `visual` 判定器（checks/__init__.py:93-136）加 case——region 取 preview-rect 左/右半（displayroute-preview-rect.json 的 rect 各切 50%），A 区验纯红+白十字、B 区验四象限（复用 validate_rgba_quadrants 的 B 侧与新增纯色校验）。`wine-trace:fusion-click-a` 校验 result JSON `clickCounts.a >= 1 且 b == 0`（点击只发给窗 A——host 用 uitest 点击窗 A 中心，编排进 suite argv 或 `--seconds` 内的固定点击脚本，复用 D11 的 uitest 驱动模式）。

- [x] **Step 3: 跑通验收**

Run: `python3 automation/smoke.py run --suite fusion --device <DEV>`（或 job 文件跑法照 displayroute-suite）
Expected: fusion-probe PASS——双窗呈现（A 区红/十字、B 区四象限）、点击窗 A 计数 +1 且 B 为 0。

- [x] **Step 4: Commit**

```bash
git add smoke/ automation/
git commit -m "feat(smoke): M4a-T6 fusion_probe 双窗探针 + x11-fusion 套件 + 按窗判定"
```

---

### Task 7: 回归门禁 + 文档收口

**Files:**
- Modify: `docs/engineering/display-route-known-issues.md`（M4a 状态行）
- Modify: 本计划文件勾选完成项

- [x] **Step 1: 虚拟桌面全量回归**

Run: `displayroute-win32-interactive` 前缀组全量（spec §6 冻结行为门禁）
Expected: 11/13 维持（keyboard 两项已知 XIM 缺口）。

- [x] **Step 2: core 基线**

Run: `python3 automation/smoke.py run --suite core --device <DEV>`
Expected: 3/4（opengl-x86 = D49 已知）。

- [x] **Step 3: 已知限制记录**

Pad 承载（M4d）未含；弹出层（M4c）未含；fusion 模式下 closeX11Canvas/ProcessService 的 D45 判据不适用（无 desktop shell——记录到 known-issues，防后续误判）。

- [ ] **Step 4: Commit**

```bash
git add docs/ docs/superpowers/plans/2026-10-10-x11-multimode-m4a.md
git commit -m "docs: M4a 收口——回归结果与已知限制"
```

---

## Self-Review 记录

1. **Spec 覆盖**：§3.1 映射层（T2/T3）、§3.2 分叉与模式位（T1/T2）、§3.3 输入（T5）、§3.4 门禁/就绪/重置（T1/T2/T4）、§4 数据流（T4）、§6 M4a 行验收（T6）、§7 判据 1-4（T6/T7）。§2 架构逐项落位。无缺口。
2. **占位符扫描**：T2 的 `entry_of_xs`/T3 的 `render_entry` 给了骨架与职责边界（非「similar to」式占位）；T6 的 uitest 点击编排标注了「执行时核实」的唯一不确定点（job 对 desktopMode 的下发口形态）。
3. **类型一致性**：`x11_toplevel_attach_window/detach_window/resize_window` 在 T3 定义、T4 消费；`x11_toplevel_input_pointer/key` 在 T5 定义与消费一致；`shouldUseMultiwindow()` T1 定义 T4/T5 消费。
4. **Review Focus 覆盖**：#1→T2 Step2（session_reset 挂 fail 收尾）；#2→T3 Step2（destroy 摘除+谓词）；#3→T3 Step1（skip 计数）；#4→T5 Step1（越界 return）；#5→T6 Step1（并排布局规避）。
