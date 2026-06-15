# 移花接木 (GraftWine) — multiton 多窗口架构设计

> 创建: 2026-06-15
> 状态: 设计方案, 待评审

---

## 1. 动机

当前 subWindow 方案的 Wine 窗口**不是一等公民**:

| 能力 | subWindow | 目标 |
|------|-----------|------|
| 任务栏独立条目 | ❌ | ✅ |
| 原生最小化/最大化 | ❌ (只有关闭 X) | ✅ |
| 分屏 | ❌ | ✅ |
| 独立生命周期 | ❌ (随主 Ability 生死) | ✅ |
| Alt-Tab 切换 | ❌ 看不到 | ✅ |

HarmonyOS 中, **只有 `TYPE_MAIN` 窗口才能获得任务栏条目**。而每个 UIAbility 实例恰好持有一个 `TYPE_MAIN` 窗口。multiton 模式允许每次 `startAbility()` 创建新实例 → 新 `TYPE_MAIN` 窗口 → 任务栏新条目。

---

## 2. 核心原理

### 2.1 关键约束

- **同进程**: 所有 UIAbility 实例默认共享同一进程 (无 `process` 字段时)
- **同 C++**: `libentry.so` 加载一次, `PluginManager` 和 `WaylandServer` 是进程级单例
- **multiton**: `module.json5` 中 `"launchType": "multiton"`, 每次 `startAbility()` 创建新实例

### 2.2 架构对比

```
── subWindow (当前) ──────────────      ── multiton (目标) ──────────────

EntryAbility (singleton)                EntryAbility (singleton)
  └── mainWindow (TYPE_MAIN)              └── mainWindow (TYPE_MAIN)
       ├── 控制面板 (Index.ets)                 └── 控制面板 (Index.ets)
       ├── subWindow notepad                   (无 subWindow)
       ├── subWindow winecfg              WineWindowAbility #1 (multiton)
       └── subWindow cmd                    └── mainWindow (TYPE_MAIN)
                                                 └── notepad 内容
                                            WineWindowAbility #2 (multiton)
                                              └── mainWindow (TYPE_MAIN)
                                                   └── winecfg 内容
```

### 2.3 为什么 subWindow 不行

```typescript
// subWindow 创建时的日志输出:
// 系统窗口管理不把 subWindow 当作独立窗口
// → 不在任务栏
// → 没有最小化/最大化按钮
// → 无法独立分屏
// → 没有独立 mission

// 这些是 HarmonyOS 的系统设计,
// subWindow 定位是"临时辅助窗口"而非"应用主窗口"
```

---

## 3. 组件交互

### 3.1 组件一览

```
┌─────────────────────────────────────────────────────┐
│  HarmonyOS 应用进程 (app.hackeris.honwine)            │
│                                                      │
│  ┌─────────────────────────┐                         │
│  │ EntryAbility (singleton)│  控制面板 / Wine 启动    │
│  │  ├── Index.ets          │                         │
│  │  └── WineWindowManager  │── 单例, 管理所有窗口     │
│  └────────┬────────────────┘                         │
│           │ startAbility(WineWindowAbility, ...)     │
│           │ emitter 通信                              │
│  ┌────────┴────────────────┐                         │
│  │ WineWindowAbility #1-*  │  每 Wine toplevel 一个   │
│  │  ├── pages/WineWindow   │  (复用同一页面)          │
│  │  └── TYPE_MAIN 窗口     │  任务栏可见!             │
│  └────────┬────────────────┘                         │
│           │ XComponent OnSurfaceCreated              │
│  ┌────────┴────────────────┐                         │
│  │ PluginManager           │  C++ 单例                │
│  │  ├── pendingToplevelId  │  路由 XComponent →       │
│  │  │                       │  EglRenderer            │
│  │  └── toplevelRenderers  │  Map<id, EglRenderer>   │
│  └────────┬────────────────┘                         │
│           │                                          │
│  ┌────────┴────────────────┐                         │
│  │ WaylandServer           │  C++ 单例                │
│  │  └── FireToplevelEvent  │  → ArkTS toplevel 回调  │
│  └─────────────────────────┘                         │
└─────────────────────────────────────────────────────┘
```

### 3.2 C++ 侧: 零改动

当前 C++ 架构已经支持 multiton 模式, 无需修改任何 C++ 代码:

```
WaylandServer::FireToplevelEvent(id, event, data)
  → NAPI threadsafe_function → ArkTS WineWindowManager.onToplevelEvent(id, event, data)

PluginManager::SetPendingToplevel(id)
  → 下一个 XComponent OnSurfaceCreated → 绑定到 toplevel #id 的 EglRenderer
```

这个 NAPI 接口抽象层保持不变: ArkTS 侧负责"创建窗口", C++ 侧不关心窗口是 subWindow 还是 TYPE_MAIN。

---

## 4. 详细设计

### 4.1 数据结构

```typescript
// WineWindowManager 内部状态
class WineWindowManager {
  // toplevelId → AbilityContext 映射 (用于 terminateSelf)
  private abilityContexts: Map<number, common.UIAbilityContext> = new Map();

  // toplevelId → 等待窗口就绪的 pending 事件 (同当前逻辑)
  private pendingEvents: Map<number, Array<[string, string]>> = new Map();

  // 是否正在启动 Ability (防止重复 startAbility)
  private pendingStarts: Set<number> = new Set();
}
```

### 4.2 场景一: Wine 创建 toplevel

```
时间线 ────────────────────────────────────────────────────→

C++ (Wayland线程)
  │ xdg_surface.get_toplevel()
  │   → FireToplevelEvent(id, "created", {w:640, h:480})
  │
  │   [后续: surface_commit → FireToplevelEvent(id, "resize", {w:800,h:600})]
  │
  ▼

ArkTS (JS主线程, 通过 NAPI threadsafe)
  │ WineWindowManager.onToplevelEvent(id, "created", data)
  │
  │   if (event === "created"):
  │     1. pendingStarts.add(id)           // 防止重复启动
  │     2. context.startAbility({           // 启动 multiton 实例
  │          bundleName: 'app.hackeris.honwine',
  │          abilityName: 'WineWindowAbility',
  │          parameters: { toplevelId: id, ...parsedData }
  │        })
  │     3. 暂存后续 resize/title 事件到 pendingEvents
  │
  │   [resize 事件可能在 Ability 启动期间到达]
  │   → 存入 pendingEvents[id]
  │
  ▼

系统 AMS (AbilityManagerService)
  │ 调度 WineWindowAbility 启动
  │ 创建新 WindowStage, 分配 TYPE_MAIN 窗口
  │
  ▼

WineWindowAbility.onCreate(want, launchParam)
  │ toplevelId = want.parameters.toplevelId
  │ WineWindowManager.registerAbility(toplevelId, this.context)
  │
  ▼

WineWindowAbility.onWindowStageCreate(windowStage)
  │ 1. mainWindow = windowStage.getMainWindowSync()
  │ 2. mainWindow.setWindowTitle(title)
  │ 3. testNapi.setPendingToplevel(toplevelId)   // ★ 必须在 loadContent 前
  │ 4. windowStage.loadContent('pages/WineWindow')
  │
  ▼

WineWindow.aboutToAppear()
  │ testNapi.initXComponent()
  │   → PluginManager::Export()  // 从 exports 获取 XComponent
  │     → pendingToplevelId_ 不为 0
  │     → 注册 XComponent callback
  │     → xcToToplevelId_[nxc] = pendingToplevelId_
  │
  ▼

XComponent.OnSurfaceCreated(window)
  │ → PluginManager::OnSurfaceCreated()
  │   → 创建 EglRenderer, 关联到 toplevel #id
  │   → RenderLoop 启动, 开始取帧渲染
  │
  ▼

WineWindowManager (收到 Ability 就绪通知)
  │ pendingStarts.delete(id)
  │ apply pendingEvents[id]    // resize → mainWindow.resize()
  │                            // title  → mainWindow.setWindowTitle()
  │ mainWindow.showWindow()    // ★ 现在才显示 (避免闪烁)
  │ → 用户在任务栏看到新窗口 ✓
```

### 4.3 场景二: 用户关闭窗口 (任务栏 X / 手势)

```
用户点击关闭按钮
  │
  ▼

系统 → WineWindowAbility.onDestroy()
  │ 1. testNapi.sendToplevelClose(toplevelId)   // 通知 Wine
  │      → xdg_toplevel_send_close(tl, ...)
  │      → Wine 收到 close 事件
  │ 2. testNapi.destroyToplevel(toplevelId)     // 清理 C++ renderer
  │      → PluginManager::DestroyToplevel(id)
  │ 3. WineWindowManager.unregisterAbility(toplevelId)
  │
  ▼

Wine 处理 close → xdg_toplevel.destroy()
  │ → FireToplevelEvent(id, "destroyed")
  │   → WineWindowManager 确认清理
  │   → (Ability 已经销毁, 不需要再 terminateSelf)
```

### 4.4 场景三: Wine 自行关闭 toplevel

```
C++ (Wine 调用 xdg_toplevel.destroy)
  │ → xdg_surface.destroy → FireToplevelEvent(id, "destroyed")
  │
  ▼

WineWindowManager.onToplevelEvent(id, "destroyed")
  │ let ctx = abilityContexts.get(id)
  │ if (ctx) {
  │   ctx.terminateSelf()       // 终止对应的 Ability 实例
  │   abilityContexts.delete(id)
  │   pendingEvents.delete(id)
  │ }
  │ testNapi.destroyToplevel(id)  // 清理 C++ renderer
  │
  ▼

系统 → WineWindowAbility.onDestroy()
  │ // Ability 已通过 terminateSelf 销毁
  │ // 任务栏条目移除 ✓
```

### 4.5 场景四: EntryAbility (控制面板) 关闭

```
EntryAbility.onDestroy()
  │ for (let [id, ctx] of abilityContexts) {
  │   testNapi.sendToplevelClose(id)  // 通知 Wine 关闭所有窗口
  │   testNapi.destroyToplevel(id)
  │ }
  │ abilityContexts.clear()
  │ testNapi.stopAll()  // 停止 Wine 进程 + Wayland compositor
  │
  ▼

Wine 进程退出 → 所有子窗口 Ability 自然销毁
```

---

## 5. 文件改动

### 5.1 `module.json5` — 新增 WineWindowAbility

```json5
{
  // ... 现有配置 ...
  "abilities": [
    {
      "name": "EntryAbility",
      "srcEntry": "./ets/entryability/EntryAbility.ets",
      "launchType": "singleton",    // 保持不变
      // ... label, icon, skills 不变 ...
    },
    {
      "name": "WineWindowAbility",
      "srcEntry": "./ets/entryability/WineWindowAbility.ets",
      "launchType": "multiton",
      "exported": false,             // 不暴露给外部应用
      "removeMissionAfterTerminate": true,  // 关闭后从任务列表移除
      "label": "",                   // 空 label, 由 Wine 标题动态覆盖
      // 不配置 skills (不需要从桌面启动)
    }
  ]
}
```

### 5.2 新文件: `WineWindowAbility.ets`

```typescript
import { AbilityConstant, UIAbility, Want } from '@kit.AbilityKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { window } from '@kit.ArkUI';
import testNapi from 'libentry.so';
import { WineWindowManager } from '../service/WineWindowManager';

const DOMAIN = 0x0000;

export default class WineWindowAbility extends UIAbility {
  private toplevelId: number = 0;
  private title: string = '';

  onCreate(want: Want, launchParam: AbilityConstant.LaunchParam): void {
    this.toplevelId = want.parameters?.toplevelId as number ?? 0;
    this.title = want.parameters?.title as string ?? '';

    if (this.toplevelId === 0) {
      hilog.error(DOMAIN, 'WWA', 'created without toplevelId, terminating');
      this.context.terminateSelf();
      return;
    }

    // 注册到全局管理器
    WineWindowManager.getInstance().registerAbility(this.toplevelId, this.context);

    hilog.info(DOMAIN, 'WWA', 'WineWindowAbility #%{public}d created: %{public}s',
      this.toplevelId, this.title);
  }

  onWindowStageCreate(windowStage: window.WindowStage): void {
    hilog.info(DOMAIN, 'WWA', '#%{public}d onWindowStageCreate', this.toplevelId);

    // ★ 关键: 必须在 loadContent 之前设置 pending toplevel
    testNapi.setPendingToplevel(this.toplevelId);

    let mainWindow = windowStage.getMainWindowSync();
    if (this.title) {
      mainWindow.setWindowTitle(this.title);
    }

    windowStage.loadContent('pages/WineWindow', (err) => {
      if (err.code) {
        hilog.error(DOMAIN, 'WWA', '#%{public}d loadContent failed: %{public}s',
          this.toplevelId, JSON.stringify(err));
        return;
      }
      hilog.info(DOMAIN, 'WWA', '#%{public}d loadContent ok', this.toplevelId);

      // 页面加载完成后通知管理器: 窗口已就绪
      WineWindowManager.getInstance().onAbilityReady(this.toplevelId);
    });

    // 监听窗口关闭
    mainWindow.on('windowEvent', (eventType: number) => {
      if (eventType === window.WindowEventType.WINDOW_DESTROYED) {
        hilog.info(DOMAIN, 'WWA', '#%{public}d window destroyed by user', this.toplevelId);
        // Wine toplevel close + renderer cleanup
        testNapi.sendToplevelClose(this.toplevelId);
        testNapi.destroyToplevel(this.toplevelId);
        WineWindowManager.getInstance().unregisterAbility(this.toplevelId);
      }
    });
  }

  onDestroy(): void {
    hilog.info(DOMAIN, 'WWA', '#%{public}d onDestroy', this.toplevelId);
    // 兜底: 如果还没清理则清理
    testNapi.sendToplevelClose(this.toplevelId);
    testNapi.destroyToplevel(this.toplevelId);
    WineWindowManager.getInstance().unregisterAbility(this.toplevelId);
  }
}
```

### 5.3 修改: `WineWindowManager.ets`

现有 ~250 行需要重大改造。核心变更:

```typescript
// ── 删除 ──
// createSubWindowWithOptions() 调用
// subWindow.resize / moveWindowTo / setWindowCornerRadius / showWindow
// 所有 subWindow 相关 import

// ── 新增 ──
import { common, Want } from '@kit.AbilityKit';
import { emitter } from '@kit.BasicServicesKit';

// 事件常量
const EVENT_TOPLEVEL_CREATED = 'wine_toplevel_created';
const EVENT_TOPLEVEL_DESTROYED = 'wine_toplevel_destroyed';

private abilityContexts: Map<number, common.UIAbilityContext> = new Map();
private pendingStarts: Set<number> = new Set();

// ★ 新: registerAbility — WineWindowAbility.onCreate 调用
registerAbility(toplevelId: number, ctx: common.UIAbilityContext): void {
  this.abilityContexts.set(toplevelId, ctx);
  hilog.info(DOMAIN, 'WineWM', '[MW] ability #%{public}d registered', toplevelId);
}

unregisterAbility(toplevelId: number): void {
  this.abilityContexts.delete(toplevelId);
  this.pendingEvents.delete(toplevelId);
  this.pendingStarts.delete(toplevelId);
}

// ★ 新: onAbilityReady — XComponent 就绪, 应用 pending 事件
onAbilityReady(toplevelId: number): void {
  hilog.info(DOMAIN, 'WineWM', '[MW] ability #%{public}d ready, flushing pending', toplevelId);
  let ctx = this.abilityContexts.get(toplevelId);
  if (!ctx) return;

  // 获取 mainWindow (从 abilityContext 需要通过 getWindowStage)
  //  实际上, 窗口尺寸/标题由 WineWindowAbility.onWindowStageCreate 直接操作
  //  pending 事件的消费也由 WineWindowAbility 负责
  //  这里只需要处理 toplevel destroyed 的 terminateSelf

  // 应用 pending 事件 (如果有积压的 resize/title)
  // ...
  this.pendingStarts.delete(toplevelId);
}

// ★ 改造: onToplevelEvent — "created" 改用 startAbility
private onToplevelEvent(id: number, event: string, data: string): void {
  switch (event) {
    case 'created':
      this.startWineWindowAbility(id, data);
      break;
    case 'destroyed':
      this.onToplevelDestroyed(id);
      break;
    case 'title':
    case 'resize':
      // 发送 emitter 事件, WineWindowAbility 监听并 apply
      emitter.emit(`wine_toplevel_${id}`, {
        data: { event, data }
      });
      break;
  }
}

private startWineWindowAbility(id: number, data: string): void {
  if (this.pendingStarts.has(id)) {
    hilog.warn(DOMAIN, 'WineWM', '[MW] start already in flight for #%{public}d', id);
    return;
  }
  this.pendingStarts.add(id);

  // 解析初始参数
  let title = '';
  let w = 800, h = 600;
  try {
    let obj = JSON.parse(data);
    if (obj.title) title = obj.title;
    if (obj.w) w = obj.w;
    if (obj.h) h = obj.h;
  } catch (_) {}

  let want: Want = {
    bundleName: 'app.hackeris.honwine',
    abilityName: 'WineWindowAbility',
    parameters: { toplevelId: id, title, w, h }
  };

  // 获取 context (从主 Ability)
  let ctx = this.uiAbilityContext;  // ★ 需要从 init 传入
  ctx.startAbility(want).then(() => {
    hilog.info(DOMAIN, 'WineWM', '[MW] startAbility #%{public}d done', id);
  }).catch((err: Error) => {
    hilog.error(DOMAIN, 'WineWM', '[MW] startAbility #%{public}d FAILED: %{public}s',
      id, JSON.stringify(err));
    this.pendingStarts.delete(id);
  });
}

private onToplevelDestroyed(id: number): void {
  let ctx = this.abilityContexts.get(id);
  if (ctx) {
    ctx.terminateSelf().catch((err: Error) => {
      hilog.warn(DOMAIN, 'WineWM', '[MW] terminateSelf #%{public}d: %{public}s',
        id, JSON.stringify(err));
    });
  }
  this.unregisterAbility(id);
}
```

### 5.4 修改: `WineWindowManager.init()`

```typescript
// 需要保存 mainAbility 的 UIAbilityContext
private uiAbilityContext: common.UIAbilityContext | null = null;

async init(windowStage: window.WindowStage, abilityContext: common.UIAbilityContext): Promise<void> {
  this.windowStage = windowStage;
  this.mainWindow = windowStage.getMainWindowSync();
  this.uiAbilityContext = abilityContext;  // ★ 新增

  // 其余不变...
}
```

### 5.5 修改: `EntryAbility.ets`

```typescript
onWindowStageCreate(windowStage: window.WindowStage): void {
  // 传入 abilityContext
  WineWindowManager.getInstance().init(windowStage, this.context);
  windowStage.loadContent('pages/Index', ...);
}
```

### 5.6 修改: `WineWindow.ets`

基本不变, 但需要知道 toplevelId (用于日志等):

```typescript
aboutToAppear(): void {
  // initXComponent 在 PluginManager::Export 中会消费 pendingToplevelId_
  // toplevelId 已被 WineWindowAbility.onWindowStageCreate 设置
  testNapi.initXComponent();
}
```

### 5.7 C++ 侧: 无需改动

当前 C++ 架构各层已充分解耦:
- `napi_init.cpp`: NAPI 函数不变
- `plugin_manager.cpp`: pendingToplevelId 路由不变
- `wayland_server.cpp`: FireToplevelEvent 不变
- `egl_renderer.cpp`: 独立 EGL context 不变
- `xdg_shell.cpp`: toplevel 事件不变

唯一可能需要的是: `sendToplevelClose` 当前是 stub (不实现), 需要真正发送 `xdg_toplevel_send_close`。

### 5.8 `main_pages.json`

无需改动 — 复用 `pages/WineWindow`。

---

## 6. 关键 API 和兼容性

### 6.1 API Level 要求

| API | 最低 API Level |
|-----|---------------|
| `launchType: "multiton"` | API 9+ |
| `UIAbilityContext.terminateSelf()` | API 9+ |
| `startAbility(want)` | API 9+ |
| `removeMissionAfterTerminate` | API 10+ |
| `emitter` | API 9+ |

项目使用 API 15 (SDK 5.0.4), 全部满足。

### 6.2 StartAbility Want 参数

```typescript
let want: Want = {
  bundleName: 'app.hackeris.honwine',    // 自己的 bundle
  abilityName: 'WineWindowAbility',       // multiton ability
  parameters: {
    toplevelId: number,                   // 自定义参数 → want.parameters.toplevelId
    title: string,
    w: number,
    h: number
  }
};
// parameters 支持 number/string/boolean, 不支持嵌套对象和 array
// → 如果需传更多参数, 可以用 JSON.stringify 塞 string 字段
```

### 6.3 terminateSelf 行为

```typescript
// HarmonyOS 文档: terminateSelf 终止当前 UIAbility 实例
// - multiton: 只终止被 startAbility 创建的当前实例
// - singleton: 终止唯一实例
// - 不影响同进程其他 Ability
await this.context.terminemeSelf();
```

---

## 7. 风险与缓解

### 7.1 startAbility 延迟

**风险**: 从 `startAbility()` 调用到 XComponent `OnSurfaceCreated` 可能需要 200–500ms, 用户感知窗口弹出延迟。

**缓解**:
- 同进程启动, JS 引擎已预热, 实际延迟 < 首次冷启动
- 可以在 `WineWindowAbility.onCreate` 中立即设置窗口标题/大小 (不等待 loadContent)
- 窗口在 `onAbilityReady` 后才 show, 但系统默认显示 TYPE_MAIN 窗口 — 如果需要在就绪前隐藏, 可用 `window.setWindowBackgroundColor('#000')` + 就绪后改为透明

### 7.2 窗口过多时内存

**风险**: 每个 WineWindowAbility 实例有独立 JS 上下文 (虽然同进程), 5+ 个窗口可能增加内存。

**缓解**:
- JS 上下文开销约 1–3MB/实例 (V8 isolate 共享)
- 页面都是轻量 WineWindow.ets (一个 XComponent), 页面级内存 < 500KB/实例
- Wine 本身通常不会同时打开超过 5 个 toplevel 窗口

### 7.3 竞态: toplevel destroyed 在 Ability 启动期间到达

**场景**:
```
t0: created → startAbility (200ms latency)
t50ms: destroyed (用户快速关闭)
t200ms: Ability 启动完成
```

**处理**:
```typescript
// WineWindowAbility.onCreate():
if (WineWindowManager.getInstance().isDestroyed(this.toplevelId)) {
  this.context.terminateSelf();  // 启动后就立即终止
  return;
}
```

### 7.4 任务栏行为兼容性

**风险**: 不同 OEM 的 HarmonyOS 实现可能对 multiton 任务栏行为有差异。

**缓解**: 需要真机验证。如果某设备上表现异常, 可以用 `supportWindowModes` 关掉全屏模式, 只用浮动窗口:

```json5
{
  "name": "WineWindowAbility",
  "supportWindowModes": ["floating"]  // 不允许全屏
}
```

### 7.5 EntryAbility 不可见时的控制面板

**风险**: 用户可能在任务栏只看到 Wine 窗口, 看不到控制面板。关闭最后一个 Wine 窗口后, EntryAbility 仍然存在但不可见。

**处理**: 可接受。EntryAbility 在后台静默运行, 用户通过桌面图标重新打开时会回到控制面板。

---

## 8. 实施步骤

### Phase 0: 准备 (C++ stub 补全)

- [ ] `sendToplevelClose`: 实现 `xdg_toplevel_send_close()` 调用
- [ ] 验证 `PluginManager::DestroyToplevel` 在窗口所有生命周期下正常工作

### Phase 1: ArkTS 侧重构

- [ ] `module.json5`: 新增 `WineWindowAbility`
- [ ] 新建 `WineWindowAbility.ets`
- [ ] 改造 `WineWindowManager.ets`:
  - `init()` 接收 `UIAbilityContext`
  - `onToplevelEvent("created")` → `startAbility()`
  - `onToplevelEvent("destroyed")` → `terminateSelf()`
  - 移除 subWindow 相关代码
- [ ] `EntryAbility.ets`: `init()` 传入 `this.context`

### Phase 2: 测试

- [ ] 单窗口: 启动 notepad → 出现在任务栏
- [ ] 多窗口: notepad + winecfg → 各自独立任务栏条目
- [ ] 用户关闭: 点 X → 窗口消失 + Wine toplevel 关闭
- [ ] Wine 关闭: xdg_toplevel.destroy → 窗口消失
- [ ] 标题更新: Wine set_title → 窗口标题栏更新
- [ ] 控制面板: 关闭最后一个 Wine 窗口后应用不崩溃

### Phase 3: 优化

- [ ] 窗口显示动画 (系统默认提供, 可能需要调参)
- [ ] 最小化时冻结 EGL 渲染 (节省 GPU)
- [ ] 窗口尺寸恢复 (记住上次 Notepad 的窗口位置/大小?)

---

## 9. 备选方案: 混合模式

如果 multiton 在真机上表现不好, 可以降级为**混合模式**:

```
EntryAbility (控制面板)
  └── 非全屏浮动窗口, 在任务栏仅显示一个

Wine 窗口仍然用 subWindow
  └── 加自定义标题栏 (绘制最小化/最大化按钮)
  └── 用浮动窗口模式让 EntryAbility 变成一个"任务栏替代品"
```

这不是优雅方案, 但可在 multiton 不可行时作为后备。

---

## 10. 参考资料

- [HarmonyOS Ability 启动模式](https://developer.huawei.com/consumer/cn/doc/harmonyos-guides-V5/uiability-launch-type-V5)
- [HarmonyOS startAbility](https://developer.huawei.com/consumer/cn/doc/harmonyos-references-V5/js-apis-inner-application-uiabilitycontext-V5)
- [HarmonyOS Window API](https://developer.huawei.com/consumer/cn/doc/harmonyos-references-V5/js-apis-window-V5)
- [HarmonyOS emitter (进程内事件)](https://developer.huawei.com/consumer/cn/doc/harmonyos-references-V5/js-apis-emitter-V5)
