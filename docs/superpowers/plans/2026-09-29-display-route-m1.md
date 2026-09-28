# Display Route M1 实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** winex11 交叉接入 displayroute 的 Xwayland，输入可用、窗口语义正确（记事本判据），scene 化提帧率，GLX 桥与 R-WSI 出裁决数据。

**Architecture:** 在 M0 全链（libX11 client → Xwayland NCP → wlroots → NativeWindow）上扩展三线：①输入线 = wlroots `wlr_seat` + xwm_set_seat + OHOS 事件注入桥；②渲染线 = ohos_output 的手搓 blit 换 wlr_scene（frame callback/damage 交 scene，目标 ≥25fps）；③wine 线 = guest 侧 x86_64 X11 栈 + wine `--with-x` + win32u OHOS bypass 加 DISPLAY 分支 → winex11.drv 接管。

**Tech Stack:** wlroots 0.20.2（wlr_seat/wlr_scene）、xkbcommon（键盘 keymap）、wine 10.x（winex11.drv）、guest x86_64 libX11/libxcb（M0 构建脚本按侧参数化）、virpipe EGL（GLX 桥 spike）。

**Spec:** docs/superpowers/specs/2026-09-27-display-route-x11-wlroots-design.md（§5 R1/R-WSI、§6.2 M0 实测结论、§7 M1 行）

## Global Constraints

- 分支：`feature/display-route-m0` 续推（用户裁决 2026-09-29，不合 master）
- submodule 保持上游净树，一切改动走 `scripts/patches/*.patch`；主仓库指针只在 submodule 推送到自身远程后才有意义
- Xwayland 钉 24.1.13、wlroots 0.20.2、wayland 1.26 不动版
- Wine 图形驱动选择只扩 `win32u/driver.c` 的 `__OHOS__` bypass，不动 PnP/注册表机制
- Xwayland argv 已带 `-ac`（沙箱 peercred 豁免未生效，M0-T9 实证）；输入链落地后若可换 peercred/xauth 另议，本计划不改
- guest (x86_64) 与 host (aarch64) 两侧库不得混链（侧别纪律见 .claude/rules/build-and-log.md）
- 每个 Bash 后台长命令先 `make -n <目标>` 验证目标存在（M0-T10 踩坑：虚构 full 目标空转一轮）

## Review Focus

1. **焦点窗口切换后的输入归属**：winex11 路线启用时旧 winewayland 链路必须不受影响（DISPLAY 未设 = 现行为不变）。测试：T4 中关 DISPLAY 跑 core 套件应 PASS。
2. **注入事件的坐标系**：OHOS 触摸坐标（物理像素）→ X 窗口局部坐标，须经 displayroute 的窗口几何换算；XComponent 缩放（4:3 aspectRatio）不可忽略。测试：T5 中点按记事本菜单项，命中项与视觉位置一致。
3. **多窗口焦点切换**：第二个窗口点击后键盘必须随焦点走（wlr_seat keyboard focus），旧窗口不得继续收键。测试：T2 双窗口输入隔离。
4. **guest/host 库侧别**：wine guest (x86_64) 链的 libX11 必须是 guest 侧产物，误链 host (aarch64) .so 即加载失败。测试：T4 中 `file` 断言 ELF arch。
5. **scene 化后的 client 内容**：换 wlr_scene 后 X 窗口内容不得消失/冻结（M0-T9 的 frame_done/configure 手动应答由 scene 接管，遗漏即内容停留首帧）。测试：T3 条纹滚动 + 截图对比。

---

### Task 1: displayroute 输入链（wl_seat + 键盘注入）

**Files:**
- Modify: `entry/src/main/cpp/display/display_compositor.cpp`（wlr_seat 创建 + xwm_set_seat）
- Create: `entry/src/main/cpp/display/display_input.cpp` / `.h`（OHOS 事件 → wlr_seat 注入桥）
- Modify: `entry/src/main/cpp/display/ncp/xclient_child.cpp`（键盘回显测试功能）
- Modify: `entry/src/main/cpp/CMakeLists.txt`（新源文件 + smoke NAPI 扩展）

**Interfaces:**
- Consumes: M0 的 `wl_ohos_output_chain_start`（已持 `struct wlr_xwayland *`）、wlroots `wlr_seat`/`wlr_keyboard`/`xkbcommon` API、M0 的 NCP 触发协议
- Produces: `display_input_inject_key(uint32_t keycode, bool press)` / `display_input_inject_motion(float x, float y, int phase)`（NAPI 可调，坐标 = XComponent 局部归一化坐标）；`wl_ohos_input_attach(struct wlr_seat*)` 由 chain 内部调用

- [ ] **Step 1: 调研 keymap 数据源**

Run: `ls thirdparty/xkeyboard-config/ | head -3; grep -n 'XKB_CONFIG_ROOT' entry/src/main/cpp/wine/wine_env.cpp | head -2`
确认 xkeyboard-config 数据树随 wine-data 打包的路径（M0 已有 XKB 数据装配：assemble.sh wine_data/xkb）。若 xkbcommon 规则数据不在打包面，补 assemble.sh 装配段（照 xkm 段的写法，断言 rules/evdev 存在）。

- [ ] **Step 2: seat 创建接入 displayroute**

在 `display_compositor.cpp` 的 chain 建立（`wlr_xwayland_create_with_server` 之后）加：
```cpp
struct wlr_seat *seat = wlr_seat_create(wl, "default");
/* wlr_xwayland_set_seat 需在 xwm 建立后调; M0 已证实 ready 事件在
 * xwm_create 之后, 在 HandleXwaylandReady 里调 */
```
`HandleXwaylandReady` 内调 `wlr_xwayland_set_seat(xwayland, seat)`。头文件 include 放既有 extern "C" 块（wlroots 头 C++ 不安全，M0 惯例）；`wlr_seat.h` 若含 C++ 不安全类型则与 `ohos_output.c` 同法下沉 C 文件。键盘 keymap：`xkb_keymap_new_from_names(ctx, &names)`（names = rules "evdev", layout "us"）+ `wlr_keyboard_keymap`。

- [ ] **Step 3: 注入桥 display_input.cpp**

```c
/* display_input.c — OHOS 事件 → wlr_seat 注入 (M1-T1)
 * 键: wlr_seat_keyboard_notify_key (先 wlr_seat_keyboard_notify_enter)
 * 指: wlr_seat_pointer_notify_enter/motion/button */
void display_input_inject_key(uint32_t keycode, bool press);
void display_input_inject_motion(float nx, float ny, int phase); /* phase: 0=enter 1=motion 2=leave */
```
键盘焦点：第一次注入前 `wlr_seat_keyboard_notify_enter(xs->surface, keys, ...)`；指针同理 enter 到命中窗口的 surface。命中测试用 M0 的 `g_client`（单窗口够 M1-T1）。

- [ ] **Step 4: 测试件——xclient_child 加键盘回显**

`XSelectInput(dpy, win, ExposureMask | KeyPressMask | KeyReleaseMask)`；KeyPress 时 `XLookupString` 转字符 log 到 hilog（标签 XCLIENT-NCP，`%{public}c`）。

- [ ] **Step 5: 真机门**

Run: smoke 触发 displayroute（M0 协议）后：
```bash
hdc -t 192.168.1.6:33363 shell "grep -a 'XCLIENT-NCP' /data/local/tmp/dr_t8.log | grep -a key"
```
Expected: 注入按键后出现 `key 'a' press` 类回显；`no seat assigned` 告警消失。
截屏一张确认窗口仍在（输入链不得破坏 M0 出图）。

- [ ] **Step 6: Commit**

`feat(display): M1-T1 displayroute 输入链——wlr_seat + OHOS 注入桥`

### Task 2: 多窗口与窗口语义

**Files:**
- Modify: `entry/src/main/cpp/display/display_input.c`（命中测试按窗口几何）
- Modify: `entry/src/main/cpp/display/ohos_output.c`（g_client 单例 → surface 列表 + blit 全部映射面）
- Modify: `entry/src/main/cpp/display/ncp/xclient_child.cpp`（第二窗口/移动测试命令）

**Interfaces:**
- Consumes: T1 的注入桥与 seat
- Produces: 多窗口 blit（每帧遍历已映射 surface，按 xs->x/y/width/height 摆位）；键盘焦点 = 最后点按窗口

- [ ] **Step 1: ohos_output 多 surface 跟踪**

`g_client` 单例改为 `wl_list`（`struct ohos_client_surface` 挂链）。`BlitClientSurface` → `BlitAllClientSurfaces`：按 `xs->x, xs->y`（X 坐标即帧内像素坐标，等比无缩放）+ width/height 裁剪拷贝；键盘焦点注入入口改查链表命中。

- [ ] **Step 2: 测试件——第二窗口 + 移动**

xclient 增加环境开关（entryParams 追加字段，非破坏）：`mode=2` 时开两个窗口（不同图案色）并周期 `XMoveWindow`。XSelectInput 加 `StructureNotifyMask`，ConfigureNotify 打日志（xs 位置随动验证）。

- [ ] **Step 3: 真机门**

Expected: 截屏两窗口同屏、位置随 MoveWindow 变化、点第二窗口后按键回显来自第二窗口（hilog 窗口 id 区分）、第一窗口不再收键。

- [ ] **Step 4: Commit**

`feat(display): M1-T2 多窗口语义——surface 列表 + 命中测试 + XWM 配置随动`

### Task 3: scene 化合成 + 帧率

**Files:**
- Modify: `entry/src/main/cpp/display/ohos_output.c`（wlr_scene 替换手搓 blit）
- Modify: `entry/src/main/cpp/display/display_compositor.cpp`（chain 传 scene 相关对象）

**Interfaces:**
- Consumes: wlroots `wlr_scene_create / wlr_scene_output_create / wlr_scene_output_commit / wlr_scene_surface_create`（0.20 头 `types/wlr_scene.h:360,418,586,624`）
- Produces: commit 链不变（scene_output_commit → output commit → HandleOutputCommit → NativeWindow 推帧），下游无感知

- [ ] **Step 1: scene 接入**

chain_start 内：`scene = wlr_scene_create()`、`scene_output = wlr_scene_output_create(scene, output)`；HandleNewSurface 对每个 xs->surface 调 `wlr_scene_surface_create(scene->tree, surface)`（挂 node 指针进 ohos_client_surface）。FrameTick 改：`wlr_scene_output_commit(scene_output, NULL)`；删除 RenderFrame/RenderTestPattern 手动 blit（图案兜底由 scene 的背景 rect 承担：`wlr_scene_rect_create` 画 800x600 底色）。

- [ ] **Step 2: 删除手动应答的验证**

确认 `wlr_surface_send_frame_done` 与 `request_configure` 处理保留（scene 不管 XWM 配置应答，`ClientRequestConfigure` 仍必需）；frame_done 由 scene 的 output frame 路径接管后删除手动调用——**逐项验证**：删 frame_done 手动调用后条纹必须仍在滚动（Review Focus #5）。

- [ ] **Step 3: 帧率测量**

Run: 触发 displayroute + client，取 60s 的 commit seq 时间戳：
```bash
hdc -t 192.168.1.6:33363 shell "grep -a 'commit seq' /data/local/tmp/dr_t8.log | tail -20"
```
Expected: 每 30 帧间隔 ≤1.2s（≥25fps）；M0 基线 ~3.5s。若未达标：定段测量（FrameTick 前后 clock_gettime 打点一次），定位残留在 commit→Flush 侧（BufferQueue），数据回填 ledger——**不在此任务里做零拷贝**（那属于 present 重构，M2）。

- [ ] **Step 4: 真机门 + Commit**

Expected: 双窗口内容正常、条带滚动、commit ≥25fps。`feat(display): M1-T3 scene 化合成——帧率 ≥25fps`

### Task 4: guest 侧 X11 栈 + wine --with-x + 驱动选择分支

**Files:**
- Modify: `scripts/build_xcb_stack.sh` / `build_x11_client.sh`（guest 侧 pass）
- Modify: `scripts/assemble.sh`（x86_64 libX11/libXext/libxcb 装配进 wine-data guest 面）
- Modify: `scripts/build_wine.sh:148`（`--without-x` → `--with-x`，仅 guest pass）
- Modify: `thirdparty/wine/dlls/win32u/driver.c`（经 `scripts/patches/wine-x11-route.patch` 新增）

**Interfaces:**
- Consumes: M0 的按侧参数化（`build_native.sh` 的 GUEST/HOST 侧 pkg-config 分目录惯例，`scripts/build_native.sh:258` 注释）
- Produces: guest `sysroot-ext/usr/lib/x86_64-linux-ohos/`（或既有 guest 库目录）含 `libX11.so.6/libXext.so.6/libxcb.so.1`；`dlls/winex11.drv/winex11.so` 编出；`DISPLAY` 设定时 winex11 直载

- [ ] **Step 1: guest 侧 X11 构建**

xcb 栈/X11 脚本按 M0 的参数化加 guest pass：目标 `x86_64-linux-ohos`，产物装 guest 库目录（照 wayland 1.26 的 guest 装法）。Run: 脚本后 `file build/sysroot-ext/**/libX11.so.6.4.0` 断言 `x86-64`。

- [ ] **Step 2: wine --with-x + winex11 编出**

`build_wine.sh` 的 OHOS pass（:148 `--without-x`）改 `--with-x`，CFLAGS/LDFLAGS 补 guest X11 头库路径。Run: `make NATIVE_ARCH=arm64-v8a` 后断言 `thirdparty/wine-build*/dlls/winex11.drv/winex11.so` 存在且 x86-64。依赖缺失（xrender/xcursor 等可选扩展）逐个 `--without-<f>` 收窄——configure 输出为准，不预设。

- [ ] **Step 3: 驱动选择分支**

`win32u/driver.c` 的 `__OHOS__` bypass（:1021 附近）加 DISPLAY 分支：
```c
else if (getenv("DISPLAY"))
{
    /* X 路线 (displayroute): DISPLAY 已设即走 winex11。
     * 摆位在 WAYLAND_DISPLAY 之后 = 双设时 wayland 赢 (保守默认,
     * Review Focus #1); 路线选择靠 env 注入互斥 (X 档位只设 DISPLAY
     * 不设 WAYLAND_DISPLAY), 不靠分支顺序。
     * 注: 既有代码约定 KeUserModeCallback 返回 0 = 加载成功 (见上方
     * winewayland 分支的 ! 判断), 新分支照抄同款判断。 */
    ... 同款 KeUserModeCallback(NtUserLoadDriver, winex11W, sizeof(winex11W), &ret_ptr, &ret_len)
}
```
打成 `scripts/patches/wine-x11-route.patch`（submodule 净树纪律）。
Expected（Review Focus #1）: 不设 DISPLAY 跑 core 套件 PASS（旧链路零变化）；设 DISPLAY 时 hilog 见 winex11 加载。

- [ ] **Step 4: wine 冒烟——xeyes 类最小程序**

xclient 改造不做；直接用 wine 自带 `notepad` 太远，先跑任意极小 X 程序验证 winex11→Xwayland 连通（若无现成二进制，用 T9 的 xclient 思路在 guest 编一个 20 行 XOpenDisplay 探针 exe，随 wine-data 带）。Expected: hilog 显示 winex11 连接成功、窗口创建。

- [ ] **Step 5: Commit**

`feat(wine): M1-T4 winex11 交叉接入——guest X11 栈 + 驱动选择 DISPLAY 分支`

### Task 5: notepad 端到端（M1 出口判据）

**Files:**
- Modify: `entry/src/main/cpp/wine/wine_env.cpp`（X 档位环境注入：DISPLAY/XAUTHORITY 不需要（-ac），WINEDLLOVERRIDES 不动）
- Modify: smoke 触发面（displayroute job 扩展：notepad 启动参数）

**Interfaces:**
- Consumes: T1 输入注入、T2 多窗口、T4 winex11
- Produces: M1 出口证据

- [ ] **Step 1: notepad 拉起**

smoke job displayroute 扩展 `x11app: "notepad"` 字段：displayroute ready 后以 guest 方式 spawn `notepad.exe`（复用 wine 启动器，env 注入 `DISPLAY=:0`）。Expected: notepad 窗口经 winex11→Xwayland→合成上屏（截图）。

- [ ] **Step 2: 输入验收**

注入点击菜单 + 键盘输入文字。Expected: 截图显示 notepad 文本区出现注入的字符串；点击命中与视觉位置一致（Review Focus #2）。

- [ ] **Step 3: Commit**

`feat(display): M1-T5 notepad 端到端——winex11 出口判据达成`

### Task 6: GLX-over-EGL 桥最小原型裁决（R1 spike）

**Files:**
- Create: `smoke/programs/glx_bridge_probe.c`（guest 探针，随 wine-data）
- Modify: `docs/superpowers/specs/...-design.md`（裁决数据回填 §5 R1）

**Interfaces:**
- Consumes: guest mesa virpipe（现行 GL 后端）、T4 的 X 连接
- Produces: go/no-go 裁决数据（surfaceless/pbuffer 可用性 + readback 带宽实测）

- [ ] **Step 1: 探针程序**

guest C 程序：`eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA)` 或 pbuffer 路径 → `glReadPixels` 1MB → 计时；`XShmPutImage` 到 X 窗口计时。合成一张「帧生成+回传」耗时表。编译进 guest 构建面（复用 smoke 的 guest 程序构建惯例 `smoke/programs`）。

- [ ] **Step 2: 实测 + 裁决**

Run: 设备跑探针（走 wine 启动器 env）。Expected: 出数据表（成败 + ms 级分段）；结论二选一回填 spec §5 R1：A) 带宽可接受 → M2 按①做 GLX-over-EGL 桥；B) 不可接受 → R1 走② DXVK 收窄面。**本任务不做 wine opengl.c 改造**——只出裁决。

- [ ] **Step 3: Commit**

`feat(smoke): M1-T6 GLX-over-EGL 探针——R1 裁决数据`

### Task 7: R-WSI 探针

**Files:**
- Create: `smoke/programs/wsi_xcb_probe.c`（guest，枚举 VkInstance 扩展）

- [ ] **Step 1: 探针 + 回填**

`vkEnumerateInstanceExtensionProperties` 查 `VK_KHR_xcb_surface`；结果（有/无 + 版本）回填 spec §5 R-WSI 行。Expected: 一行结论。`feat(smoke): M1-T7 R-WSI 探针`

### Task 8: M1 收尾

- [ ] **Step 1: 回归**——`core` + `wine-vulkan` 套件 PASS（DISPLAY 未设场景，Review Focus #1）
- [ ] **Step 2: spec 回填**——M1 实测结论节（§6.3）：帧率、输入链、winex11 接入形态、GLX 裁决、R-WSI 结果
- [ ] **Step 3: M1 验收报告**——过/未过项 + M2 入口建议

## Self-Review 结论

- 覆盖 spec §7 M1 行全部三要素（winex11 接入/窗口语义+输入/GLX 裁决）+ R-WSI ✅
- 无 TBD/placeholder：guest X11 构建细节依赖既有参数化脚本（给出验证命令而非虚构路径）✅
- 接口一致性：display_input 注入函数签名在 T1 定义、T2 消费一致 ✅
- Review Focus 五条各自挂到了 T4(1)/T5(2)/T2(3)/T4(4)/T3(5) ✅
