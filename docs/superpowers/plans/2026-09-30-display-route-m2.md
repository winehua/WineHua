# Display-Route M2 实现计划——呈现链零拷贝 + Venus 重锚 + IME + PC 窗口形态

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** X 路线跑通真实 D3D 负载（dxvk 套件出图正常）且合成性能达 ≥25fps，IME 与 PC 双窗口形态落地。

**Architecture:** 三阶段。阶段 A（关键路径）：先探针后改造——host EGL OHOS 导入探针裁决零拷贝路线（R-ZC ②），drisw 可见窗 present 验证收口 GL 策略（M1-T6 改判），然后 wlroots gles2 OHOS 导入分支 + Venus SURFACE_ID 重锚（wl_surface id → X window id）双线并进，dxvk 套件 X 路线出图为出口。阶段 B：IME 三候选裁决后落地。阶段 C：PC 模式 output↔OHOS 系统窗绑定。全程 wine/venus 渲染核心零改动（M1 已证：窗口系统集成层与渲染核心分离，virpipe/venus/dxvk 不知道窗口系统存在）。

**Tech Stack:** wlroots 0.20.2（补丁携带）、xserver xwayland-24.1.13（补丁携带）、wine fork（feature/display-route-m0）、mesa virpipe/venus（构建配置定制，无代码补丁）、virglrenderer fork（vtest 私有命令）、DXVK 1.10.3/2.6.2 + vkd3d limited-500k、host EGL/GLES（app 进程内，Maleoon）。

**Spec:** `docs/superpowers/specs/2026-09-27-display-route-x11-wlroots-design.md`（§4.2 Venus 重锚 / §4.4 窗口管理 / §5 R-ZC、R-IME、R1 / §6.3 M1 实测结论 / §7 里程碑）。M1 遗留：`docs/engineering/display-route-known-issues.md`（§1 清账是本计划 T1）。M1 验收：`docs/superpowers/specs/2026-09-29-display-route-m1-acceptance.md`。

## Global Constraints

- 分支：`feature/display-route-m0` 同分支续推（M0/M1 既定裁定），不合 master。wine submodule 提交先推自己远程再动主仓指针；wlroots/xserver 钉版不变、零本地提交、改动全走 `scripts/patches/*.patch`（补丁签名守卫生效）。
- guest/host 侧别纪律（build-and-log.md）：组件先答"跑在 guest 还是 host"；host 交叉依赖进 `build/host-ext/<NATIVE_ARCH>/`，guest 产物进 `build/sysroot-ext/`，不跨侧链接。
- 设备端只跑不判：探针报能力缺席 = SKIP/合法答案；判定只读归档。
- 套件回归前 force-stop + 冷启动（known-issues §3.2）；`hdc -b` 热更二进制后 sha256 对账（§3.1）。
- `glx-bridge-probe` 不得用作回归门（§3.3）；新门禁探针自带阈值判定。
- 改 wine 源码 → 完整构建（wine-data 重打包）；`WINEHUA_WINEDEBUG` 现在可随 `--env` 下发（M1-T5 已修）。
- 文档只写当前有效状态：每个裁决回填 spec 对应行，不写尝试史。

## Review Focus

1. **重锚后的 id 歧义**：X window id 复用（窗口销毁后 id 被 X 复用）会让 virgl 侧帧投错窗——wine 侧 surface 生命周期与 X window 销毁的竞态必须有销毁即失效的映射删除（期望：窗口销毁后同 id 旧帧被丢弃而不是投进新窗）。
2. **零拷贝切换后的格式/stride 面**：gles2 导入分支对 OH_NativeBuffer 格式（ABGR/XBGR8888 等 M0 已背书的映射）与 stride 的假设，不得只在 800×600 固定输出下成立。
3. **双路线回归**：每个触碰共享代码（win32u、venus、virgl 传输）的任务，DISPLAY 未设的 wayland 路线必须不回退（core 套件 + 至少一个 dxvk 套件 keep-green）。
4. **IME 注入路径的输入正确性**：无论三选哪条，中文输入的 preedit/commit 必须落到正确焦点窗口，模态对话框场景不串窗（M1-T5 的几何无关聚焦经验直接复用）。
5. **PC 模式的生命周期异步**：OHOS 系统窗创建/销毁与 X toplevel map/unmap 异步，销毁竞态不得崩溃（期望：系统窗先死时合成侧 surface 安全清理）。

---

## 阶段 A：呈现链（关键路径）

### Task 1: known-issues §1 清账——输出路径必修三处 + 合成器启动收尾

**Files:**
- Modify: `entry/src/main/cpp/display/ohos_output.c`（munmap 长度、stride 丢帧日志、Map 失败归还 buffer）
- Modify: `entry/src/main/cpp/display/display_compositor.cpp`（启动失败统一 cleanup）

**Interfaces:**
- Consumes: 无（独立清账）
- Produces: `HandleOutputCommit` 无静默失败路径（每条失败分支有日志/计数）；启动链失败不泄漏

- [ ] **Step 1: munmap 复用 mmap 的 bytes 变量**（known-issues §1.1）——RED 依据 = 代码审查实锤（mmap 用 `h->size`、munmap 用 `ht*dst_stride`）。修后两值恒同源。
- [ ] **Step 2: stride 不一致分支加限频日志 + 计数**（§1.1）。Expected: 构造 stride 分叉（临时改 dst_rows 钳制）时 hilog 出现 `stride mismatch` 且帧不丢帧计数递增。
- [ ] **Step 3: `OH_NativeBuffer_Map` 失败分支归还 window buffer**（`FlushBuffer` 或取消路径）。
- [ ] **Step 4: 启动线程体统一 cleanup（goto cleanup 风格），retrigger 管道赋值收进锁**（§1.2），顺带 §1 杂项（`#pragma once` 重复、appPid 未消费、write 返回值日志化）。
- [ ] **Step 5: 回归**——冷启动 core 套件 4/4 + displayroute job 重放行为面不回退。
- [ ] **Step 6: Commit** `fix(display): M2-T1 输出路径清账——munmap 配对/丢帧可见性/失败归还/启动收尾`

### Task 2: R-ZC ② 探针——host EGL `EGL_NATIVE_BUFFER_OHOS` 导入可用性

**Files:**
- Create: `entry/src/main/cpp/display/ohos_egl_import_probe.c`（host 侧自检，合成器 bring-up 内嵌）
- Modify: `entry/src/main/cpp/display/display_compositor.cpp`（探针挂载点：env `WINEHUA_EGL_IMPORT_PROBE=1` 时在 renderer 初始化前跑）
- Modify: `docs/superpowers/specs/...-design.md` §5 R-ZC 行（裁决回填）

**Interfaces:**
- Consumes: host EGL/GLES（app 进程已在用，旧合成器 egl_renderer 同源）；`OH_NativeBuffer` 分配（M0 路径）
- Produces: 裁决结论（可用/不可用 + 失败环节），写入 hilog + `displayroute-egl-import-probe` 标记文件（内容 = 结论枚举），供 smoke job 归档判定

- [ ] **Step 1: 探针实现**——分配一个小 OH_NativeBuffer（RGBx 8888，64×64）→ host `eglCreateImageKHR(ctx, EGL_NATIVE_BUFFER_OHOS, native_buffer)` → `glEGLImageTargetTexture2DOES` → FBO attach → `glReadPixels` 回读校验像素一致 → 销毁。每步结果落 hilog；任何一步失败 = 记录失败环节枚举。RED 依据：该组合 spec 明注"API 存在已证、组合未验证"。
- [ ] **Step 2: 真机跑探针**。Expected: 标记文件产生，结论二选一：(a) 全链通过 → T4 按零拷贝做；(b) 失败 → T4 降级为 spec 预案（CPU 合成上限，R-ZC ① 形态），失败环节回填 spec §5 R-ZC。
- [ ] **Step 3: smoke 判定接入**——displayroute job 的 check 读标记文件（判定只读归档）。
- [ ] **Step 4: Commit** `feat(display): M2-T2 host EGL OHOS 导入探针——零拷贝路线裁决`

### Task 3: drisw 可见窗 present 链验证（M1-T6 改判收口）

**Files:**
- Create: `smoke/tests/dx-glx-present/test.json` + 探针扩展（wine fork `programs/winehua_glx_bridge_probe/main.c` 加 visible-window swapbuffers 模式，或新 program `winehua_glx_present_probe`——实现时按侵入最小选）
- Modify: `docs/superpowers/specs/...-design.md` §5 R1 行（present 链结论回填）

**Interfaces:**
- Consumes: M1-T4 的 X 路线（`WINEHUA_DISPLAY_ROUTE=x11`）、winex11 GLX/drisw、Xwayland→scene→NativeWindow 投递
- Produces: 结论行——X 路线 OpenGL 程序**可见窗 SwapBuffers** 出图正确/不正确（帧内容经 drisw→XPutImage→Xwayland 全链）

- [ ] **Step 1: 探针扩展**——可见窗 + 双缓冲 SwapBuffers 循环（旋转色块，帧间内容可判变），`--result` JSON 帧计数 + 内容 CRC 序列；照 `dx-notepad` 惯例 from_wine 入 displayroute 套件。
- [ ] **Step 2: 真机 X 路线跑**。Expected: 归档帧序列内容可判变化且无冻结（判定：CRC 序列单调变化）；同时记录帧率（drisw present 的真实 fps——零拷贝对照基线的 GL 分量）。
- [ ] **Step 3: 结论回填 spec R1**（GL 完整走 X 路线是否成立；不成立时失败环节定位）。
- [ ] **Step 4: Commit** `feat(smoke): M2-T3 drisw 可见窗 present 验证——GL 策略收口`

### Task 4: 零拷贝落地——wlroots gles2 渲染器 + OHOS 导入分支（依赖 T2 结论 a）

**Files:**
- Modify: `scripts/patches/wlroots-ohos-ncp-spawn.patch`（或新增 `wlroots-ohos-gles2-import.patch`——实现时按补丁签名守卫的粒度惯例定）
- Modify: `scripts/build_display_libs.sh`（host EGL/GLES 依赖进 host-ext，若未覆盖）
- Modify: `entry/src/main/cpp/display/display_compositor.cpp`（renderer 选择：探针通过 → gles2，否则回退 pixman）
- Modify: `entry/src/main/cpp/display/ohos_output.c`（gles2 路径的帧输出：BufferQueue 生产者直连 EGL surface，替代 mmap copy）

**Interfaces:**
- Consumes: T2 结论（导入可用）；M0 的 OH_NativeBuffer wlr_buffer/allocator 背书（格式映射 ABGR/XBGR8888→RGBA_8888 等）
- Produces: `wl_ohos_output_chain_start` 在 gles2 模式下的帧路径 = GPU 直通（无 mmap/memcpy）；`WINEHUA_FRAME_TRANSPORT` 语义扩展值（如 `gles2+buffer_queue`）

- [ ] **Step 1: wlroots gles2 可用性打通**——host-ext 增加 EGL/GLES 依赖，`wlr_renderer_autocreate` 路径或显式 `wlr_gles2_renderer_create`；补丁内注释写明 OHOS 导入分支的实测依据（防被当错误经验修正，原则 15）。
- [ ] **Step 2: OHOS 导入分支**——`eglCreateImageKHR(EGL_NATIVE_BUFFER_OHOS)` 包装 wlr_buffer 的 texture 创建；**格式/stride 断言写成机械检查**（Review Focus #2：不得只在 800×600 成立）。
- [ ] **Step 3: 输出路径切换**——gles2 模式下 `HandleOutputCommit` 的 copy 路径退位（保留 pixman 回退分支，env 可强制）。
- [ ] **Step 4: 双路线回归**——displayroute job 重放 + X 路线探针帧率（对照 M1 基线 18.6fps）；wayland 路线 core 不回退（Review Focus #3）。
- [ ] **Step 5: Expected**——分段遥测 copy+flush 段归零/近零，合成 fps ≥25（spec §6.3 残留清偿）；不达标时数据回填 + 残留定位，不硬凑。
- [ ] **Step 6: Commit** `feat(display): M2-T4 gles2 零拷贝——OHOS 导入分支 + GPU 直通输出`

### Task 5: Venus SURFACE_ID 重锚——X window id（与 T4 并行）

**Files:**
- Modify: wine fork `dlls/winex11.drv/vulkan.c`（WINEHUA 私有面分支：镜像 `winewayland.drv/vulkan.c` 174 行形态，id = X window id）
- Modify: `entry/src/main/cpp/graphics/graphics_broker.cpp`（virgl_child 宿主回调：按 X window id 路由）
- Modify: `entry/src/main/cpp/display/`（window_id → wlr_xwayland_surface → wl_surface 映射表 + 帧注入 scene）
- Modify: `docs/superpowers/specs/...-design.md` §4.2（落地形态回填）

**Interfaces:**
- Consumes: 私有 present 协议（tag `0x574853`，vtest 命令不变——"链路其余不动"）；`wlr_xwayland_surface` 的 `window_id + surface` 双字段；T1 清账后的输出路径
- Produces: `vkCreateWin32SurfaceKHR`（Windows 侧）在 X 路线 wine 进程内产出私有 surface（id=X window）；合成器侧映射 API `display_route_surface_for_xwindow(xid)`；**销毁即失效语义**（Review Focus #1：映射表随 `wlr_xwayland_surface` destroy 事件删除，迟到的旧 id 帧丢弃并计数）

- [ ] **Step 1: wine 侧**——`winex11.drv/vulkan.c` 加 WINEHUA 分支：Win32 HWND → winex11 的 X window 映射（驱动内已有）→ 私有 surface（`0x574853 | window_id`）。vkDestroy/查询系列镜像 winewayland 实现。RED 依据：现状 X 路线 Vulkan present 无路径（M1-T7 判否实锤）。
- [ ] **Step 2: 合成器侧映射表**——new_surface/destroy 事件维护 `window_id → scene surface`，destroy 删除 + 旧 id 帧丢弃计数（hilog 限频）。
- [ ] **Step 3: virgl_child 宿主回调路由**——按 id 查映射表投帧；无映射 = 丢弃 + 计数（不崩溃、不阻塞渲染线程）。
- [ ] **Step 4: 单元级真机验证**——winex11 路线跑 `winehua_vulkan_smoke`（offscreen 不够，要 present 模式）：归档帧出图 + 销毁竞态专项（快速开销窗口循环，验证丢弃计数工作、无崩溃）。
- [ ] **Step 5: 双路线回归**（Review Focus #3）——wayland 路线 dxvk 套件 keep-green（`WINEDLLOVERRIDES`/驱动选择未动）。
- [ ] **Step 6: Commit**（wine fork + 主仓两笔，指针成对）`feat(wine): M2-T5 X 路线 Vulkan 私有面——SURFACE_ID 重锚 X window id` / `feat(display): M2-T5 重锚接收侧——window_id 映射与销毁失效`

### Task 6: 出口判据——dxvk 套件 X 路线出图（依赖 T4+T5）

**Files:**
- Create: `smoke/suites/displayroute-dxvk.json`（dxvk 矩阵的 X 路线镜像：`env` 钉 `WINEHUA_DISPLAY_ROUTE=x11` + `DISPLAY=:0` + displayroute 前置）
- Modify: `docs/superpowers/specs/...-design.md` §7 M2 行（出口状态回填）

**Interfaces:**
- Consumes: T4（零拷贝输出）+ T5（重锚）；dxvk 套件既有用例定义（档位钉死约束不变：声明 d3d 必声明 dxvk）
- Produces: X 路线 dxvk 判定 PASS 集 + 与 wayland 路线同套件的性能对照数据（§8.4 观察项起点）

- [ ] **Step 1: 套件定义**——从 `dxvk.json` 镜像用例清单，env 加 X 路线三键；判定沿用既有 checks（不新造判据）。
- [ ] **Step 2: 真机跑通**。Expected: dxvk 基线矩阵 X 路线 PASS；失败按链路定位（重锚/零拷贝/驱动选择），修到绿——这是出口判据本体。
- [ ] **Step 3: 对照数据归档**——X 路线 vs wayland 路线同用例帧率/耗时表，进 spec §8.4 观察项。
- [ ] **Step 4: Commit** `feat(smoke): M2-T6 dxvk X 路线套件——出口判据达成`

## 阶段 B：IME（裁决型）

### Task 7: IME 三候选裁决 + 落地

**Files:**
- Create: `docs/decisions/0006-ime-route.md`（三选一决策记录，按 decisions/ 惯例）
- Modify: 按裁决结果定（下游补丁移植 / 自写 XWM↔text-input 桥进 wlroots 补丁 / OHOS 应用层注入）
- Modify: `docs/superpowers/specs/...-design.md` §5 R-IME 行（裁决回填）

**Interfaces:**
- Consumes: `wlr_xwayland_surface` 事件群（§4.4）、M1 的几何无关焦点（T5 经验）、OHOS IME Kit（应用层）
- Produces: 中文输入在 X 路线 notepad 可用（preedit + commit 落正确焦点窗；模态对话框不串窗——Review Focus #4）

- [ ] **Step 1: 调查周**——三候选各出可行性事实：① 下游补丁源（搜索 mutter/kwin/wlroots 邻居树的 Xwayland IME 桥实现，references 文档已注明 mutter 无）② 自写桥的工作量面（text-input-v3 ↔ XIM/CTT 协议翻译）③ OHOS 应用层注入（现有注入桥扩展，无新协议）。产出对照表进决策记录。
- [ ] **Step 2: 裁决 + 回填 spec R-IME**（Ruling 格式：选谁、为什么、代价）。
- [ ] **Step 3: 落地 + 验收**——notepad 中文输入真机用例（preedit 显示、上屏、焦点切换后 IME 跟随；模态对话框场景）。
- [ ] **Step 4: Commit** `feat(display): M2-T7 IME——<按裁决定>`

## 阶段 C：窗口形态

### Task 8: PC 模式双窗口——output↔OHOS 系统窗绑定

**Files:**
- Modify: `entry/src/main/cpp/display/ohos_output.c`（多 output：每 X toplevel 一个 headless output ↔ NativeWindow）
- Modify: `entry/src/main/cpp/display/display_compositor.cpp`（系统窗生命周期绑定：XComponent surface 按窗申请）
- Modify: `entry/src/main/ets/`（PC 模式系统窗管理——spec §4.4"现有 ArkTS service 平移"）
- Modify: `smoke/tests/`（PC 多窗 + 菜单 tooltip + 模态对话框验收用例，spec §4.4 点名）

**Interfaces:**
- Consumes: `wlr_headless_add_output` / `wlr_output_destroy`（§4.4 实证条目）、M1 的 surface 列表/命中测试/焦点
- Produces: PC 模式下每个 X 窗口独立 OHOS 系统窗呈现；销毁竞态安全（Review Focus #5：系统窗先死 → 合成侧安全清理，真机专项）

- [ ] **Step 1: 多 output 骨架**——toplevel map → add_output + NativeWindow 绑定；unmap → destroy。销毁竞态专项用例先行（TDD：先写竞态用例看它崩/泄漏，再修）。
- [ ] **Step 2: ArkTS 系统窗管理平移**——按 spec §4.4 等价物迁移，异步时序处理。
- [ ] **Step 3: 验收用例**——PC 多窗、菜单 tooltip、模态对话框三件（spec 点名），归档帧判定。
- [ ] **Step 4: Commit** `feat(display): M2-T8 PC 双窗口形态——output↔系统窗绑定`

### Task 9: M2 收尾

- [ ] **Step 1: 全量回归**——core / wine-vulkan / dxvk（wayland 路线不回退）+ displayroute / displayroute-dxvk（X 路线）+ 已知问题协议过一遍（known-issues 修一条删一条）。
- [ ] **Step 2: spec 回填**——§7 M2 行出口状态、§8.4 对照数据。
- [ ] **Step 3: M2 验收报告**——`docs/superpowers/specs/2026-09-30-display-route-m2-acceptance.md`（过/未过 + 挂起 + M3 入口：旧合成器退役清单）。
- [ ] **Step 4: Commit** `feat(display): M2-T9 收尾——回归 + spec 回填 + 验收报告`

## Self-Review

- spec 覆盖：§4.2（T5）、§4.4（T8）、§5 R-ZC ②（T2/T4）、R1 收口（T3）、R-IME（T7）、§7 M2 全三件 + 出口（T6/T9）✅
- M1 遗留承接：known-issues §1（T1）、§3 协议（全局约束）、drisw 收口（T3）✅
- 接口一致性：重锚两端（wine 私有面 id / 合成器映射表）在 T5 Interfaces 成对声明 ✅
- Review Focus 五条 → T5(1)/T4(2)/T4-T5-T6(3)/T7(4)/T8(5) ✅
- 占位符检查：T7 落地形态按裁决定（spec 明文 M2 裁决项，计划结构合法）；其余任务 Files/Steps 具体 ✅
