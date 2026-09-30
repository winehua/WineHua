# X 路线 OpenGL 呈现桥 + 帧时钟 VSync 驱动 设计（2026-09-30）

> 目标：让 OpenGL（wgl）程序在 X 路线**能出图且不被钳在 30fps**。
> 上游依据：`2026-09-27-display-route-x11-wlroots-design.md` §2.4（X 路线 GL 呈现链三处缺件）、
> §6.4；本次实测与被推翻的旧结论见下「证据」一节。
> 范围：本 spec 只做**两件事**——GL 呈现桥、帧时钟 VSync 驱动。窗口形态（阶段 C）、
> IME（阶段 B）、裁剪/补栈 GLX 不在其中。

## 1. 证据（现状与已推翻的旧结论）

### 1.1 X 路线 GL：渲染通、呈现无出口（已量化）

- T3（r20260930-014632）已定位：WGL → win32u 通用 EGL 驱动（
  `egldrv_init_egl_platform` = `EGL_PLATFORM_SURFACELESS_MESA`）→ FBO drawable，
  `framebuffer_surface_swap()` 是**空实现**（`dlls/win32u/opengl.c:407` 直接 `return TRUE`）。
- 2026-09-30 复测（`smoke/jobs/displayroute-gl-baseline.json`，设备 .5）：
  `winehua_graphics_smoke` 在 X 路线 **frames=3028 / producerFps=447**，
  `displayFps=-1.0`，程序自报 `no compositor display sequence observed`。
  同程序 wayland 路线 790 帧 / 114fps / PASS。
  ⇒ **渲染完全没被拖住，差的只是呈现出口**；该读数即 GL 工作的 RED 基线。

### 1.2 零拷贝 GL 呈现的现成机制（wayland 路线）

`winewayland.drv/opengl_readback.c` 的 present 分支（`winehua_readback_present`）：

```
surface_id = wl_surface id
if (surface_id && winehua_surface_zero_copy_ready(surface_id)) {   /* 宿主已挂目标 */
    winehua_begin_present_surface(surface_id);   /* 写 shm 页: {magic,version,surface_id} */
    glFlush(); eglSwapBuffers();                 /* 真正的 present */
    winehua_finish_present_surface();
}
```

- shm 页 `winehua_present_surface_<pid>.shm` 由 **guest mesa 的 vtest winsys** 读取
  （`mesa/.../virgl/vtest/virgl_vtest_socket.c:785` `winehua_vtest_get_present_surface_id`），
  随 vtest 协议把「present 目标 id」送到宿主。
- 宿主入口：`virgl_child.cpp:351 OnVtestPresent(texId, …, clientPid, surfaceId, …)`
  → `virgl_surface_presenter.cpp:816 PresentVirglSurface(pid, surfaceId, …)`
  → `g_presenters`（`SurfaceQueuePresenterManager`）——**与 X 路线 Vulkan 帧的落点是同一张表**。
- 就绪握手：宿主挂好目标写 `winehua_zc_surface_<key>.ready`（`graphics_broker.cpp:82-98`，
  key = `(pid<<32)|surface_id`），并把 `WINEHUA_ZERO_COPY_READY_DIR` 传进 guest 环境。
- 实测（2026-09-30，设备 .5，`WINEHUA_OPENGL_DIAG=1`）：wayland 路线 GL 呈现
  **本来就是零拷贝**（`presents=720 readbacks≈7`）⇒ 读回只是回退路径，不是生产形态。
  （`WINEHUA_WAYLAND_READBACK=1` 开关不改变该比例：零拷贝目标可用时恒走零拷贝。）

### 1.3 帧时钟：X 路线自建 30Hz，wayland 走系统 VSync

| | 节奏来源 | 上限 | 实测 |
|---|---|---|---|
| wayland | `OH_NativeVSync_RequestFrame`，期望区间 `{60,120,120}`，100ms 超时 + 16.67ms 兜底（`graphics/egl_renderer.cpp:635-670`） | 屏幕刷新率 × 客户端出帧 | 90~116fps |
| X 路线 | `wl_event_source_timer_update(g_out.frame_timer, 33)`（`display/ohos_output.c:650`）；`display/` 内无任何 `NativeVSync` | ~30fps | 25~28fps |

同源证据：同名 cube 用例 wayland 903 帧 vs X 路线 221 帧（M2 验收已记）。

### 1.4 与本 spec 相关的既有约束

- known-issues §2.6（不变量）：present 路径每帧 `glFinish` 是 guest 帧槽位归还安全性的
  依据。**提高帧率会直接提高它的预算压力**（见 §4 风险）。
- 判据欠账（原 §2.4）：`dx-glx-present` 的 `displayed` 门读 `WINEHUA_DISPLAY_FPS_FILE`
  （wayland 渲染器写的文件，X 路线永远缺）；`visual:rgba-quadrants` 判整屏截图，
  而 X 路线出图面是侧栏预览小框（约 500×390 物理像素）。

## 2. 任务 1：GL 呈现桥（X 路线）

### 2.1 做法

**不引入 GLX、不引入 EGL-on-X11**：guest 侧继续用**已经跑通的 surfaceless EGL**
（`egldrv` 现状，T3 已证渲染正常），只把**呈现出口**换成 §1.2 那条 id 通道，
id 取 **X window id**（route 内唯一、与 Vulkan 私有面同一把钥匙）。

1. **驱动挂接（guest）**：在 `dlls/winex11.drv` 增 GL 驱动实现（对称于同目录既有的
   `vulkan.c` 私有面实现），提供 `OpenGLInit` 的 driver funcs；`WINEHUA_DISPLAY_ROUTE=x11`
   且 GLX 缺席时启用。surface 创建/make_current 复用 surfaceless EGL + FBO 的既有形态。
2. **呈现序列（swap）**：
   ```
   id = X window id of HWND            /* winex11 内已有 hwnd→xwindow 映射 */
   if (id && winehua_surface_zero_copy_ready(id)) {
       publish(id); glFlush(); eglSwapBuffers(); unpublish();
   } else {
       回退：现状（空转）或既有 readback 路径，并限流打一条日志（不得静默）
   }
   ```
   `publish/ready` 两个函数从 `winewayland.drv/opengl_readback.c` 提取为**共享实现**
   （放 `dlls/win32u/` 或 `wine/` 下的公共 TU），避免第二份拷贝。
3. **宿主侧**：**预期不动**——`PresentVirglSurface(pid, surfaceId)` 已按同一张表找目标；
   `display_guest_frames` 已把该目标的帧挂到 X 窗口的合成器节点。若实测发现
   presenter 对 X 路线的目标解析有差异（如 pid 归属），只补查找逻辑，不改契约。
4. **降级可见性**：id 缺失 / 目标未就绪 / swap 失败 ⇒ 每条都有限流日志（沿用
   `winehua_wayland_diag` 的形态），不得静默空转。

### 2.2 判据（验收）

- **主判据**：`winehua_graphics_smoke` 在 X 路线 `displayFps > 0` 且四象限可见
  （当前 RED：`displayFps=-1`、`no compositor display sequence observed`）。
- **判据改造（本任务内）**：程序侧改成输出**帧内容 CRC 序列**（证明客户端在画 + 内容在变），
  出图与否由主机侧判；`visual:rgba-quadrants` 改为按区域裁剪（或按框内 CRC 变化判活），
  不再假设"整屏四象限"。改造后 `dx-glx-present` / `opengl-*` 在两条路线用同一套判据。

## 3. 任务 2：帧时钟改 VSync 驱动

### 3.1 做法

1. `display/` 增 `OH_NativeVSync` 实例（期望区间 `{60,120,120}`，与 wayland 渲染器同口径），
   `RequestFrame` 回调里写一个 **eventfd**；把该 eventfd 用 `wl_event_loop_add_fd` 挂进
   合成器 event loop，回调只做"唤醒 + 置脏"，**实际渲染仍在 event loop 线程**（wlroots 不是线程安全的）。
2. `FrameTick` 的调用来源从 33ms 定时器切到该唤醒；**damage 门控保留**（无 damage 不渲染）。
3. **回退**：`OH_NativeVSync` 创建失败 ⇒ 保留现有 33ms 定时器（行为与本变更前一致）。
4. 分段遥测（`segment scene render+commit` / `rate …`）保留，作为前后对照。

### 3.2 判据（验收）

- X 路线 cube / GL 的 `outCommits` 提升到屏幕刷新率量级（≥60/s，目标 90~120/s），
  且 `rate` 行 `ticks` 与 VSync 一致；
- 无 damage 时 `needsFrame=0`（门控仍生效，不能变成 120Hz 空转）；
- 帧时间预算：`segment scene render+commit` + present 段在 8.3ms（120Hz）内；
  超出则按 §4 处理。

## 4. 风险与已知耦合

1. **每帧 glFinish 的预算**（known-issues §2.6 不变量）：帧率越高，这个全同步越贵。
   120Hz 预算 8.3ms/帧，而今天实测 scene 3.2ms + present 1.6ms + glFinish 未单独计时。
   **本 spec 要求先测**（present 分段已经有仪器）；若超预算，**接 §2.6 已记的升级项**：
   用 native fence 替换 glFinish（`wlr_egl_create_sync` / `wlr_egl_dup_fence_fd` 已在树里），
   并把不变量检查器的判据同步改掉。
2. **120Hz 下的 CPU 竞争**：合成器与 guest（box64 翻译）抢核；若实测掉帧，回退到
   60Hz 期望值（`{60,60,60}`）是可接受的中间态。
3. **guest 侧 EGL 形态**：本方案依赖 surfaceless EGL 在 X 路线可用——T3 已证渲染正常，
   但**未证**其 swap 路径在"有真实 present 目标"时的行为；任务 1 第一步就是拿
   graphics_smoke 打通一条端到端的帧。
4. **判据改造的范围**：两个判据（CRC 序列 / 区域裁剪）会同时影响 wayland 路线既有用例，
   改造必须保持 wayland 判定不回归（对照跑一轮）。

## 5. 验收（整体）

| # | 判据 | 载体 |
|---|---|---|
| 1 | X 路线 `winehua_graphics_smoke` 出图（四象限可见 / CRC 序列推进） | 新判据 + 截屏 |
| 2 | X 路线 cube（dxvk）帧率 ≥60fps，`angleRegressions=0` | `displayroute-dxvk` job |
| 3 | 无 damage 不空转（`needsFrame=0`） | 合成器 `rate` 行 |
| 4 | wayland 路线既有套件不回退 | core / wine-vulkan-present / dxvk |
| 5 | 不变量检查器 `rel_unsynced_presented=0`（§2.6） | `[GUEST-FRAMES] stats` |

## 6. 不在本 spec 内

- 窗口形态 / 每窗系统窗（阶段 C）——它会在结构上进一步消掉合成那一跳；
- IME（阶段 B）；
- 教科书 GLX 补栈与"X 路线不接 OpenGL"的裁剪裁决——本 spec 用更省的路径绕开了该裁决，
  若本方案失败再回到该裁决。
