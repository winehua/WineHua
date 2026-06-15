# 移花接木 — 鼠标键盘输入设计

> 创建: 2026-06-15
> 状态: 设计方案
> 依赖: multiton 多窗口架构 (已实现)

---

## 1. 现状

```
Wine → Wayland 协议查询 wl_seat → not found → 输入设备不初始化 → Wine 窗口"石化"
```

当前 Wayland compositor 注册了 `wl_compositor`、`wl_shm`、`xdg_wm_base`、`wl_subcompositor`、`wp_viewporter`、`wl_output`，**缺少 wl_seat**。

XComponent 侧回调 `DispatchTouchEvent = {}`，触控事件被丢弃。

---

## 2. 目标架构

```
┌──────────────────────────────────────────┐
│  HarmonyOS 输入栈                          │
│   触控 / 鼠标 / 键盘 → XComponent 回调     │
└────────────┬─────────────────────────────┘
             │ DispatchTouchEvent / DispatchMouseEvent / KeyEvent
             ▼
┌──────────────────────────────────────────┐
│  PluginManager (C++)                      │
│   ├── 坐标转换 (surface → wine content)    │
│   ├── 事件路由 (XComponent → toplevelId)  │
│   └── → Seat::InjectPointer/Keyboard     │
└────────────┬─────────────────────────────┘
             │
             ▼
┌──────────────────────────────────────────┐
│  Seat (C++, wayland)                      │
│   ├── wl_seat global                      │
│   ├── wl_pointer: enter/leave/motion/     │
│   │    button/axis/frame                  │
│   ├── wl_keyboard: keymap/enter/leave/    │
│   │    key/modifiers                      │
│   └── focus tracking (per-surface)        │
└────────────┬─────────────────────────────┘
             │ Wayland protocol events
             ▼
┌──────────────────────────────────────────┐
│  Wine (Wayland 客户端)                     │
│   ├── wl_pointer_listener → 鼠标事件      │
│   ├── wl_keyboard_listener → 键盘事件     │
│   └── 输入驱动初始化 ✓                     │
└──────────────────────────────────────────┘
```

---

## 3. C++ 组件设计

### 3.1 Seat (`seat.h/cpp`) — 新文件

管理 wl_seat global + pointer/keyboard/touch 设备，维护 focus surface。

```cpp
class Seat {
public:
    static Seat* GetInstance();

    // 注册 wl_seat global 到 wl_display
    void Register(wl_display* display);
    void Unregister();

    // ── 事件注入 (由 PluginManager 调用) ──

    // pointer
    void PointerEnter(uint32_t toplevelId, wl_fixed_t sx, wl_fixed_t sy);
    void PointerMotion(uint32_t toplevelId, wl_fixed_t sx, wl_fixed_t sy);
    void PointerButton(uint32_t button, uint32_t state);
    void PointerLeave();

    // keyboard
    void KeyboardEnter(uint32_t toplevelId);
    void KeyboardKey(uint32_t key, uint32_t state);
    void KeyboardLeave();

    // ── focus 管理 ──
    // toplevelId → wl_resource* 映射 (由 WaylandServer 提供)
    void OnSurfaceCreated(uint32_t toplevelId, wl_resource* surface);
    void OnSurfaceDestroyed(uint32_t toplevelId);

private:
    wl_global* global_ = nullptr;

    // 设备资源 (per-client, 单 client 只有一个)
    wl_resource* pointerResource_ = nullptr;
    wl_resource* keyboardResource_ = nullptr;
    wl_resource* touchResource_ = nullptr;

    // focus
    uint32_t focusedToplevel_ = 0;
    wl_resource* focusedSurface_ = nullptr;

    // 状态
    uint32_t serial_ = 1;
    wl_array* pressedKeys_ = nullptr;  // for keyboard_enter
};
```

### 3.2 Seat 绑定流程

当 Wine 客户端 bind `wl_seat`:

```
wl_seat_bind(client, data, version, id)
  → wl_resource_create(wl_seat_interface)
  → wl_seat_send_capabilities(pointer | keyboard)  // 声明能力
  → wl_seat_send_name("Wine-Virtual-Seat")

客户端收到 capabilities 后 bind wl_pointer / wl_keyboard:
  seat_get_pointer(client, seatRes, id)
    → wl_resource_create(wl_pointer_interface)
    → pointerResource_ = res  // 存引用, 后续事件发给它
    → wl_pointer_send_frame(res)  // 首个 frame 事件

  seat_get_keyboard(client, seatRes, id)
    → wl_resource_create(wl_keyboard_interface)
    → keyboardResource_ = res
    → wl_keyboard_send_keymap(WL_KEYBOARD_KEYMAP_FORMAT_NO_KEYMAP, fd, 0)
    → (Wine 用 xkbcommon 处理 keymap, 但 NO_KEYMAP 下回退到 raw keycode)
    → wl_keyboard_send_repeat_info(40, 400)
```

### 3.3 坐标转换

```
OHOS 触控 (XComponent 内坐标)
   │
   ▼
wineX = (touchX - vpX) / scale
wineY = (touchY - vpY) / scale
   │
   ▼
wl_pointer_send_motion(time, wl_fixed_from_double(wineX), wl_fixed_from_double(wineY))
```

其中 `scale = vpW / contentW`（viewport 缩放到 XComponent surface）。

计算所需参数由 `EglRenderer::GetViewport(out vpX, out vpY, out vpW, out vpH, out contentW, out contentH)` 暴露。

### 3.4 触控 → wl_pointer 映射

| OHOS TouchEvent | wl_pointer 事件 |
|-----------------|-----------------|
| DOWN | `wl_pointer_send_enter` (首次) → `wl_pointer_send_motion` → `wl_pointer_send_button(BTN_LEFT, PRESSED)` → `wl_pointer_send_frame` |
| MOVE (单点) | `wl_pointer_send_motion` → `wl_pointer_send_frame` |
| UP | `wl_pointer_send_button(BTN_LEFT, RELEASED)` → `wl_pointer_send_frame` |

多点触控暂不映射（Wine 不依赖多点触控手势）。

### 3.5 鼠标事件

`OH_NativeXComponent_MouseEvent_Callback` (API 9+):

| OHOS MouseEvent | wl_pointer 事件 |
|-----------------|-----------------|
| MOUSE_PRESS (LEFT) | `wl_pointer_send_button(BTN_LEFT, PRESSED)` |
| MOUSE_RELEASE (LEFT) | `wl_pointer_send_button(BTN_LEFT, RELEASED)` |
| MOUSE_MOVE | `wl_pointer_send_motion` (先 motion 再处理) |
| MOUSE_PRESS (RIGHT) | `wl_pointer_send_button(BTN_RIGHT, PRESSED)` |

滚轮: `OH_NativeXComponent_MouseEventButton` 检查 BACK_BUTTON/FORWARD_BUTTON → `wl_pointer_send_axis`。

### 3.6 键盘事件 — Keycode 映射

OHOS keycode 值域与 Linux evdev 不重叠。需要映射表：

```cpp
// 常用键映射 (OHOS → Linux evdev)
static const std::unordered_map<int32_t, uint32_t> kKeycodeMap = {
    // 字母
    {KEY_A, 30}, {KEY_B, 31}, ..., {KEY_Z, 44},
    // 数字
    {KEY_0, 11}, {KEY_1, 2}, ..., {KEY_9, 10},
    // 功能
    {KEY_ENTER,     28},
    {KEY_ESCAPE,    1},
    {KEY_SPACE,     57},
    {KEY_TAB,       15},
    {KEY_BACKSPACE, 14},
    {KEY_DELETE,    111},   // OHOS KEY_DELETE → evdev KEY_DELETE
    {KEY_SHIFT_LEFT,  42},
    {KEY_SHIFT_RIGHT, 54},
    {KEY_CTRL_LEFT,   29},
    {KEY_CTRL_RIGHT,  97},
    {KEY_ALT_LEFT,    56},
    {KEY_ALT_RIGHT,   100},
    {KEY_F1, 59}, ..., {KEY_F12, 88},
    // 方向键
    {KEY_UP,    103},
    {KEY_DOWN,  108},
    {KEY_LEFT,  105},
    {KEY_RIGHT, 106},
    // ...
};
```

完整映射表约 100+ 项，覆盖标准 PC 键盘。

`OH_NativeXComponent_KeyAction`: `KEY_ACTION_DOWN = 0`, `KEY_ACTION_UP = 1`。
对应 wl_keyboard key_state: `WL_KEYBOARD_KEY_STATE_PRESSED = 1`, `WL_KEYBOARD_KEY_STATE_RELEASED = 0`。

---

## 4. 改动清单

### 4.1 新文件

| 文件 | 说明 |
|------|------|
| `seat.h` | Seat 类声明 |
| `seat.cpp` | wl_seat/pointer/keyboard/touch 实现 + keycode 映射 |

### 4.2 修改文件

| 文件 | 改动 |
|------|------|
| `wayland_server.h` | 新增 `GetDisplay()` (返回 wl_display*) + toplevelId→wl_surface 映射 |
| `wayland_server.cpp` | Start() 中调用 `Seat::Register(display_)`; surface_commit 新增 wl_surface 注册 |
| `plugin_manager.h` | 新增 XComponent→toplevelId 查询; 新增 mouse/key/focus callback 声明 |
| `plugin_manager.cpp` | 实现 DispatchTouchEvent + RegisterMouseEventCallback + RegisterKeyEventCallback; Expose viewport 查询 |
| `egl_renderer.h` | 新增 `GetViewport(int& vpX, vpY, vpW, vpH, contentW, contentH)` |
| `egl_renderer.cpp` | GetViewport 返回当前 letterbox 视口参数 |

### 4.3 不改的文件

- `xdg_shell.cpp` — 不变
- `napi_init.cpp` — 不变
- ArkTS 侧 — 不变

---

## 5. 事件路由链

每个 `DispatchTouchEvent` 携带 `OH_NativeXComponent* component`:

```
PluginManager::DispatchTouchEvent(component, window)
  → xcToToplevelId_[component] → toplevelId
  → toplevelRenderers_[toplevelId].GetViewport(...)
  → 坐标转换 (touch → wine content)
  → Seat::InjectPointer(toplevelId, wx, wy, button, state)
    → wayland_server: toplevelId → wl_surface resource
    → wl_pointer_send_enter(motion/button/frame)
```

### 首次触控 (无 focus):

```
DOWN 事件:
  → PointerEnter(toplevelId)  // 发送 wl_pointer_enter + surface + coordinates
  → PointerMotion(x,y)        // 发送 wl_pointer_motion
  → PointerButton(BTN_LEFT, PRESSED)  // 发送 wl_pointer_button
  → frame
```

### 移动:

```
MOVE 事件:
  → 如果 toplevelId 变了 → PointerLeave(旧) + PointerEnter(新)
  → 如果同一 surface → PointerMotion(x, y) + frame
```

### 抬起:

```
UP 事件:
  → PointerButton(BTN_LEFT, RELEASED) + frame
```

### 键盘:

```
KeyDown:
  → 如果无 focus 或 focus 不同 → KeyboardEnter(toplevelId)
  → mapKeycode(OHOS → evdev)
  → wl_keyboard_send_key(key, WL_KEYBOARD_KEY_STATE_PRESSED)
  → wl_keyboard_send_modifiers(...)

KeyUp:
  → wl_keyboard_send_key(key, WL_KEYBOARD_KEY_STATE_RELEASED)
```

---

## 6. 坐标转换细节

XComponent 100%×100% 填充 WineWindow 页面 → EGL surface 大小 = 窗口物理大小。

EGL 渲染 letterbox:
```cpp
// 当前 egl_renderer.cpp 已有计算
float contentAspect = (float)fw / (float)fh;
float surfaceAspect = (float)width_ / (float)height_;
// vpX, vpY, vpW, vpH = viewport in surface pixels
// fw, fh = content (Wine frame) in logical pixels
```

转换:
```cpp
// touch coordinates relative to XComponent surface
float wineX = (touchX - vpX) * (fw / vpW);
float wineY = (touchY - vpY) * (fh / vpH);
```

`GetViewport(int& vpX, int& vpY, int& vpW, int& vpH, int& contentW, int& contentH)` 需要加锁，因为 viewport 在渲染线程中更新。

---

## 7. 实施步骤

### Phase 0: 基础 Seat (无 focus tracking)
- [ ] `seat.h/cpp`: wl_seat global + pointer/keyboard 设备 + bind 逻辑
- [ ] `wayland_server.cpp`: Start() 中注册 Seat
- [ ] 验证: Wine 启动后能看到 wl_pointer/wl_keyboard 绑定日志

### Phase 1: 触控 → 指针转发
- [ ] `egl_renderer.h/cpp`: 暴露 GetViewport
- [ ] `plugin_manager.cpp`: 实现 DispatchTouchEvent
- [ ] 坐标转换 + Seat::InjectPointer
- [ ] 验证: notepad 菜单可以点击

### Phase 2: 鼠标事件 (仅 2in1 设备)
- [ ] `plugin_manager.cpp`: RegisterMouseEventCallback
- [ ] 鼠标按钮映射
- [ ] 验证: 外接鼠标操作

### Phase 3: 键盘
- [ ] Keycode 映射表
- [ ] `plugin_manager.cpp`: RegisterKeyEventCallback
- [ ] XComponent 获取键盘焦点 (ArkTS 侧 `focusable(true)`)
- [ ] 验证: 键盘输入英文到 notepad

---

## 8. Keycode 完整映射

OHOS keycode 定义在 `<ace/xcomponent/native_xcomponent_key_event.h>`:

```
OHOS code range: 2000-3000+
Evdev code range: 1-255

Char keys:  KEY_A=2017 → evdev 30  (offset: -1987)
Digits:     KEY_0=2000 → evdev 11  (offset: -1989)
Numpad:     KEY_NUMPAD_0=2100 → evdev 82  (offset: -2018)
F-keys:     KEY_F1=2029 → evdev 59  (offset: -1970)
Navigation: KEY_DPAD_UP=2064 → evdev 103  (offset: -1961)
Modifiers:  KEY_SHIFT_LEFT=2047 → evdev 42
            KEY_CTRL_LEFT=2049 → evdev 29
            KEY_ALT_LEFT=2051 → evdev 56
Media/Sys:  KEY_ENTER=2054 → evdev 28
            KEY_SPACE=2043 → evdev 57
            KEY_TAB=2044 → evdev 15
            KEY_ESCAPE=2058 → evdev 1
            KEY_BACKSPACE=2057 → evdev 14
```

映射表将放置在 `seat.cpp` 中，约 120 项，覆盖完整标准键盘 + 小键盘。

---

## 9. 风险

| 风险 | 说明 | 缓解 |
|------|------|------|
| Wine 使用 xkbcommon + NO_KEYMAP | 没有真实 keymap fd，raw keycode 映射可能丢失修饰键语义 | 如果不用 Ctrl/Alt 快捷键，raw keycode 足够。后续可用 memfd_create 注入最小 keymap |
| 键盘焦点 | 键盘事件需 XComponent 有焦点 | ArkTS 侧 `XComponent.focusable(true)` + `requestFocus()` |
| 触控灵敏度 | ~60fps 触控采样 vs wl_pointer 坐标精度 | wl_fixed_t 24.8 精度足够 (1/256 px) |
| 多 surface 焦点切换 | 触控在 toplevel 间移动需正确 enter/leave | PluginManager 检查 toplevelId 变化自动切换 |
