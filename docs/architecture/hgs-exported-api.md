# HGS 导出接口规划（C++ / NAPI / ArkTS HAR）

> 最后核实：2026-11-04
> 适用仓库：`/workspace/WineHua-root/WineHua/hgs`（submodule `main`）
> 状态：**阶段 0 已实施；阶段 1 导出面已落地**（P1.1 canonical 导出 +
> 旧名弃用转发 + `.d.ts` 补齐；P1.3 setProcessEnvironment；N1 原生输入）。
> 阶段 2 窗口管理器 core（W1）、事件接线与 NAPI（W2a）、默认 ArkTS host（W2b）已落地。阶段划分与
> [hgs-shared-runtime-phase0.md](hgs-shared-runtime-phase0.md) 对齐。

## 0. 结论

HGS 的导出面分三层，稳定边界是 **NAPI**：

```text
C++ 宿主 / 同进程 native 模块
        │  hgs/api/*.h（源码级，随 HAR 编译）
        ▼
    libhgs.so  ── NAPI ──▶ ArkTS 宿主（entry / BetterSH / HarmonyBox）
        │
        └── hgs/Index.ets（HAR 导出：组件 / 服务 / Ability 基类）
```

三条原则：

1. **新 API 不再出现 backend 名词**：`createRenderer/destroyRenderer` 这类
   EGL 专属命名退出公共面，统一为 `attachSurface/resizeSurface/detachSurface`。
2. **旧导出不删，先转发再弃用**：WineHua 现有 55 个 NAPI 导出保持可用，
   新增 canonical 名称；旧名至少保留一个版本并打 deprecation 日志。
3. **窗口协议对象与宿主窗口分开命名**：`Toplevel` 指 Wayland 对象，
   `Window` 指 HGS 窗口管理器的宿主窗口模型。

## 1. 现状盘点

### 1.1 NAPI（`libhgs.so`，55 个导出）

| 域 | 现有导出 |
|---|---|
| Surface | `bindXComponent`、`createRenderer`、`resizeRenderer`、`destroyRenderer` |
| Server | `startServer`、`stopServer`、`setOutputSize`、`setDisplayScale`、`setDesktopMode`、`getDesktopRootId`、`setStateCallback` |
| Toplevel | `setToplevelCallback`、`getCurrentToplevelId`、`setPendingToplevel`、`cancelPendingToplevel`、`destroyToplevel`、`sendToplevelClose`、`notifyToplevelResize`、`notifyToplevelResizeEnd`、`findToplevelAt`、`raiseToplevel`、`setToplevelVisible` |
| Input | `sendPointerEvent`、`sendKeyEvent`、`sendScrollEvent` |
| Pointer 扩展 | `setPointerLockCallback`、`registerHostWindow` |
| IME | `setImeCallback`、`sendImeCommit`、`sendImePreedit`、`imeBackspace` |
| Gamepad/Controller | `initGameController`、`cleanupGameController`、`isGamepadConnected`、`getGamepadCount`、`setGamepadButtonCallback`、`setGamepadAxisCallback`、`setGamepadDeviceCallback`、`setGamepadRumbleCallback`、`controllerSet*`、`controllerGet*`、`controllerStartBridge`、`controllerStopBridge`（12 个） |
| 日志/诊断 | `LOGI`、`LOGW`、`LOGE`、`dumpWindowLayout` |

### 1.2 `.d.ts` 与 NAPI 不一致

`hgs/src/main/cpp/types/libhgs/Index.d.ts` 只声明了 36 个左右导出，
**缺少全部 gamepad/controller 导出，也缺 `getDesktopRootId`、`stopServer`**。
运行时能调、类型系统看不到，属于当前公共契约的缺口。阶段 1 必须补齐或明确标为 internal。

### 1.3 ArkTS HAR（`Index.ets`）

```ts
export { HgsSurface }              // 通用 surface 组件
export { HgsWindowPage }           // 通用窗口页 (W2b)
export { HgsWindowHostService }    // 默认 ArkTS 窗口 host (W2b)
export { HgsAppWindowAbility }     // 通用窗口 Ability 基类 (W2b)
```

`HgsSurface` 目前同时暴露 Wine 专用 props（`mouseMode/floating/touchpadOn/
scaleRawDelta/enterSynth`），不属于通用 surface 契约，阶段 3 前要拆到
`WineInputSurface`。

## 2. 命名与版本规则

### 2.1 命名

| 规则 | 示例 |
|---|---|
| 动词在前，领域明确 | `startServer`、`attachSurface`、`setWindowHost` |
| ID 类型写进参数名 | `toplevelId`（uint32）、`surfaceId`（uint64/bigint）、`generation`（uint32） |
| 协议对象用 Toplevel | `raiseToplevel`、`closeToplevel`、`notifyToplevelResize` |
| 宿主窗口用 Window | `setWindowHost`、`notifyWindowGeometry` |
| 新公共面不出现 Renderer/EGL/NativeImage | 统一 `Surface` |
| 异步/跨线程回调用 `Callback` | `setToplevelEventCallback`、`setWindowCallback` |

### 2.2 版本

- `HGS_API_VERSION` 宏 / `getApiVersion()`：`major << 16 | minor`，阶段 0 为 `1`。
- 新增导出 = minor +1；删改签名 = major +1。
- `getLastError()` 返回最近一次失败原因（线程安全，空串表示无错误）。
- 弃用流程：保留旧名 → 内部转发新名 → 打 `[HGS-DEPRECATED]` 日志 →
  下一个 major 删除。至少跨一个 minor 版本。

### 2.3 稳定性分级

| 级别 | 内容 | 变更规则 |
|---|---|---|
| Stable | runtime/surface/input/events/toplevel/window | 只能加法；破坏性变更走 major |
| Experimental | IME、pointer lock、gamepad/controller、zero-copy | 可改，需在 `.d.ts` 标 `@experimental` |
| Deprecated | `createRenderer` 等旧名 | 保留一个 minor，然后删 |
| Internal | `dumpWindowLayout`、controller 调试接口 | 不进主 `.d.ts` 或单独 `hgs/debug` |

## 3. C++ 公共 API

### 3.1 头文件清单

| 头 | 内容 | 阶段 | 稳定性 |
|---|---|---|---|
| `api/hgs_types.h` | ID/配置/输入/窗口基础类型 | 0 | Stable |
| `api/hgs_events.h` | `ToplevelEventType` + 名称双向映射 | 0 | Stable |
| `api/hgs_host.h` | `IHgsHost` / `ToplevelEvent` | 0 | Stable |
| `api/hgs_endpoint.h` | socket 路径拆分 / `ClientEnvironment` | 0 | Stable |
| `api/hgs_instance.h` | `HgsInstance` 生命周期/配置/Surface/输入 | 0 | Stable |
| `api/hgs.h` | 伞头 | 0 | Stable |
| `api/hgs_window_types.h` | `WindowId/WindowSpec/WindowEvent/WindowState` | 2 | Stable |
| `api/hgs_window_host.h` | `IWindowHost` | 2 | Stable |
| `api/hgs_window_policy.h` | `IWindowPolicy` | 2 | Stable |
| `api/hgs_window_manager.h` | `HgsWindowManager` | 2 | Stable |
| `api/hgs_export.h` | `HGS_API` 可见性宏 | 0（空宏）/ 1（启用） | Internal |

### 3.2 符号可见性

阶段 0 不做 `-fvisibility=hidden`：`seam/host_hooks.cpp`、`seam/zero_copy.cpp`
仍靠跨 `.so` 的 `dlsym` 双向解析符号，收紧可见性会先破坏 WineHua。
阶段 1 把 seam 改成显式 provider 后，再：

```cmake
set_target_properties(hgs PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)
```

公共类/函数用 `HGS_API` 标注；seam 符号单独用 `HGS_SEAM_API`。

跨项目复用走 **HAR 源码依赖**（同一 HGS 版本在各宿主内重新编译），
不承诺 C++ ABI 稳定；如果未来需要二进制分发/第三方 native 宿主，
再补一层 `hgs_c_api.h` 的 C ABI 包装。

### 3.3 C++ 导出清单

```cpp
namespace hgs {

// hgs_instance.h
class HGS_API HgsInstance {
public:
    static HgsInstance& Instance();
    static uint32_t ApiVersion();          // HGS_API_VERSION
    bool Start(const HgsConfig&);
    void Stop();
    bool IsRunning() const;
    std::string LastError() const;         // 失败原因，空串 = 无错误
    std::string WaylandDisplayName() const;
    std::vector<std::string> ClientEnvironment() const;
    void SetHost(IHgsHost*);
    void SetOutputSize(int32_t, int32_t);
    void SetDesktopMode(bool);
    void SetInputBackend(InputBackend);
    InputBackend GetInputBackend() const;
    ToplevelId DesktopRootId() const;
    SurfaceBinding AttachSurface(ToplevelId, SurfaceToken);
    bool ResizeSurface(ToplevelId, int32_t, int32_t, SurfaceGeneration = 0);
    bool DetachSurface(ToplevelId, SurfaceGeneration = 0);
    void SendPointerEvent(const PointerEvent&);
    void SendKeyEvent(const KeyEvent&);
    void SendScrollEvent(const ScrollEvent&);
    void NotifyToplevelResize(ToplevelId, int32_t, int32_t, bool);
    void SendToplevelClose(ToplevelId);
    void RaiseToplevel(ToplevelId);
    void SetToplevelVisible(ToplevelId, bool);
    void DestroyToplevel(ToplevelId);
    ToplevelId FindToplevelAt(int32_t, int32_t) const;
    void SetPendingToplevel(ToplevelId);
    ToplevelId TakePendingToplevel();
    void CancelPendingToplevel(ToplevelId);
    // 阶段 2
    HgsWindowManager& WindowManager();
};

// hgs_host.h
class HGS_API IHgsHost {
public:
    virtual ~IHgsHost() = default;
    virtual void OnToplevelEvent(const ToplevelEvent&) = 0;
    virtual void OnStateChanged(std::string_view) = 0;
};

} // namespace hgs
```

Native XComponent 的 `OHNativeWindow*` 绑定（`OnSurfaceCreated` 回调里拿到）
由 HGS 内部 `graphics/egl_surface` → `PluginManager::CreateRendererFromWindow`
完成，**不进入公共 API**；公共面只暴露 `AttachSurface(toplevelId, surfaceId)`。

## 4. NAPI 导出规划

### 4.1 为什么不改成嵌套对象

HarmonyOS native module 的 `.d.ts` 生态以**扁平命名导出**为主，
现有 `import hgs from 'libhgs.so'; hgs.startServer(...)` 已经形成习惯。
改成 `hgs.runtime.start(...)` 会牵动 WineHua/BetterSH 全部调用点，
收益只是命名分组。因此：**保持扁平导出**，用命名前缀和文档分组，
不引入嵌套对象。C++ 侧仍按 namespace 分组。

### 4.2 新增 canonical 导出（阶段 1）

```ts
// ── 枚举（数值与 C++ 枚举一致，勿重排）──────────────────
export enum HgsCompositionMode { Unspecified = 0, PerToplevel = 1, DesktopRoot = 2 }
export enum HgsInputBackend { Auto = 0, ArkTS = 1, Native = 2 }

// 与 hgs::ToplevelEventType 一一对应
export enum HgsToplevelEventType {
  Unknown = 0,
  Created = 1, Destroyed = 2,
  PopupHide = 3, PopupMove = 4, PopupShow = 5, PopupResize = 6,
  Argb = 7, Resize = 8, Surface = 9, Title = 10, Limits = 11,
  Maximized = 12, Unmaximized = 13, Fullscreen = 14, Unfullscreen = 15,
  Minimized = 16, Restored = 17, MoveStart = 18, MoveEnd = 19,
  Modal = 20, DesktopRoot = 21,
}
```

```ts
// ── runtime ──────────────────────────────────────────────
export interface HgsFeatureConfig {
  shm?: boolean;
  subsurface?: boolean;
  viewporter?: boolean;
  xdgPopup?: boolean;
  dataDevice?: boolean;
  relativePointer?: boolean;
  textInput?: boolean;
  externalSurface?: boolean;
  winehuaToplevel?: boolean;
}

export interface HgsOutputConfig {
  width?: number;          // 0/缺省 = 不修改
  height?: number;
  scale?: number;
  refreshMilliHz?: number;
}

export interface HgsConfig {
  socketPath: string;
  output?: HgsOutputConfig;
  composition?: HgsCompositionMode;
  inputBackend?: HgsInputBackend;
  features?: HgsFeatureConfig;
  setProcessEnvironment?: boolean;
}

// 旧调用 startServer('...') 继续可用；新调用传对象。
// 对象形式等价于先 setOutputSize/SetDesktopMode/setInputBackend 再 startServer；
// 字段缺省（0/undefined）不修改宿主现状。
export const startServer: (config: string | HgsConfig) => boolean;
export const stopServer: () => void;
export const isRunning: () => boolean;
export const getWaylandDisplayName: () => string;
export const getClientEnvironment: () => string[];
export const setOutputSize: (w: number, h: number) => void;
export const setDesktopMode: (enabled: boolean) => void;
export const getDesktopRootId: () => number;
export const setInputBackend: (mode: HgsInputBackend) => void;
export const getInputBackend: () => HgsInputBackend;
export const getApiVersion: () => number;
export const getLastError: () => string;
```

```ts
// ── surface ──────────────────────────────────────────────
// generation 由 attachSurface 分配；0 = 失败
export const attachSurface: (toplevelId: number, surfaceId: bigint) => number;
export const resizeSurface: (toplevelId: number, width: number, height: number,
                              generation?: number) => boolean;
export const detachSurface: (toplevelId: number, generation?: number) => boolean;

// 原生输入/后续 XComponent 补挂；toplevelId 可省略以兼容旧调用
export const bindXComponent: (xcomp: object, toplevelId?: number) => void;
```

Native 模式（`libraryname:'hgs'` + `inputBackend=Native`）不需要调用
`attachSurface/resizeSurface/detachSurface`：`OnSurfaceCreated` 已经拿到
`OHNativeWindow*`，由 HGS 内部直接绑定 renderer。`attachSurface` 只服务
ArkTS `XComponentController` 路径。

页面侧必须把 generation 存到实例字段，change/destroy 回传：

```ts
private generation: number = 0;

// onSurfaceCreated
this.generation = hgs.attachSurface(this.toplevelId, BigInt(surfaceId));

// onSurfaceChanged
hgs.resizeSurface(this.toplevelId, w, h, this.generation);

// onSurfaceDestroyed
hgs.detachSurface(this.toplevelId, this.generation);
```

```ts
// ── input ────────────────────────────────────────────────
// action 继续用 number 保持与现有 ArkTS MouseAction 兼容：
// 1=Press 2=Release 3=Move（与 hgs::PointerAction 一致）
export const sendPointerEvent: (toplevelId: number, action: number,
    px: number, py: number, button: number,
    rawDeltaX?: number, rawDeltaY?: number, fromMouse?: boolean) => void;
export const sendKeyEvent: (toplevelId: number, evdevCode: number,
                             pressed: boolean) => void;
// axis：0=垂直 1=水平
export const sendScrollEvent: (toplevelId: number, axis: number, value: number,
    scrollStep: number, px: number, py: number) => void;
```

```ts
// ── events / toplevel ────────────────────────────────────
// 新类型化回调：type 为 HgsToplevelEventType 数值，json 原样透传
export const setToplevelEventCallback: (
    cb: (id: number, type: HgsToplevelEventType, json: string) => void) => void;
export const setStateCallback: (cb: (state: string) => void) => void;

export const setPendingToplevel: (id: number) => void;
export const takePendingToplevel: () => number;   // 出队语义
export const cancelPendingToplevel: (id: number) => void;
export const notifyToplevelResize: (id: number, w: number, h: number,
                                    resizing?: boolean) => void;
export const closeToplevel: (id: number) => void;       // xdg close
export const destroyToplevel: (id: number) => void;     // 本地强收尾（含 pointer lock）
export const raiseToplevel: (id: number) => void;
export const setToplevelVisible: (id: number, visible: boolean) => void;
export const findToplevelAt: (px: number, py: number) => number;
```

```ts
// ── window manager（阶段 2）──────────────────────────────
export enum HgsWindowState { Normal = 0, Minimized = 1, Maximized = 2, Fullscreen = 3 }

export enum HgsWindowEventType {
  Unknown = 0,
  Created = 1, Destroyed = 2, TitleChanged = 3,
  GeometryChanged = 4, StateChanged = 5, FocusChanged = 6,
  VisibleChanged = 7, CloseRequested = 8,
  MoveRequested = 9, ResizeRequested = 10,
  PopupShown = 11, PopupMoved = 12, PopupResized = 13, PopupHidden = 14,
}

export interface HgsWindowSpec {
  id: number;
  pid: number;
  appId: string;
  title: string;
  // 对应 C++ WindowSpec.geometry；JS 侧扁平化，便于宿主直接使用
  x: number; y: number; width: number; height: number;
  minWidth?: number; minHeight?: number;
  maxWidth?: number; maxHeight?: number;
  parentId?: number;
  modal?: boolean;
  decorated?: boolean;
}

export interface HgsWindowGeometry { x: number; y: number; width: number; height: number; }

export interface HgsWindowEvent {
  type: HgsWindowEventType;
  id: number;
  spec: HgsWindowSpec;
  geometry: HgsWindowGeometry;
  state: HgsWindowState;
  focused: boolean;
  edges: number;         // ResizeRequested
}

export interface HgsWindowHost {
  createWindow(spec: HgsWindowSpec): void;
  destroyWindow(id: number): void;
  setGeometry(id: number, x: number, y: number, w: number, h: number): void;
  setVisible(id: number, visible: boolean): void;
  setTitle(id: number, title: string): void;
  setState(id: number, state: HgsWindowState): void;
  startMove(id: number): void;
  startResize(id: number, edges: number): void;
  requestClose(id: number): void;
}

export const setWindowCallback: (cb: (event: HgsWindowEvent) => void) => void;
export const setWindowHost: (host: HgsWindowHost) => boolean;
export const notifyWindowGeometry: (id: number, x: number, y: number,
                                    w: number, h: number) => void;
export const notifyWindowFocus: (id: number, focused: boolean) => void;
export const notifyWindowState: (id: number, state: HgsWindowState) => void;
export const notifyWindowClosed: (id: number) => void;
export const getWindow: (id: number) => HgsWindowSpec | null;
export const listWindows: () => HgsWindowSpec[];
export const getWindowZOrder: () => number[];
```

### 4.3 旧名兼容表（阶段 1 起）

| 旧导出 | 处理 | 新名/语义 |
|---|---|---|
| `createRenderer(id, sid)` | 转发 + 弃用 | `attachSurface(id, sid)`（旧名忽略返回的 generation） |
| `resizeRenderer(id, w, h)` | 转发 + 弃用 | `resizeSurface(id, w, h, 0)` |
| `destroyRenderer(id)` | 转发 + 弃用 | `detachSurface(id, 0)` |
| `getCurrentToplevelId()` | 转发 + 弃用 | `takePendingToplevel()`（出队语义） |
| `notifyToplevelResizeEnd(id)` | 转发 + 弃用 | `notifyToplevelResize(id, 0, 0, false)` |
| `sendToplevelClose(id)` | 转发 + 弃用 | `closeToplevel(id)` |
| `setToplevelCallback(cb)` | 保留（字符串版） | 新代码用 `setToplevelEventCallback` |
| `setDisplayScale(s)` | 保留 no-op + 弃用 | 阶段 2 由 `HgsConfig.output.scale` 取代 |
| `startServer(path)` | 保留 | 同时接受 `HgsConfig` 对象 |
| `stopServer/setOutputSize/setDesktopMode/getDesktopRootId` | 保留 | 语义不变 |
| `sendPointerEvent/sendKeyEvent/sendScrollEvent` | 保留 | 语义不变 |
| `setPendingToplevel/cancelPendingToplevel` | 保留 | 语义不变 |
| `notifyToplevelResize/raiseToplevel/setToplevelVisible/findToplevelAt/destroyToplevel` | 保留 | 语义不变 |
| `bindXComponent(xcomp)` | 保留 | 新增可选 `toplevelId` |
| `setStateCallback` | 保留 | 语义不变 |
| `setPointerLockCallback/registerHostWindow` | 保留，标 Experimental | Wine 扩展 |
| IME 五个 | 保留，标 Experimental | Wine 扩展 |
| gamepad/controller 十二个 | 保留，标 Experimental，**补进 `.d.ts`** | 扩展 |
| `LOGI/LOGW/LOGE` | 保留 | 通用日志 |
| `dumpWindowLayout` | 保留，标 Internal | 诊断 |

### 4.4 `.d.ts` 规范

- 全部导出必须有声明；运行时存在但 `.d.ts` 缺失的（gamepad/controller）在阶段 1 补齐。
- `surfaceId` 用 `bigint`，不要用 `BigInt`（后者是构造器对象类型）。
- 回调类型写全参数名；可选参数用 `?`，不要用 `any`。
- 实验/弃用用 JSDoc：
  ```ts
  /** @deprecated 用 attachSurface；阶段 3 删除 */
  export const createRenderer: ...
  /** @experimental Wine 专用；BetterSH/HarmonyBox 不要使用 */
  export const setImeCallback: ...
  ```

## 5. ArkTS HAR 导出规划

### 5.1 目标 `Index.ets`

```ts
// 公共组件 / 服务
export { HgsSurface } from './src/main/ets/components/HgsSurface';
export { HgsWindowPage } from './src/main/ets/components/HgsWindowPage';
export { HgsWindowHostService } from './src/main/ets/service/HgsWindowHostService';
export { default as HgsAppWindowAbility } from './src/main/ets/entryability/HgsAppWindowAbility';

// 公共类型/枚举（ArkTS 侧）
export { HgsInputBackend, HgsWindowState, HgsWindowEventType } from './src/main/ets/types/HgsTypes';
export type { HgsConfig, HgsWindowSpec, HgsWindowHost } from './src/main/ets/types/HgsTypes';

// 过渡/兼容
```

`HelloWorld` / `EmptyEglView` / `HgsWindowAbility` / `openHgsWindow`
已删除（2026-11-04 占位清理）；`smoke/egl_renderer` 空 EGL 渲染器同步删除，
native 路径统一由 `CreateRendererFromWindow` 建真实渲染器。

### 5.2 `HgsSurface` 公共 props

| prop | 类型 | 稳定性 | 说明 |
|---|---|---|---|
| `xcId` | `string` | Stable | XComponent id |
| `toplevelId` | `number` | Stable | 静态 id |
| `resolveId` | `() => number` | Stable | 动态 id（桌面 root） |
| `inputBackend` | `number` | Stable | 0/1/2 |
| `keyboardTarget` | `number` | Stable | 键盘目标 |
| `tag` | `string` | Stable | 日志前缀 |
| `mouseMode/floating/touchpadOn/scaleRawDelta/enterSynth` | — | Deprecated | 阶段 3 移到 WineHua 的 `WineInputSurface` |

### 5.3 `HgsWindowHostService` 公共 API

```ts
export interface HgsWindowHostOptions {
  abilityName?: string;   // 默认 'HgsAppWindowAbility'
  pageUrl?: string;       // 默认 'pages/HgsWindowPage'
  windowScale?: number;   // 默认 1；BetterSH 可设 2
}

export interface HgsWindowHostStartOptions {
  socketPath: string;
  outputWidth?: number;        // >0 才下发
  outputHeight?: number;
  inputBackend?: number;       // 默认 2 (Native)
  composition?: number;        // 默认 1 (PerToplevel)
  dataDevice?: boolean;        // 默认 true
  winehuaToplevel?: boolean;   // 默认 false
  setProcessEnvironment?: boolean;  // 默认 false
}

export class HgsWindowHostService {
  static init(ctx: common.UIAbilityContext, options?: HgsWindowHostOptions): void;
  // 封装 hgs.startServer；须先 init。string 形式只给 socketPath，其余用默认。
  static start(options: string | HgsWindowHostStartOptions): boolean;
  static shutdown(): void;
}
```

### 5.4 `HgsAppWindowAbility`

```ts
export default class HgsAppWindowAbility extends UIAbility {
  // onCreate: 读 want.parameters.toplevelId/title/w/h
  // onWindowStageCreate: setPendingToplevel -> loadContent(pageUrl)
  //                       -> mainWindow.resize(w*scale, h*scale)
  // onWindowStageDestroy: hgs.notifyWindowClosed(toplevelId)
}
```

宿主侧只需要一个 5 行子类（注册到 `module.json5`）和一个 5 行页面包装。

## 6. 窗口管理器 host 接口

### 6.1 C++

`IWindowHost`（core → host）见 §15.3；host → core 直接调
`HgsWindowManager::NotifyHostGeometry/Focus/Close/State`，不在 host 接口里
再定义一套反向虚函数。

### 6.2 NAPI

统一走 `setWindowHost(host)` + `setWindowCallback(cb)` 两条通道：

```text
core ──setWindowCallback──▶ JS：窗口事件（Created/Title/State/...）
core ──HgsWindowHost──────▶ JS：createWindow/setGeometry/...（操作请求）
JS   ──notifyWindow*──────▶ core：几何/焦点/状态/关闭回执
```

返回语义：

- 所有 host 方法都是 `void`，调用即“已投递”；JS 侧异步执行。
  `createWindow` 失败（`startAbility` 抛错/窗口创建失败）由 host 调
  `notifyWindowClosed(id)` 回滚 core 状态。
- 失败只记日志，不抛异常、不做同步回传（避免 TSFN 阻塞 UI 线程）。
- `setWindowHost(host): boolean`：false 表示已有 host 或参数非法；这是唯一同步返回值。

线程：core 可能在 Wayland 线程处理事件，但 `setWindowCallback` 与
`HgsWindowHost` 的所有调用都经 TSFN 投递到 UI 线程执行，JS 代码不会跑在
Wayland 线程；因此 host 实现里可以直接调 `startAbility` / `window.Window` API。

## 7. 扩展接口

| 扩展 | 导出 | 稳定性 | 归属 |
|---|---|---|---|
| IME | `setImeCallback`、`sendImeCommit`、`sendImePreedit`、`imeBackspace` | Experimental | WineHua |
| Pointer lock | `setPointerLockCallback`、`registerHostWindow` | Experimental | WineHua |
| Gamepad/Controller | 现有 12 个 | Experimental | WineHua |
| Zero-copy / VirGL / Venus | C++ `IExternalSurfaceProvider`（阶段 4），不新增 NAPI | Experimental | WineHua |
| 诊断 | `dumpWindowLayout`、`getLastError`、`getApiVersion` | Internal/Stable | 通用 |

扩展接口不进入 BetterSH/HarmonyBox 的接入文档；它们只依赖 Stable 组。

## 8. 分阶段导出计划

| 阶段 | 新增导出 | 旧导出变化 | ArkTS HAR |
|---|---|---|---|
| 0 ✅ | 无（只落 C++ 头文件） | 无 | 无 |
| 1 ✅ | `startServer(config)`、`isRunning`、`getWaylandDisplayName`、`getClientEnvironment`、`getApiVersion`、`getLastError`、`attachSurface`、`resizeSurface`、`detachSurface`、`setInputBackend`、`getInputBackend`、`setToplevelEventCallback`、`takePendingToplevel`、`closeToplevel`、`bindXComponent(2 参)` | `createRenderer/resizeRenderer/destroyRenderer/getCurrentToplevelId/notifyToplevelResizeEnd/sendToplevelClose` 转发并打弃用日志；`.d.ts` 补全 gamepad/controller 等缺失声明 | `HgsSurface` 增加 `inputBackend`（N1 原生输入） |
| 2 | `setWindowCallback`、`setWindowHost`、`notifyWindowGeometry/Focus/State/Closed`、`getWindow/listWindows/getWindowZOrder`（W1/W2a/W2b 已落地，真机待回归） | `setToplevelCallback` 标 deprecated | 新增 `HgsWindowPage`、`HgsWindowHostService`、`HgsAppWindowAbility` |
| 3 | 无新增（BetterSH/HarmonyBox 用 Stable 组） | 删除 `createRenderer` 等弃用别名 | 拆出 `WineInputSurface`；`HgsSurface` 删除 Wine props |
| 4 | `IExternalSurfaceProvider`（C++） | — | 无 |

## 9. 兼容性检查清单

阶段 1 落地前逐项确认：

- [ ] `startServer` 同时接受 string 和 object；NAPI 用 `napi_typeof` 分支。
- [ ] `attachSurface` 返回 generation；旧 `createRenderer` 仍返回 `undefined`，
      不暴露 generation。需要 generation 的宿主改用 `attachSurface`。
- [ ] `resizeSurface/detachSurface` 的 `generation` 缺省为 0（通配），旧调用不传。
- [ ] `bindXComponent` 第二参数可选；旧一参调用只注册 surface 回调、不建 renderer。
- [ ] `setToplevelEventCallback` 与 `setToplevelCallback` 共用内部唯一的 typed sink；
      两者是它的两个适配器，后注册者覆盖前者并打 WARN。宿主只能选一个，
      不能指望同时订阅。
- [ ] `getLastError` 在每次失败路径写入；成功不清理（避免误导），或成功清空（二选一，文档写死）。
- [ ] `.d.ts` 与 `napi_init.cpp` 导出表逐项对齐（当前差 gamepad/controller）。
- [ ] `surfaceId` 类型统一 `bigint`。
- [ ] 弃用日志只在首次调用每个旧名时打印，避免刷屏。

## 10. 待确认

1. **是否保留 `setToplevelCallback` 字符串版长期可用？**
   建议：保留到阶段 3 结束；WineHua 迁到 `setWindowCallback` 后删除。
2. **`startServer` 用 union 还是新增 `configure(config)`？**
   建议：union。旧调用零改动；新调用一个对象表达完整配置。
3. **gamepad/controller 是否进入主 `.d.ts`？**
   建议：进入，标 `@experimental`；否则运行时可用但类型缺失，属于契约漏洞。
4. **是否现在启用 `-fvisibility=hidden`？**
   建议：阶段 1 拆完 seam 后再启用；阶段 0 只定义 `HGS_API` 宏（空实现）。
