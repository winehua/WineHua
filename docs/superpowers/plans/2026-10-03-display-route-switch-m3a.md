# 显示路线参数化开关（M3a）实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `winehua.displayRoute` 参数化开关（默认 wayland 零差异）+ x11 可交互台架（桌面 surface / 真实输入 / 生命周期 / guest env / smoke CLI）。

**Architecture:** 单点裁决 = ArkTS `DisplayRouteService`（同 `DesktopModeService` override 语义，不持久化）；native 仅 env 汇集点需要 route（启动时经 NAPI 一次性写入 native 镜像）；surface 与输入分发各只有一处 route 分支（DesktopWindow）；尺寸经 `chain_start(w,h)` 参数化，smoke 台架保持 800×600。

**Tech Stack:** ArkTS/ArkUI（EntryAbility、DesktopWindow、service）、NAPI（smoke_napi.cpp）、native C（display/ohos_output.c、display_input.c、proc/ 汇集点）、python（automation/smoke.py）。

**Spec:** `docs/superpowers/specs/2026-10-03-display-route-switch-m3a-design.md`（判据 §7、防坑 §6 与本计划冲突时以 spec 为准）

## Global Constraints

1. **默认态零差异**：参数缺席时不新建对象、无新日志、不走新分支（服务取值=一次内存读）。
2. **route 判断全库只许三处**：`DesktopWindow.ets`（surface 分发 + 输入分发）、native env 汇集点（stamp）。发现第四处即停。
3. **自动化台架不动**：`smokeDisplayRoute` 无尺寸实参时默认 800×600，smoke 套件证据链不变。
4. **WAYLAND_DISPLAY 互斥**：不在 env 表达「删除」——追加 `WINEHUA_DISPLAY_ROUTE=x11` + `DISPLAY=:0` 两键即可，删除动作由 `wine_child.cpp:492-498` 既有分支完成（后写胜出语义 + 既有 unsetenv）。
5. **构建/验证口径**：触 `entry/src/main/cpp/display/` 的任务跑 `make NATIVE_ARCH=arm64-v8a hap` + `displayroute-rate` job（设备 .206）；ArkTS-only 任务跑 hap 构建；收尾跑 core 套件。设备连接键以 `hdc list targets` 实时为准。
6. 提交信息不带 co-author。分支 `feature/display-route-m0`，每任务一提交。

## Review Focus

- **未带参冷启动**（回归面最大）：桌面开合、notepad、输入全走旧路径，逐项与变更前一致。
- **surfaceId 生命周期**：桌面窗关→开循环，每次 surfaceId 都变，旧值不可复用；合成器须干净收尾后重绑。
- **触摸除零/未启动**：surface 尺寸未知（0）时事件必须丢弃；合成器未启动时 post 无副作用。
- **一致性检查误伤**：`--display-route x11` 只对显式传参生效；不传时任何套件行为不变。
- **button 注入编码**：必须镜像 `display_input.c` 内部 InjectButton 的既有按键编码，不发明新编码。

---

### Task 1: DisplayRouteService + EntryAbility 解析

**Files:**
- Create: `entry/src/main/ets/service/DisplayRouteService.ets`
- Modify: `entry/src/main/ets/entryability/EntryAbility.ets`（`explicitDesktopMode` 段之后，约 :73）

**Interfaces:**
- Produces: `DisplayRouteService.getInstance().isX11(): boolean`、`routeName(): string`（后续所有任务消费）；常量 `ROUTE_WAYLAND='wayland'`、`ROUTE_X11='x11'`。

- [ ] **Step 1: 写 DisplayRouteService**（镜像 `DesktopModeService.ets:32-51` 的 override 部分，**无** preferences 持久化）：

```typescript
import { hilog } from '@kit.PerformanceAnalysisKit';

const DOMAIN: number = 0x0000;
export const ROUTE_WAYLAND: string = 'wayland';
export const ROUTE_X11: string = 'x11';

// 显示路线裁决 (wayland=现状 / x11=实验台架): 仅显式 want 生效, 不持久化 ——
// 每次冷启动必须带参 (防"实验后 app 停在 x11"的僵尸态)。单例理由与结构同
// DesktopModeService (AppStorage 跨 Ability 读取不可靠, EntryAbility 注释实证)。
// 消费点白名单 (spec §2.2): DesktopWindow surface 分发 / 输入分发 /
// native env 汇集点 (经 NAPI 镜像)。出现第四处 route 判断 = 违规。
export class DisplayRouteService {
  private static instance: DisplayRouteService | null = null;
  static getInstance(): DisplayRouteService {
    if (!DisplayRouteService.instance) {
      DisplayRouteService.instance = new DisplayRouteService();
    }
    return DisplayRouteService.instance;
  }
  private route: string = ROUTE_WAYLAND;
  setOverrideMode(v: string): void {
    if (v === ROUTE_WAYLAND || v === ROUTE_X11) {
      this.route = v;
      hilog.info(DOMAIN, 'wine', 'display route override set: %{public}s', v);
    } else {
      hilog.warn(DOMAIN, 'wine', 'display route override ignored: %{public}s', v);
    }
  }
  isX11(): boolean { return this.route === ROUTE_X11; }
  routeName(): string { return this.route; }
}
```

- [ ] **Step 2: EntryAbility 解析**（`explicitDesktopMode` 块后插入，模式照抄 :72-74）：

```typescript
    // 显示路线显式覆盖 (wayland|x11): 仅显式 want 生效, 不持久化 (spec
    // 2026-10-03 §2.2)。消费点白名单见 DisplayRouteService 头注释。
    const explicitDisplayRoute = !!parameters && parameters['winehua.displayRoute'] !== undefined &&
      parameters['winehua.displayRoute'] !== null;
    if (explicitDisplayRoute) {
      DisplayRouteService.getInstance().setOverrideMode(value('winehua.displayRoute', ''));
    }
```

（`value()` 为该文件既有 helper；import 区加 `DisplayRouteService`。）

- [ ] **Step 3: 验证**：`make NATIVE_ARCH=arm64-v8a hap`；
  `python3 automation/smoke.py install --device <键>`；无参冷启动 → hilog 无 `display route` 行；`hdc shell "aa start -a EntryAbility -b app.hackeris.winehua --ps winehua.displayRoute x11"` → hilog 出现 `display route override set: x11`；`--ps winehua.displayRoute bogus` → WARN 行且默认 wayland。
- [ ] **Step 4: Commit** `feat(display): winehua.displayRoute 参数解析——DisplayRouteService 单点裁决`

### Task 2: 尺寸参数化（六处 800×600 收敛）

**Files:**
- Modify: `entry/src/main/cpp/display/ohos_output.h`（chain_start 签名 :102-109）、`ohos_output.c`（:1067、:1255、:1269、:1299、:1312、:1368 + 新 dims getter）、`display_input.c`（:282）、`display_compositor.cpp`（DeferredOutputChainStart、StartWithSurface）、`smoke/smoke_napi.cpp`（SmokeDisplayRoute 可选 w/h）

**Interfaces:**
- Produces: `wl_ohos_output_chain_start(..., int out_w, int out_h)`；`void wl_ohos_output_dims(int *w, int *h)`（display_input 消费）；`WineHua_DisplayRoute_StartWithSurface(uint64_t sid, bool script, int w, int h)`（w/h≤0 ⇒ 800×600，smoke 台架语义）；NAPI `smokeDisplayRoute(sid, script?, w?, h?)`（argc 兼容旧两参/一参调用）。
- Consumes: Task 1 无依赖；本任务为 T3/T4 提供尺寸基座。

- [ ] **Step 1**：`ohos_output.c` 顶部 `g_out` 加 `int out_w, out_h;`；chain_start 存参（≤0 时回退 800/600 并 WARN），六处写死全部改引 `g_out.out_w/out_h`；:1368 日志改为打印实际值；新增：

```c
void wl_ohos_output_dims(int *w, int *h)
{
    if (w) *w = g_out.out_w > 0 ? g_out.out_w : 800;
    if (h) *h = g_out.out_h > 0 ? g_out.out_h : 600;
}
```

- [ ] **Step 2**：`display_input.c:282` 的 `int fw = 800, fh = 600;` 改 `wl_ohos_output_dims(&fw, &fh);`（include ohos_output.h）。
- [ ] **Step 3**：`display_compositor.cpp`：`DeferredOutputChainStart` 加 `int w, h;`；`StartWithSurface(surface_id, script_enabled, w, h)`（w/h≤0 ⇒ 800/600）传链；`WineHua_DisplayRoute_Start()` 调 `(0,false,0,0)`。
- [ ] **Step 4**：`smoke_napi.cpp` `SmokeDisplayRoute` 解析可选第 3/4 个 int 实参（argc 判据，缺省 0）。
- [ ] **Step 5: 验证**：hap 构建；`smoke.py run --job smoke/jobs/displayroute-rate.json --prefix reuse` → PASS 且 hilog `output chain up: 800x600`（不变即通过——这是零差异证据）。
- [ ] **Step 6: Commit** `feat(display): 输出尺寸参数化——chain_start(w,h) 收敛六处 800x600`

### Task 3: 桌面 surface 分发 + 合成器生命周期

**Files:**
- Modify: `entry/src/main/ets/pages/DesktopWindow.ets`（DesktopController）、`entry/src/main/cpp/display/display_compositor.cpp`（stop 路径）、`smoke/smoke_napi.cpp`（`smokeDisplayRouteStop`）

**Interfaces:**
- Consumes: T1 `isX11()`；T2 四参 `smokeDisplayRoute`。
- Produces: NAPI `smokeDisplayRouteStop()`（未启动时 no-op）。

- [ ] **Step 1**：DesktopController 加 `private x11Started: boolean = false; private x11W = 0; private x11H = 0;`。`onSurfaceCreated`：`isX11()` 时**不调** `testNapi.createRenderer`（直接 return，渲染器归属合成器）。`onSurfaceChanged`：x11 且未启动时记下 `rect.surfaceWidth/Height` 并启动：

```typescript
      if (DisplayRouteService.getInstance().isX11() && !this.x11Started) {
        this.x11Started = true;
        this.x11W = rect.surfaceWidth; this.x11H = rect.surfaceHeight;
        try { smokeNapi.smokeDisplayRoute(BigInt(surfaceId), false,
                rect.surfaceWidth, rect.surfaceHeight); } catch (e) { this.x11Started = false; }
        return; // x11 不走 wayland resize 链
      }
```

（尺寸在 onSurfaceChanged 而非 onSurfaceCreated 启动：rect 在此刻才可用，语义仍是「绑定时刻尺寸」。）`onSurfaceDestroyed`：x11 → `smokeNapi.smokeDisplayRouteStop()` + `x11Started=false`。
- [ ] **Step 2**：`display_compositor.cpp`：`DisplayRouteRetriggerWake` 扩展协议——字节 `'r'`=retrigger（现状）、`'s'`=停机（`g_stop=true`，主循环 `while(!g_stop)` 退出后走既有 fail 收尾段）；新增 `WineHua_DisplayRoute_Stop()`：`g_started` 为真时向 `g_retrigger_pipe[1]` 写 `'s'`（同一把锁保护），否则 no-op。
- [ ] **Step 3**：`smoke_napi.cpp` 加 `smokeDisplayRouteStop` 导出（props 表 :123-127 处追加）。
- [ ] **Step 4: 验证**（设备）：x11 冷启动开桌面窗 → hilog `output chain up: <W>x<H>`（实测值≈surface）；关窗 → `displayroute stopped or failed; cleaned up`；重开 → 新 chain 且 surfaceId 不同（三循环）。默认路线开合桌面 → 行为同旧（零差异）。
- [ ] **Step 5: Commit** `feat(display): x11 台架绑桌面 surface——一处分发 + stop 生命周期`

### Task 4: 输入接线（真实触摸/键盘）

**Files:**
- Modify: `entry/src/main/cpp/display/display_input.{c,h}`（post_button）、`smoke/smoke_napi.cpp`（`smokeDisplayRouteButton`）、`DesktopWindow.ets`（onTouch 顶部分支 + onKey）

**Interfaces:**
- Consumes: T2 `wl_ohos_output_dims`；既有 `smokeDisplayRouteKey/Motion`（smoke_napi.cpp:86,103）、`OH2EVDEV`（common/KeyMap）、`mapMouseButton`（common/MouseMap）。
- Produces: `void wl_ohos_input_post_button(int button, bool press)`（任意线程安全，镜像 post_key 队列模式；**编码=内部 InjectButton 既有值，Step 1 先读源码钉死**）。

- [ ] **Step 1**：读 `display_input.c` InjectButton 段，确认按键编码（左/中/右的整数值），按 post_key 同款投递队列暴露 `wl_ohos_input_post_button`；头文件声明；NAPI 导出。
- [ ] **Step 2**：DesktopPage 加 `private x11SurfW = 0; x11SurfH = 0;`（onSurfaceChanged x11 分支里更新）。`onTouch` **最顶部**：

```typescript
        if (DisplayRouteService.getInstance().isX11()) {
          if (this.x11SurfW <= 0 || this.x11SurfH <= 0) return; // 未绑定, 丢弃
          const t = ev.touches[0];
          const nx = t.x / this.x11SurfW, ny = t.y / this.x11SurfH;
          if (ev.type === TouchType.Down) {
            smokeNapi.smokeDisplayRouteMotion(nx, ny, 0);
            smokeNapi.smokeDisplayRouteButton(1, true);
          } else if (ev.type === TouchType.Move) {
            smokeNapi.smokeDisplayRouteMotion(nx, ny, 1);
          } else if (ev.type === TouchType.Up) {
            smokeNapi.smokeDisplayRouteButton(1, false);
            smokeNapi.smokeDisplayRouteMotion(nx, ny, 2);
          }
          return; // x11 不进 wayland 手势状态机
        }
```

（按钮值以 Step 1 实读编码替换 `1`。物理鼠标 onMouse 同构补三分支，键值经 `mapMouseButton` 映射到 InjectButton 编码。）
- [ ] **Step 3**：XComponent 链上加 `.onKey((ev: KeyEvent) => { const e = OH2EVDEV(ev.keyCode); if (e > 0) smokeNapi.smokeDisplayRouteKey(e, ev.type === KeyType.Down); })`（仅 x11 生效，wayland 分支不动）。
- [ ] **Step 4: 验证**（设备，人工为主——原则 #21）：x11 桌面拖动测试客户端窗口跟手、点按切焦点；`--ps winehua.displayRoute x11 --ps winehua.program C:\windows\notepad.exe` 冷启动 → 物理键盘输入有回显。默认路线桌面触摸/键盘行为同旧。
- [ ] **Step 5: Commit** `feat(display): x11 台架真实输入——post_button 补链 + 桌面事件直连`

### Task 5: guest env 汇集点

**Files:**
- Modify: `smoke/smoke_napi.cpp`（`setDisplayRoute` → native 镜像）、EntryAbility（启动时调用）、native env 汇集点（Step 1 定位，`entry/src/main/cpp/proc/` 下 `__env=` 追加点，spawner.cpp:47-53 注释所指函数）

**Interfaces:**
- Produces: NAPI `setDisplayRoute(name: string)`（仅接受 wayland/x11；启动时 EntryAbility 解析后调用一次）；native 汇集点 stamp：route=x11 时追加 `__env=WINEHUA_DISPLAY_ROUTE=x11`、`__env=DISPLAY=:0`（WAYLAND_DISPLAY 交由 wine_child 既有分支删除——Global Constraint 4）。

- [ ] **Step 1**：`grep -rn '"__env="\|__env=' entry/src/main/cpp/proc/` 钉出两条 spawn 路径（smoke runner / 产品 broker）共同的追加点；若两条路径各有一处，则 stamp 函数放 proc/ 公共头，两处各调一行。
- [ ] **Step 2**：native 镜像 + 汇集点 stamp（route 非 x11 时零操作）。
- [ ] **Step 3**：EntryAbility 在 `explicitDisplayRoute` 块内调 `smokeNapi.setDisplayRoute(value('winehua.displayRoute',''))`。
- [ ] **Step 4: 验证**：x11 冷启动 `winehua.program` 拉 notepad → wine stderr（沙箱 `temp/wine_stderr_*.log`，`hdc file recv -b` 回拉）出现 `[WineChild] display route=x11 (WAYLAND_DISPLAY removed)`；默认路线 → 无此行。
- [ ] **Step 5: Commit** `feat(display): guest 路由 env 汇集点——stampDisplayRoute 单函数两路径`

### Task 6: smoke CLI 与一致性检查

**Files:**
- Modify: `automation/smoke.py`（参数 :1233 区、aa start 拼装 :614-616 区、冷启动守卫 :467-473 区、一致性检查 :710-714 F7 逻辑旁）

**Interfaces:**
- Produces: `run --display-route {wayland,x11}`（默认 None = 现状）。

- [ ] **Step 1**：加参 + 冷启动携带 `--ps winehua.displayRoute <v>`（逐行照抄 desktop-mode 三处）。
- [ ] **Step 2**：一致性检查：`display_route == 'x11'` 时，选中测试声明 env 必须含 `WINEHUA_DISPLAY_ROUTE=x11`，否则 `die`（信息写明依据 spec §5；防 core 被静默拖进 x11）。`wayland` 值仅校验不拦（显式声明 wayland 的套件合法）。
- [ ] **Step 3: 验证**：`--display-route x11 --job smoke/jobs/displayroute-rate.json` 全程绿；`--display-route x11 --suite core` → die 且信息正确；无参 `--suite core` → PASS 不变。
- [ ] **Step 4: Commit** `feat(smoke): --display-route CLI + 路线声明一致性检查`

### Task 7: 验收跑批 + 毁灭性重建

**Files:** 无新改动（evidence 归档 `build/automation-logs/`）

- [ ] **Step 1 零差异门**（spec §7.1）：无参冷启动 → `smoke.py run --suite core` 4/4 PASS；桌面开合 + notepad 输入人工过一遍。
- [ ] **Step 2 x11 台架**（§7.2/7.3）：`--display-route x11` 开桌面 → rate 行/`output chain up` 证尺寸=surface；人工：拖窗跟手、点按焦点、键盘回显。
- [ ] **Step 3 A/B 对账**（§7.4/7.5）：`--display-route x11 --job smoke/jobs/displayroute-rate.json`（或 gl-resize）PASS；归档 presented-route 证据为 x11。
- [ ] **Step 4 毁灭性重建**（§7.6，复现原则 #4）：`rm -rf build/ && make NATIVE_ARCH=arm64-v8a`（全量）→ 重跑 Step 1/2/3 全绿。日志落 `build/automation-logs/`，失败非零退出不许宣布完成。
- [ ] **Step 5: Commit**（若有收尾修正）+ 汇报。
