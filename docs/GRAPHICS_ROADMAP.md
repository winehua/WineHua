# Wine 图形路线图：从控制台到上屏

> 现状: `cmd.exe /c echo hello` 通过, 无图形
> SDK 可用: EGL, GLESv2/v3, Vulkan, libnative_drm
> SDK 缺失: X11, Wayland, libgbm

---

## 总体思路

Wine 的图形输出需要两步：
1. Wine 内部渲染（GDI → OpenGL/Vulkan → 像素）
2. 像素送到屏幕（需要某种"显示器"）

鸿蒙没有 X11/Wayland，但可以交叉编译轻量 X11 栈丢进 HNP。
选择 X11 而非 Wayland 的理由：
- Xvfb 只有 2MB，Weston 需要 10MB+ 且依赖 wayland-protocols
- Wine 对 X11 支持最成熟
- X11 库标准 C，交叉编译简单

最后一步：不追求"嵌进鸿蒙窗口"，先做到"能截取像素画面"即可验证。

---

## 第一步：交叉编译 X11 最小栈

**验证的问题**: 鸿蒙 Linux 环境能否运行 X Server + Wine 连接它？

**要编译的**:
| 库 | 大小估算 | 说明 |
|----|---------|------|
| xcb-proto | 0 | 只生成头文件 |
| libxcb | ~500KB | X protocol C 绑定 |
| xcb-util | ~200KB | xcb 辅助 |
| libX11 | ~1MB | X11 客户端 |
| libXext | ~100KB | X 扩展 |
| xorgproto | 0 | 协议头文件 |
| Xvfb | ~2MB | 虚拟 framebuffer X server |

**操作**: 用 OHOS clang 交叉编译，产物放入 `out/staging/opt/winebox/`，重新编译 Wine `--with-x`

**验证通过标准**: 在设备上手动启动 Xvfb，`DISPLAY=:99 wine cmd.exe /c echo hello` 不报 X11 错误

**风险**: xcb 可能用到 sysroot 没有的 libc 函数；可逐个解决

---

## 第二步：启用 Wine OpenGL 渲染 (GDI 初始化)

**验证的问题**: Wine GDI + OpenGL 能否用鸿蒙的 EGL/GLES 初始化成功？

**操作**: 重新配置 Wine:
```bash
--with-x --with-opengl --with-freetype
```
链接鸿蒙 SDK 的 `libEGL.so`, `libGLESv2.so`

**验证通过标准**: `xvfb-run wine winecfg`（或 notepad）不报 GDI 错误
- `ps aux` 看到 `notepad.exe` 和 `wineserver` 进程存活
- 日志没有 "failed to init GDI" 等错误

**风险**: 
- OpenGL ↔ EGL 映射可能需要 `libglvnd` 或手动 stub
- Freetype 需要交叉编译（SDK 可能没有）

---

## 第三步：截取 framebuffer 画面

**验证的问题**: Wine 是否确实画出了界面内容？

**操作**: 用 `xwd` 或自己写的小程序从 Xvfb 的 framebuffer 读取像素，保存为图片
```bash
# Xvfb 内存中的 framebuffer 可以通过 X11 API 获取
xwd -root -display :99 | convert xwd:- screenshot.png
```

**验证通过标准**: `screenshot.png` 包含 notepad 窗口的像素画面（哪怕只是空白窗口）

---

## 第四步：像素传到鸿蒙侧显示

两种方案，先易后难。

### 方案 A：X11 路线（优先，配合步骤1-3）

从 Xvfb framebuffer 提取像素 → 共享内存 → 鸿蒙 XComponent 显示。

**操作**:
1. Linux 侧: `XDALLAlias` 或定时 `XGetImage` 抓取 root window 像素，写入 `shm_open` 共享内存
2. 鸿蒙侧 (ArkUI XComponent Native): `mmap` 打开同一块共享内存，渲染到 surface

**优势**: 步骤1-3 已验证 X11 栈可用，直接延续
**劣势**: 每帧需要 CPU 拷贝像素（1080p ≈ 8MB/帧），30fps 还行，60fps 吃力

### 方案 B：Wayland 路线（后续切换）

步骤3 验证图形渲染成功后，切换到 Wayland 以获得更好的鸿蒙集成：

**新增编译项**:
| 库 | 用途 | 
|----|------|
| wayland-protocols | XML → C 协议绑定 |
| libwayland-client | Wine `winewayland.drv` 链接 |
| libwayland-server | 自写 compositor |

**操作**:
1. 重新编译 Wine: `--with-wayland`（可与 X11 共存）
2. 写最小 `HarmonyCompositor`（基于 libwayland-server，几百行）
3. Compositor 劫持 Wine 每个 `xdg_toplevel` 创建事件
4. 通过 TCP socket 通知鸿蒙侧："新窗口 id=X, 尺寸=WxH"
5. 鸿蒙侧 `window.createWindow` 动态拉起 XComponent 窗口
6. 像素传输：`wl_shm` → 共享内存 fd 直通 XComponent

**架构**:
```
┌─ Linux 兼容环境 ─────────────────────┐
│  Wine (winewayland.drv)               │
│    ↓ wayland protocol                 │
│  HarmonyCompositor (libwayland-server) │
│    ↓ TCP localhost:8888               │
├─ 鸿蒙侧 ─────────────────────────────┤
│  ArkUI 主应用                          │
│    → XComponent (共享内存 fd / CPU拷贝) │
│    → window.createWindow (浮动窗口)     │
└───────────────────────────────────────┘
```

**优势**: 
- 原生 Wayland 协议，无 X11 中间层
- Wine 程序每个窗口 → 鸿蒙独立浮动窗口，交互自然
- 后续可升级 DMA-BUF 零拷贝

**风险**:
- `winewayland.drv` 相对年轻，可能有兼容 bug
- 自写 compositor 需要处理 subsurface、data_device 等细节
- Wayland 依赖链比 X11 多（libffi, expat 等）

### 路线切换时机

```
步骤1: 交叉编译 Xvfb     ←── 先走 X11
步骤2: Wine --with-x     ←── 验证 GDI 能初始化
步骤3: 抓帧截图          ←── 验证像素正确
步骤4: 方案A 上屏        ←── 先用 X11 像素搬运
    ↓ (确认图形栈稳定后)
切换到 Wayland:
  步骤4B: 交叉编译 wayland + 写 compositor
  步骤4C: Wine --with-wayland + 鸿蒙窗口调度
```

---
## 备选路线

如果 X11 交叉编译阻力太大：

**直接 EGL 路线**: 写一个最小 Wine 图形驱动 (`wineohos.drv`)，直接用 EGL 创建
offscreen pbuffer surface 做渲染，完全绕过 X11/Wayland。工作量较大但依赖最少。

**DRM 路线**: 利用 `libnative_drm.so` 直接控制帧缓冲，类似嵌入式 Linux 方案。
不确定鸿蒙 DRM 接口是否对第三方进程开放。

---
## 下一步操作

当前从**第一步**开始：交叉编译 Xvfb 和依赖库。首选 Alpine Linux musl x86_64 预编译包。
