# x86_64 PC 已知问题

> 状态：未解决（2026-09-22 归档）。文中的问题没有修复，之后也未再排查或复现。

> 更新: 2026-07-31（首次记录于 2026-06 底）

## 图形: ChoosePixelFormat 失败 (PFD_DRAW_TO_WINDOW 不支持)

### 现象

`ChoosePixelFormat()` 返回 0，`GetLastError()` = 183 (ERROR_ALREADY_EXISTS)。

### 根因

WGL/OpenGL driver (opengl32.so → winewayland.drv) 枚举了 12 个 pixel format，但全部缺少 `PFD_DRAW_TO_WINDOW` 标志:

```
trace:opengl:wglChoosePixelFormat PFD_DRAW_TO_WINDOW required but not found for iPixelFormat=1
trace:opengl:wglChoosePixelFormat PFD_DRAW_TO_WINDOW required but not found for iPixelFormat=2
...
trace:opengl:wglChoosePixelFormat PFD_DRAW_TO_WINDOW required but not found for iPixelFormat=12
```

`PFD_DRAW_TO_WINDOW` 对应 EGL 的 `EGL_WINDOW_BIT` (EGL_SURFACE_TYPE)。所有 pixel format 缺少此标志意味着 guest `libEGL.so` 只支持离屏渲染 (pbuffer)，不支持窗口表面渲染。
（`winewayland.drv/opengl.c` 直接复用宿主 driver 的 `p_init_pixel_formats`；`win32u/opengl.c` 中 `PFD_DRAW_TO_WINDOW` 依赖 EGL surface 的 `EGL_WINDOW_BIT`。）

### 推测原因（未验证）

x86_64 上 guest Mesa (libEGL.so 及其依赖) 以原生方式执行，设置 `EGL_PLATFORM=wayland` 时，guest 内的 libwayland-client.so 需要连接 Wayland compositor。但 compositor 运行在 host 进程内，guest 代码无法直接访问。

arm64 上 Box64 通过指令翻译加载 guest 库，可能在 syscall 层面桥接了 guest Wayland client 到 host compositor。

### 状态

🟡 未修复。x86_64 原生执行 guest graphics 的架构限制（7-13 后 guest_gfx 路径
`EGL_PLATFORM=wayland` + `BOX64_EMULATED_LIBS` 已纳入 libEGL 模拟清单，但本问题
描述的是 x86_64 原生路径，需在 x86_64 设备上复核是否仍复现）。
