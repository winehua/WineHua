# wine 定制

> 适用场景：改 wine 源码、合并上游 wine、排查"某个行为是上游的还是我们改的"。
> 基线：13289668（2026-06-07，upstream/master merge-base，winehua 独有 118 commit，`git merge-base` 实测该基线至今未合入新上游）
> 生成：2026-08-01；2026-09-25 逐条对照代码核实，补齐标题栏主题化 / modal 协议 / 手柄 / resolver 等遗漏（逐文件行数以核实日 diff 为准）
> 说明：wine 改动以新增文件为主（+24045/-343），合并策略是"先合新文件、再处理修改文件"。鸿蒙相关代码以 `__OHOS__` 宏、`ohos_*` 文件/函数、`WINEHUA_*` 环境变量为标记；少数 PE 编译目标（mmdevapi/client.c、setupapi/queue.c）不能用 `__OHOS__`，改为按驱动名/无条件生效并留注释。

## 定制点总览

| 定制点 | 问题 | 思路 |
|---|---|---|
| wineserver 宿主拉起 | wineserver 是 `.so` 不能 posix_spawn | ntdll 走 Process Broker 请求宿主创建，socket 扫描 + 就绪轮询防重复 |
| 子进程转发 | NCP 模式无嵌套 fork/execve | `spawn_process` 全部转发宿主 broker；`fork_and_exec` 直接返回失败 |
| 匿名 RWX / noexec | 内核拒匿名 RWX，数据分区文件映射加不上 X | `ohos_map_exec_section` 等委托层：可执行段匿名 RWX 映射、PE 头页 pread 读入 |
| 盘符映射 | 沙箱无 symlink，`dosdevices/c:` 建不出 | `ohos_drive_unix_path()` 硬编码映射（C:/Z:），server 侧建命名事件对象供枚举 |
| 路径布局 | 打包路径与发行版不同 | `WINEDATADIR`/`WINEBINDIR`/`WINEUNIXDIR` 环境变量覆盖 |
| DLL 搜索 | WoW64 下 exe 旁 DLL 找不到；DXVK/vkd3d 要按档位注入 | loader 搜索路径扩展：exe 目录恢复 + `\??\unix` overlay（d3d11/dxgi/d3d12） |
| 私有 swapchain | 宿主只有 OHNativeWindow，无 Vulkan 原生 WSI | surface/swapchain 全实现在 win32u，present 经 `vn_winehua_present` 交宿主 |
| 桌面模式窗口 | 虚拟桌面下所有窗口都要 subsurface 根 | 桌面模式跳过"仅父窗口可见"过滤；min/max 约束扩展 |
| 装饰窗受管判定 | 程序自绘阴影窗被判成独立窗口，多出黑窗 | `is_window_managed` 对带 owner 的纯装饰浮层返回 FALSE，走 subsurface 通路 |
| 标题栏按钮 | 主题字体顶掉系统中文字体；高 DPI 下按钮成窄高矩形 | aero.msstyles 删字体行；defwnd.c 尺寸统一函数，绘制与 hit test 共用 |
| 模态对话框上报 | 宿主合成器需要知道模态关系来编排窗口 | `winehua-toplevel` 私有 Wayland 协议 + modal 探测上报 |
| 音频后端 | 沙箱不能调 OHOS 音频 API | wineohos.drv：Unix socket + mmap 环形缓冲交宿主混音；MCI 走 waveOut 回退 |
| 手柄 | 无 evdev/libinput 生态 | winebus 走宿主 socket 转发（XInput/DirectInput/rumble），`WINEHUA_GAMEPAD_*` 显式开启 |
| DNS 解析 | musl 无 libresolv | dnsapi 内建 resolver fallback（libresolv_musl.c） |
| Windows 版本伪装 | 程序按版本分支行为 | `WINEHUA_WINDOWS_VERSION`（CrossOver 移植） |
| 中文/主题字体 | musl locale 缺位、msstyles 字体顶系统字体 | ntdll 补 LC_ALL 解析 + po 编译；主题删字体行 |
| 相对指针 | 游戏需要原始鼠标运动 | zwp_relative_pointer_v1 接入 + enter 静默校准 |
| 验证程序 | 自动化回归不能依赖人工操作 | winehua_*_smoke 系列以 JSON 状态文件 + 心跳上报结果 |

## 变更总览

- **修改文件 107 个**（上游已存在）：33 个含删除行（合并冲突敏感区）、30 个文本纯追加（低风险）、44 个 aero.msstyles 位图（二进制，无法文本合并）
- **新增文件 48 个**：wineohos.drv 驱动（10）、winewayland.drv 扩展（8）、ntdll ohos 层（6）、win32u 诊断（2）、mciqtz32 音频（3）、winebus 手柄（2）、dnsapi（1）、server（1）、smoke 程序（14）、.gitignore（1）
- **删除行总计 -343**：修改文件大多为"插入式"改动，上游行为保留在 `#else` 分支中；defwnd.c 是最大例外（-168，标题栏重绘重构）

## 修改文件明细（合并冲突敏感区，逐个记录）

### dlls/win32u/vulkan.c：WineHua 私有 swapchain 全实现（+638/-1）
- **为什么存在**：HarmonyOS 宿主只有 OHNativeWindow WSI，没有 Wayland/Vulkan 原生 surface。WineHua 把 swapchain 全部实现到 win32u：surface 句柄用 `0x574853xxxxxxxx` tag 编码 Wayland proxy id（不走 host WSI），swapchain 图像是普通 Venus 图像，present 通过 `dlopen("libvulkan_virtio.so")` 取 `vn_winehua_present` 交给 Host 端 Venus presenter（含 surface_id/serial/deadline 参数）。
- **依赖的上游行为**：`vulkan_surface_create`/`vulkan_swapchain_create` 等 driver_funcs 入口、`get_surface_rect` 等现有几何辅助。是否启用由 `WINEHUA_VULKAN_PRESENT` / `WINEHUA_PRESENT_BACKEND` / `WINEHUA_GRAPHICS_BACKEND=virgl` 环境变量控制（后者是子进程标记 fallback，因为 per-launch 变量不随 Windows 环境传递）。
- **不变式**：`struct surface` 新增 `winehua_private` 字段；`swapchain_from_handle` 对私有句柄的解析路径不能与上游 host swapchain 混用；`vkGetSwapchainImagesKHR` 返回的是私有 VkImage 数组，DXVK 据此直接 present——丢失此实现则 DXVK 在 createSwapchain 即失败。
- **验证方法**：winehua_vulkan_smoke + winehua_d3d11_smoke 全链路；`WINEHUA_DXVK_TRACE_PRESENT_IMAGE=1` 追踪 present 图像。

### dlls/mciqtz32/mciqtz.c：waveOut 回退后端重构（+210/-69）
- **为什么存在**：OHOS 上 DShow 管线不可用（无 DirectShow 解码器），MCI 播放 MP3/WAV 走新增的 waveOut 后端（minimp3 软解码 + WinMM waveOut）。重构了资源释放：抽出 `MCIQTZ_release_graph`/`MCIQTZ_wait_for_thread`，错误路径从逐个 Release 改为统一调用，并支持 DShow 失败后回退 waveOut。
- **依赖的上游行为**：`MCIQTZ_drvOpen`/`mciOpen`/`mciClose`/`mciPlay` 入口与 MCI 状态机；校验提前（原来在 DShow 图构建后才查 `MCI_OPEN_ELEMENT`，现在先查）。
- **不变式**：`WINE_MCIQTZ.backend` 字段（NONE/DSHOW/WAVEOUT）决定 mciClose/mciPlay 走哪套逻辑；`MCIQTZ_waveout_open` 返回 0 才算成功，否则返回 MCI 错误码。
- **验证方法**：winehua_audio_smoke（MP3/WAV 播放 + 音量/暂停/停止）。

### dlls/ntdll/loader.c：导入搜索路径修复 + DXVK/vkd3d overlay 搜索（+150/-4）
- **为什么存在**：三个独立动机：① ARM64 WoW64 入口构建主模块时 load_path 为 NULL，按 Windows 规则应从 PE 自身 NT 名恢复 exe 目录再解析导入（否则 exe 旁的原生 DLL 找不到）；② 进程级 DXVK/vkd3d overlay（Unix 运行时目录，非 C: 前缀）通过内部 `\\??\unix` 命名空间暴露，loader 在常规搜索前按 `WINEHUA_D3D_BACKEND` + `WINEHUA_DXVK_ROOT` 搜索 `\??\unix<root>\x64\<d3d11|dxgi>.dll`，混合会话（`vkd3d_limited_500k`）下另按 `WINEHUA_VKD3D_ROOT` 覆盖 `d3d12.dll`；③ 带 load_path 搜索失败后补搜 `default_load_path`（OHOS WoW64 路径显式传入 app 目录后，vulkan-1.dll 等依赖仍须命中系统路径）。
- **依赖的上游行为**：`build_module`/`find_dll_file` 的搜索顺序、`fixup_imports` 的 load_path 语义。
- **不变式**：overlay 只覆盖 d3d11/dxgi/d3d12 三者，按档位开关生效，WineD3D 与普通应用不受影响；`get_module_path_end` 静态内联声明在 build_module 前，上游若改名会导致编译错而非静默错。
- **验证方法**：DXVK 运行时下 d3d11_smoke 确认加载 `...\dxvk\x64\d3d11.dll` 而非内置；vkd3d 档下 d3d12 同理；无 overlay 时应用 DLL 搜索顺序回归。

### dlls/ntdll/unix/virtual.c：Box64/noexec 兼容（+76/-7）
- **为什么存在**：OHOS 宿主限制：① Box64 包装的 mmap 不支持 `MAP_FIXED_NOREPLACE`（undef 回退 MAP_TRYFIXED/普通 mmap）；② dlopen 不能用绝对路径（改为只传文件名让系统 linker 搜 libs/）；③ 应用数据分区 noexec，`PROT_EXEC` 的 mprotect 和 PE 执行段映射委托给 ohos_virtual.c（`ohos_jit_enable`/`ohos_mprotect_exec`/`ohos_map_exec_section`）；④ Box64 不支持 `MEM_TOP_DOWN`（TEB 分配去掉该标志）；⑤ `virtual_alloc_thread_data` 的 map_view 失败时回退 anon_mmap；⑥ PE 头页无法从 noexec 文件加执行权限——改为 pread 读入匿名页（0b1a274e）；`mprotect` 加 X 失败（页权限创建时确定的假成功场景）再换匿名页兜底（f085bc22，实现在 ohos_virtual.c）。
- **依赖的上游行为**：`map_image_into_view` 的段映射循环（OHOS 分支只换映射函数，大小/截断检查逻辑保留并提前 goto done）。
- **不变式**：`#undef MAP_FIXED_NOREPLACE` 必须在功能测试宏使用前；exec 段必须保证可执行（否则 Box64/Wine 崩溃）。
- **验证方法**：完整 make + 启动 wineboot/explorer 无段错误；`ohos_map_exec_section` 失败有 ERR 日志。

### dlls/winewayland.drv/wayland_surface.c：app_id 后缀 / desktop 模式 geometry / min-max / modal 上报（+78/-8）
- **为什么存在**：① xdg_toplevel app_id 追加 class 后缀（`Shell_TrayWnd`→`.taskbar`、`#32769`→`.desktop-shell`），让宿主合成器无需几何启发式即可识别任务栏/桌面；② `WINEHUA_DESKTOP_MODE` 下 window_geometry 转发真实屏幕坐标（范围检查 ±32768）；③ `wayland_surface_update_min_max`（ohos 扩展文件）在 reconfigure 时更新 min/max 约束；④ `wayland_surface_ensure_contents` 加 `window_contents` 参数——保留最近 GDI 标题栏/边框内容再 attach，修复 Vulkan 首帧 present 后的死锁与装饰丢失；⑤ 模态关系上报钩子（205ba344）：把 wine 探测到的模态对话框关系经私有协议通知宿主（见 waylanddrv.h 与 modal.c 条目）。
- **依赖的上游行为**：`wayland_surface_reconfigure` 的角色分派、`xdg_surface_set_window_geometry` 语义（上游传 rect.left/top，OHOS 桌面模式改传屏幕坐标）。
- **不变式**：函数签名变化（`ensure_contents`）会连带 window.c 调用点；上游若同期修改需三方合并。
- **验证方法**：桌面模式下任务栏/桌面窗口 app_id 正确；Vulkan 客户端 present 不挂起；窗口拖动/缩放几何正确。

### dlls/ntdll/unix/file.c：无 symlink 盘符映射（+60/-1）
- **为什么存在**：OHOS NAPI 沙箱无 symlink()，`dosdevices/c:` 不存在。在 `get_drives_info`（dev/ino 缓存）、`get_dos_device`、`nt_to_unix_file_name_no_root` 三处用 `ohos_drive_unix_path()`（ohos_file.c）硬编码映射盘符；`unix_to_nt_file_name` 容忍 `STATUS_OBJECT_PATH_NOT_FOUND` 并让 `\\?\unix` 回退成功。
- **依赖的上游行为**：`find_drive_nt_root` 的盘符解析、`get_default_drive_device`。
- **不变式**：`lstat` 先探真实路径，失败才走硬编码；`get_nt_and_unix_names` 中 unix 路径加 null 终止（修复潜在越界）。
- **验证方法**：explorer My Computer 可见 C:/Z:；Z: 指向用户 Download 目录。

### dlls/ntdll/unix/loader.c：OHOS 路径初始化 + broker 启动 wineserver（+56/-1）
- **为什么存在**：① 沙箱目录布局不同于发行版——`WINEDATADIR`/`WINEBINDIR`/`WINEUNIXDIR` 环境变量覆盖 data_dir/bin_dir/dll_dir（打包路径 `files/wine/share`、`files/wine/bin` 等）；② wineserver 是 libwineserver.so，无法 posix_spawn，改用 Process Broker 请求宿主 `StartNativeChildProcess`，spawn 前扫描 `.wineserver/<host>/socket` 防重复启动（轮询就绪/残留确认）；③ `pre_exec` 返回 0、`reexec_loader` 跳过默认 reexec（Pad 无 preloader/execve）。
- **依赖的上游行为**：`init_paths` 的相对路径推导、`start_server` 的启动协议。
- **不变式**：`#ifdef __OHOS__` 分支完整替换 fork 路径，`#else` 保留上游实现；wineserver socket 就绪轮询超时 5s。
- **验证方法**：冷启动 wineboot 成功、wineserver 由宿主进程拉起；stamp 机制下重编后单进程存活。

### dlls/mmdevapi/client.c：ohos 后端共享模式格式转换（+34/-14）
- **为什么存在**：OHOS 后端固定 48kHz 立体声 s16 混音，客户端常见格式（44.1kHz 等）会被 `validate_fmt`/`IsFormatSupported` 拒绝。以驱动名 `wineohos.drv` 判断（**注意：此文件是 PE 编译目标，`__OHOS__` 未定义，只能按模块名判断**），让后端接受共享模式格式并内部归一化；`AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM` 短路 GetMixFormat 调用（后端不支持该调用）。
- **依赖的上游行为**：`validate_fmt` 的 compatible 参数语义、`stream_init`/`client_IsFormatSupported` 的 mix_fmt 比较流程。
- **不变式**：`backend_allows_shared_mode_conversion()` 仅对 wineohos.drv 生效，pulse/alsa 等走上游路径。
- **验证方法**：winehua_audio_smoke 以 44.1kHz/16bit 客户端格式播放成功。

### dlls/win32u/opengl.c：EGL 诊断 + 可覆盖 libEGL 路径（+36/-3）
- **为什么存在**：① OHOS 设备无标准 `libEGL.so`，`WINEHUA_EGL_LIBRARY_PATH` 覆盖 dlopen 目标（`winehua_preload_guest_egl_deps` 预载 guest EGL 依赖）；② `winehua_opengl_diag` 输出 eglGetConfigs 失败/像素格式计数/GL/ES renderable 分类等诊断（`WINEHUA_OPENGL_DIAG` 控制），帮助排查 VirGL EGL 无 EGL_OPENGL_BIT 的场景。
- **依赖的上游行为**：`egl_init` 的 SONAME_LIBEGL 默认值、`egldrv_init_pixel_formats` 的过滤逻辑（仅统计，不改行为）。
- **验证方法**：graphics_smoke 在真实设备跑通 + 诊断日志可见。

### dlls/shell32/shlfolder.c：desktop.ini CLSID 解析容错（+28/-1）
- **为什么存在**：desktop.ini 的 CLSID 不可实例化（未注册/损坏）时绑定失败导致 explorer 无法打开该文件夹。修改后：解析失败仅告警、绑定失败回退默认文件系统处理器。
- **依赖的上游行为**：`SHELL32_GetCustomFolderAttributeFromPath` + `SHELL32_CoCreateInitSF` 流程。
- **验证方法**：构造含假 CLSID 的 desktop.ini 文件夹，explorer 仍可浏览。

### dlls/ntdll/unix/server.c：dosdevices 无 symlink 容错 + fd 存活检测（+24/-2）
- **为什么存在**：① `setup_config_dir` 中 symlink 创建失败降级为警告（不再 fatal），`fd_cwd` 回退打开 `drive_c` 目录；② `server_init_process` 中 fd 传递期间 wineserver 可能退出，`recv(MSG_PEEK|MSG_DONTWAIT)` 检测存活。
- **依赖的上游行为**：dosdevices 布局约定（无 symlink 时 ntdll/mountmgr 的 ohos 映射补位）。
- **验证方法**：新 prefix 创建不报错；wineserver 异常退出时子进程能感知而非悬挂。

### dlls/win32u/sysparams.c：1280x800 兼容虚拟模式 + 分辨率模拟（+83/-3）
- **为什么存在**：(1) 720p 宽屏设备上 1024x768 不可用（高度不够）——新增 `{1280,800}` 虚拟模式 + `virtual_mode_fits_compatibility_envelope` 包络逻辑：单模式宿主下保持 1280x800 逻辑包络，宿主编排缩放。(2) 测试/适配需要在不改宿主的前提下模拟其它分辨率——`WINEHUA_SIMULATE_RESOLUTION` 按进程模拟 ChangeDisplaySettings（b7f43d4b）。
- **依赖的上游行为**：`add_virtual_mode`/`get_screen_sizes` 模式表与过滤条件（只在函数内加判断，不改表结构）。
- **验证方法**：1080p 输出下列出 1024x768；全屏 3D 应用 mode 切换正确；设 SIMULATE_RESOLUTION 后 EnumDisplaySettings 返回模拟值。

### dlls/winewayland.drv/opengl.c：readback 驱动钩子（+17/-3）
- **为什么存在**：`WINEHUA_WAYLAND_READBACK` 启用时用 opengl_readback.c 的包装 driver_funcs（p_init_egl_platform/p_surface_create/p_make_current）替换默认，把 GL 帧经 glReadPixels 回读 + shm commit 交给宿主合成器（零拷贝 present surface 页为快速路径）。`egl`/`funcs`/`egl_config_for_format` 从 static 改为导出供 readback 用。
- **依赖的上游行为**：`WAYLAND_OpenGLInit` 的 driver_funcs 包装链。
- **不变式**：未设环境变量时行为与上游完全一致。
- **验证方法**：graphics_smoke 在 readback 模式 FPS/像素正确；零拷贝页计数 `winehua_gl_zero_copy_presents`。

### dlls/winewayland.drv/window.c：桌面模式窗口 win_data 创建（+13/-5）
- **为什么存在**：桌面模式（`WINEHUA_DESKTOP_MODE=1`）下所有窗口（含桌面窗口/HWND_MESSAGE）都需创建 win_data 作为 subsurfacing 根；独立窗口模式保持上游"仅父窗口可见"过滤。
- **依赖的上游行为**：`wayland_win_data_create` 的 GA_PARENT 过滤逻辑（整体保留在 `#else` 语义内）。
- **验证方法**：桌面模式 explorer 桌面可见；独立模式窗口行为回归。

### dlls/winewayland.drv/window.c：装饰浮层的受管判定（f6492f84827，+12/-0）
- **为什么存在**：程序常以「内容窗口 + 外扩一圈的装饰窗口」自绘阴影/边框（企业微信登录窗的 `PerryShadowWnd`：owner=内容窗口，style=`96880000` 含 `WS_POPUP|WS_SYSMENU`，exstyle=`080800a0` 含 `WS_EX_LAYERED|NOACTIVATE|TOOLWINDOW|TRANSPARENT`）。上游 `is_window_managed` 的"popup with sysmenu == caption"判据把它判为受管，于是调用处 `if (!(managed = is_window_managed(...)) && surface) toplevel = owner_hint;` 的 `&&` 短路掉 owner 关系，装饰窗口各自成为独立 xdg_toplevel。Windows 下靠 DWM 按 z 序合成、X11 下靠 WM 识别 `WS_EX_TOOLWINDOW`，观感都是一体；**wayland 的 xdg_toplevel 没有工具窗口概念，winewayland 也从不调用 `xdg_toplevel.set_parent`**，依附关系无处表达 → 每个装饰层多出一个独立宿主窗口（企业微信表现为纯黑窗盖在主界面上）。
- **语义修改**：对「显式设了 owner」且「`WS_EX_LAYERED`+`WS_EX_NOACTIVATE`+`WS_EX_TOOLWINDOW` 三件套齐全」的窗口，`is_window_managed` 返回 FALSE —— 受管判定的语义由「像窗口就是独立窗口」改为「程序声明自己不参与窗口管理（纯装饰浮层）就依附到 owner」。返回 FALSE 之后走的是上游既有的 owner_hint → `wl_subsurface` 通路，不是新增路径；限定"有 owner"是为了不误伤无主的浮动面板。
- **依赖的上游行为**：`wayland_win_data_create_wayland_surface` 的 role 分派（`toplevel_surface` 非空即 `SUBSURFACE`）；win32u 的 `owner_hint` 来源（`dlls/win32u/window.c:2405` 取 `GW_OWNER`，为空时按窗口左上角位置推断）。
- **影响面**：`managed` 在 winewayland 后端只有三个消费点 —— `wayland_keyboard.c:818`（键盘 focus 进窗口时是否设前台）、`is_managed()` → `has_owned_popups()`（反向影响 owner 窗口自身的受管判定）、`conf->managed` → `surface->window`（`wayland_surface_config_is_compatible` 不比较该字段，不会触发额外 configure）。
- **合并注意**：`winex11.drv` 的同名函数保持上游原样，两个后端在这一点上分叉。
- **验证方法**：打点比对 `managed` 由 1 变 0、`owner_hint` 正常传入、`[WL-STAT] toplevels` 由 2 降到 1；企业微信多窗口模式黑窗口消失（目视确认）。
- **已知边界（均未验证）**：① 判据插在函数最前，优先级高于上游全部八条判据（含 `WS_EX_APPWINDOW` 这种"程序显式要求作为独立窗口出现"的声明）；企业微信窗口带 `WS_POPUP`，判据挪进 popup 分支效果相同而影响面更小，当前保留最前位置。② owner 窗口的 `has_owned_popups` 依据随之减少（`WeChatLogin` style=`16080000`，本就无 caption/thickframe，受管依据单薄），实测登录窗显示正常，但未专门验证。③ tooltip、输入法候选窗、拖拽半透明预览若带齐三件套且有 owner 会同样走子表面，未实测。

### dlls/win32u/defwnd.c + dlls/aero.msstyles/aero.rc + dlls/uxtheme/window.c：标题栏按钮主题化与尺寸统一（93c7c58b 等，defwnd.c +88/-168 为全树最大删除块）
- **为什么存在**：① msstyles 的 `[SysMetrics]` 字体经 `SPI_SETNONCLIENTMETRICS` 写注册表，绕过 locale 映射导致中文标题渲染退化——aero.rc 删除主题字体行（每行 INI 字符串要独立引号，wrc 需 `-u`）；② 高 DPI 下标题栏按钮宽高来自两个独立度量，成 34×46 窄高矩形——defwnd.c 新增 `caption_button_size()`/`get_caption_button_rect()` 统一正方形，绘制与 hit test 共用同一尺寸函数；③ Caption 颜色改浅适配深色主题；④ 还原窗口最外 1px 圈缺失导致黑边（3ce65ee4）。44 个 bmp 二进制随主题更新。
- **合并注意**：defwnd.c 重构了 NC 绘制路径，是合并冲突风险最高的一块。
- **验证方法**：中文系统字体渲染正常；高 DPI 下按钮正方形、点击区域与绘制一致。

### dlls/ntdll/version.c：Windows 版本伪装（65b5128d，+42/-12）
- **为什么存在**：部分程序按 Windows 版本分支行为，固定伪装不够用。从 CrossOver 移植 `WINEHUA_WINDOWS_VERSION` 环境变量按进程覆盖版本号。
- **验证方法**：设不同值后 `winver`/`GetVersionEx` 返回对应版本。

### dlls/win32u/window.c：最小化窗口豁免 present_rect 替换（037984bd，+17/-1）
- **为什么存在**：最小化窗口的可见 rect 退化为图标位，present 用它裁剪会拿不到画面。
- **做法**：最小化窗口不走 present_rect 替换逻辑。
- **验证方法**：最小化再还原后画面恢复。

### dlls/winewayland.drv/wayland_pointer.c：相对模式 enter 静默校准（752ef4ae，+44/-1）
- **为什么存在**：进入相对指针模式时 `zwp_relative_pointer_v1` 的 enter 事件会跳一次绝对位置，游戏内表现为视角突跳。
- **做法**：相对模式下对 enter 事件做静默校准（不注入运动量），后续 motion 才按相对量注入。
- **验证方法**：红警 2 / PAL2 类游戏进入相对模式首帧视角不跳。

### tools/makedep.c：install 命令去重/断言放宽（+12/-1）
- **为什么存在**：`find_install_command` 跳过带 dest 的条目去重（避免打包期 install 规则冲突）；`output_install_commands` 的 `files.count==1` 断言先打印上下文再触发，便于定位 OHOS 打包脚本的 install 命令问题。
- **依赖的上游行为**：install_commands 数组的匹配/去重语义。
- **验证方法**：完整 make 打包通过；错误场景有可读输出而非裸断言。

### dlls/setupapi/queue.c：needmedia 重试上限（+10/-1）
- **为什么存在**：无光驱的 OHOS 设备上 `SetupCommitFileQueueW` 的 SOURCE_MEDIA 请求会无限循环弹"插入磁盘"阻塞安装。加 10 次重试上限后放弃（**PE 目标，无 `__OHOS__`，无条件生效**）。
- **验证方法**：安装含缺失 media 的驱动/组件不再挂死。

### dlls/winewayland.drv/waylanddrv.h：结构扩展（+25/-1）
- **为什么存在**：`struct wayland_surface` 增加 min/max 尺寸字段；`wayland_surface_ensure_contents`、`wayland_shm_buffer_copy` 签名声明（配合 ohos min-max 与装饰保留）；`winehua_toplevel` 私有协议对象与 `winehua_modal_owner` 字段、`winehua_modal_update/release` 声明（模态对话框上报，205ba344）。
- **验证方法**：编译一致性即可；模态上报见 modal.c 条目。

### dlls/winevulkan/vulkan_thunks.c：surface/swapchain thunk 改走 vk_funcs（+6/-6）
- **为什么存在**：`vkGetPhysicalDeviceSurfacePresentModesKHR`/`SurfaceSupportKHR`/`vkGetSwapchainImagesKHR` 的 thunk 原本直接解包 host 句柄调用 host 函数——私有 surface/swapchain 没有 host 对象，改为经 `vk_funcs->p_*` 转发（win32u 私有实现注册的入口）。
- **依赖的上游行为**：thunk 生成模板（make_vulkan 已加对应 driver funcs）。
- **验证方法**：DXVK swapchain 创建/枚举/acquire 全流程。

### dlls/opengl32/unix_wgl.c：extensions 索引修复（+1/-1）
- **为什么存在**：上游 bug：`client->extensions[i] = TRUE`（i 是解析后索引）应为 `extensions[i]` 的值索引，extension_count 超过数组大小时会越界写/错位。
- **验证方法**：多扩展 GL 上下文 WGL 功能检测正确。

### dlls/mmdevapi/main.c：ohos 驱动注册 + 卸载顺序（+6/-4）
- **为什么存在**：① 驱动列表前缀加 `ohos`（OHOS 后端）；② `process_detach`/`main_loop_stop`/unload 仅在 `module_unixlib` 存在时调用（修复 midi 驱动未加载时的空指针路径）。
- **验证方法**：DllMain 卸载无崩溃；音频驱动列表含 ohos。

### dlls/winewayland.drv/window_surface.c：shm_buffer_copy 导出（+3/-3）
- **为什么存在**：`wayland_shm_buffer_copy` 从 static 改外部符号（window.c 在持锁状态下直接传当前 GDI buffer，避免递归加锁）。
- **验证方法**：present 后窗口装饰/首帧不丢。

### include/wine/vulkan_driver.h：driver 版本与 funcs 扩展（+4/-1）
- **为什么存在**：`WINE_VULKAN_DRIVER_VERSION` 47→48；`vulkan_funcs` 增加 3 个私有 swapchain 入口（PresentModes/Support/GetSwapchainImages）。版本号必须与 winevulkan 一致，否则 driver 拒绝加载。
- **验证方法**：winevulkan/winewayland 双向加载成功。

### 低风险追加（仅 +行，无删除，合并时直接重放）— 30 个
- **dlls/ntdll/unix/process.c**：`spawn_process` 走 `ohos_broker_spawn_child`（NCP 不支持嵌套子进程，转发宿主）、`fork_and_exec` 直接返回失败（无 execve）、`WINEHUA_PROCESS_EXIT_TELEMETRY` 退出遥测
- **dlls/ntdll/unix/server.c 同目录其余**、**dlls/mountmgr.sys/unixlib.c**：`get_dosdev_symlink` 无 symlink 时映射 z:→`/storage/Users/currentUser`、c-y→`$WINEPREFIX/drive_X`
- **dlls/win32u/driver.c**：services.exe 无法 PnP 时 `WAYLAND_DISPLAY` 存在即直接 `NtUserLoadDriver(winewayland.drv)`；null driver 用 `nulldrv_CreateWindow`
- **dlls/win32u/freetype.c**：无 fontconfig 时扫描 `/system/fonts`（HarmonyOS 字体）
- **dlls/win32u/winstation.c**：`WINEHUA_DESKTOP` 环境变量覆盖默认 desktop 名
- **dlls/winewayland.drv/vulkan.c**：私有 surface tag + present 使能（与 win32u 同源逻辑）：present 模式返回 VK_TRUE、映射 win32_surface 扩展、不传 host WSI 扩展
- **dlls/winewayland.drv/wayland.c**：modal 协议注册与绑定（配合 modal.c）
- **dlls/winevulkan/loader.c**：`vkGetDeviceProcAddr` 对 8 个 swapchain 入口绕过 host 查询；`is_device_extension_supported`/`vkEnumerateDeviceExtensionProperties` 恒暴露 `VK_KHR_swapchain`（win32u 侧再剥离）
- **dlls/winevulkan/vulkan.c**：`WINEHUA_DXVK_TRACE_CAMERA` 命令缓冲映射追踪
- **dlls/winevulkan/make_vulkan**：`USER_DRIVER_FUNCS` 增加 3 个入口（配合 thunk 修改）
- **dlls/winebus/unixlib.c / bus_ohos.c / 无线手柄相关（587f7239/755816e0/7005b686 三连）**：手柄经宿主 socket 转发——DirectInput + XInput + rumble，`WINEHUA_GAMEPAD_ENABLE/SOCKET/MODE` 控制，**默认保持 Wine 原行为、显式开启才生效**
- **libs/vkd3d/.../hlsl_codegen.c**：SM4 half 类型算术（NEG/RCP/ADD/DIV/DOT/比较/MAD/MIN/MAX/MUL）——vkd3d 缺 half 支持，配合 d3dcompiler 修复
- **dlls/d3dcompiler_43/tests/hlsl_d3d11.c**：`test_sm4_half_arithmetic` 回归测试；**compiler.c**：Compile2 返回值 TRACE
- **dlls/mciqtz32/mciqtz_private.h**：`mciqtz_backend`/`wave_state` 枚举与 waveOut 字段
- **dlls/dnsapi/Makefile.in**：登记 libresolv_musl.c
- **configure.ac / 各 Makefile.in（ntdll、win32u、winewayland、server、wineohos.drv）**：注册新模块/源文件
- **loader/wine.inf.in**：mmdevapi.dll 加入 self-register 段（注册 WASAPI COM 类，见文末说明）
- **programs/explorer/systray.c**：taskbar rect 为空时回退底部（workarea 计算在桌面模式的修复）
- **server/directory.c**：`__OHOS__` 下 init_directories 时在 `\??\` 创建 C:/Z: 命名事件对象（无 symlink 时让 GetLogicalDrives 能枚举盘符）
- **server/process.c / ntdll/unix/server.c 部分**（a3f725d0）：bootstrap 期可观测性打点

**注**：`dlls/ntdll/unix/env.c`（+86/-1，含 PEB `DllPath` 从 `WINEDLLPATH` 构建 + bootstrap 打点）与 `dlls/mciqtz32/Makefile.in`（+2/-1）含删除行，归入 33 个冲突敏感区，不列在本节。

## 新增文件明细（按目录/主题归类）

### dlls/wineohos.drv/：鸿蒙音频/MIDI 驱动（10 个文件）
- **职责**：mmdevapi 的 `ohos` 后端 unixlib。`ohos.c`（+2366）实现流管理（48kHz s16 固定混音、capture/playback、SCM_RIGHTS fd 接收）；`ohos_audio_client.c/h` + `audio_ipc_protocol.h` 是 Unix socket + mmap 环形缓冲的 IPC 客户端（与宿主进程内音频服务通信，沙箱内不能直接调 OHOS 音频 API）；`ohos_midi.c/h` 是 MIDI 软合成（TSF/TML 合成 PCM 走同一音频通道）；`tsf.h`（TinySoundFont，SoundFont2 合成器）/`tml.h`（TinyMidiLoader）为第三方单头库（MIT/ZLIB 许可，上游无对应物）；`wineohos.spec`/`Makefile.in` 为构建配套。
- **验证方法**：winehua_audio_smoke（WASAPI/MME 播放 + MIDI）；hilog 抓 `ohosaudio` channel。

### dlls/ntdll/unix/ohos_*：ntdll 鸿蒙适配层（6 个）
- **职责**：`ohos_broker.c/h`（+318）——Process Broker 客户端：Unix socket 请求宿主主进程创建子进程（wineserver、wine 子进程），SCM_RIGHTS 传 socket fd，含 wineserver socket 扫描/就绪轮询；`ohos_file.c/h`——`ohos_drive_unix_path` 盘符→Unix 路径映射（替代 symlink）；`ohos_virtual.c/h`——`ohos_jit_enable`/`ohos_mprotect_exec`/`ohos_map_exec_section`（noexec 数据分区的可执行内存/段映射方案）。
- **验证方法**：wineboot 冷启动（broker 链路）；explorer 读 Z: 文件（file）；运行含 JIT 的应用（virtual）。

### OpenGL 诊断与 readback：win32u/opengl_diag.c/h + winewayland.drv/opengl_diag.c/h、opengl_readback.c/h（8 个）
- **职责**：`opengl_diag`（win32u 113 行 + wayland 184 行）——`winehua_opengl_diag`/`winehua_wayland_diag` 格式化诊断输出 + GL stall watchdog（`WINEHUA_OPENGL_DIAG`/`WINEHUA_GL_STALL_DIAG` 控制）+ `winehua_preload_guest_egl_deps`；`opengl_readback.c`（+620）——GL 帧回读管线：零拷贝 present surface 页（magic/版本化共享页）快速路径 + glReadPixels 回读 + shm commit，提交/释放/丢帧计数导出。
- **验证方法**：graphics_smoke 两种路径各跑一次对比；`WINEHUA_WAYLAND_READBACK=1` 时看 `winehua_gl_*` 计数。

### dlls/winewayland.drv/wayland_surface_ohos.c/h：桌面模式 surface 扩展（2 个）
- **职责**：`wayland_surface_update_min_max`（按窗口 style 设置 xdg_toplevel min/max）、desktop 坐标范围常量（±32768）。
- **验证方法**：桌面模式窗口缩放被合成器约束；standalone 模式不触发。

### dlls/mciqtz32/mciqtz_waveout.c/h、minimp3.h（3 个）
- **职责**：waveOut 后端：minimp3（+1865，第三方单头 MP3 解码器）软解 MP3/WAV 为 PCM，经 waveOut 播放，含循环/音量/位置查询（对应 MCI 命令）；`mciqtz_waveout.c`（+639）实现 open/play/close/seek/set/status。
- **验证方法**：audio_smoke 播放 MP3/WAV 出声、暂停/恢复/音量正确。

### server/musl_compat.c（1 个）
- **职责**：musl libc 缺失 `epoll_pwait2` 时的弱符号 stub（ENOSYS），使 wineserver 能在 OHOS（musl）编译。
- **验证方法**：OHOS 上 wineserver 启动、event loop 正常。

### dlls/winewayland.drv/modal.c + winehua-toplevel.xml（205ba344，2 个）
- **职责**：模态对话框探测与上报。wine 侧探测「禁用 owner 的对话框」关系，经私有 Wayland 协议 `winehua-toplevel`（modal_update/modal_release 请求）通知宿主合成器，供宿主编排模态窗口的层级与输入路由。
- **验证方法**：打开模态对话框，宿主日志出现 modal_update；关闭后 modal_release。

### dlls/dnsapi/libresolv_musl.c（08cbdd64，+670）
- **职责**：musl 无 libresolv，dnsapi 的 resolver 调用（res_query/dn_expand/ns_get16/ns_get32 等）内建 musl 移植实现，DNS 解析不再依赖系统库。
- **验证方法**：`nslookup` 类程序或应用内域名解析成功。

### dlls/winebus/bus_ohos.c/h 等（2 个，587f7239/755816e0/7005b686）
- **职责**：手柄宿主转发通道：连接宿主的 gamepad socket，把设备插拔/轴/按键/震动（rumble）映射成 DirectInput 与 XInput 设备。`WINEHUA_GAMEPAD_ENABLE` 显式开启才生效（默认走 Wine 原生 evdev 路径）。
- **验证方法**：宿主连接手柄后 wine 程序可枚举并使用；震动回传。

### programs/winehua_*_smoke + winehua_keep + winehua_dinput_probe + smoke_protocol.h（14 个）
- **职责**：自研验证程序（非上游测试框架，以 JSON 状态文件 + 心跳报告结果）：
  - `winehua_d3d11_smoke/main.c`（+5862）：DXVK Legacy D3D11 冒烟——只走公共 D3D11/DXGI API，覆盖设备创建/feature level 契约、BC 纹理、descriptor identity、RGBA pattern、MSAA resolve、sampler pair、subresource、MRT gbuffer、Heaven mini 管线（cube/comparison sampler/pass probes）、stencil query，且校验加载模块必须是 DXVK 而非 WineD3D（`module_is_native`/`dxvk_modules_loaded`）
  - `winehua_vulkan_smoke/main.c`（+1188）：离屏 Vulkan（刻意无 WSI）——验证 winevulkan → x86_64 loader → Mesa Venus 链：物理设备/队列、buffer copy、image clear、storage/sampled 读写
  - `winehua_graphics_smoke/main.c`（+873）+ `graphics_runtime_env.h`：Win32 OpenGL 窗口 + 3D 动画 + FPS 上报（验证 VirGL GL 链路）
  - `winehua_audio_smoke/main.c`（+486）：MP3/WAV 播放 + MIDI（验证音频全链路）
  - `winehua_keep/main.c`（+70）：桌面保活——加入 shell 虚拟桌面持有隐藏窗口并吞掉 WM_CLOSE，阻止 wineserver 1 秒超时销毁桌面（替代 `--no-auto-close`）
  - `winehua_smoke_protocol.h`（+187）：smoke 公共选项/结果序列化（automation/offscreen/present/seconds/result_path）
- **验证方法**：`winehua_<x>_smoke --automation --seconds N` + 检查 result JSON 字段。

### .gitignore（1 个）
- **职责**：忽略 wine 构建产物（`__build__` 等 OHOS 打包目录）。
- **验证方法**：`git status` 干净。

## 合并策略备注

1. **新增文件先行**：48 个新增文件（ohos 层、smoke、单头库）与上游无冲突，直接合入即可，是全部工作量的主体
2. **修改文件 107 个**：33 个含删除行的按本清单逐条 review；30 个文本纯追加的低风险（其中 `__OHOS__` 守卫型可在合并时评估是否保留）；44 个 bmp 二进制随 aero.msstyles 主题整体取
3. **可能已上游化的点**：`hlsl_codegen.c` half 支持与上游 vkd3d 演进可能重复（上游 vkd3d 后续版本有 half 处理）；`opengl32/unix_wgl.c` extensions 索引修复上游可能已修——合并前先 diff 上游对应文件
4. **`__OHOS__` 守卫覆盖**：ntdll（env/file/loader/process/server/virtual）、win32u（driver/freetype/winstation）、winewayland（wayland_surface）、mountmgr、server/directory；PE 目标文件（mmdevapi/client.c、setupapi/queue.c）无宏守卫、按驱动名或无条件生效，review 时注意

[待确认]：tools/makedep.c 两处修改（install 命令去重/断言放宽）的触发场景——`scripts/` 下打包脚本没有任何 `make install`/`DESTDIR` 调用，仓库内无对应证据；推测是打包期对接 `make install DESTDIR=` 布局时遇到多文件 install 条目。wine.inf.in 增加 mmdevapi.dll 的意图已确认：加在 `[RegisterDllsSection]`（flag 1 = self-register），wineboot 处理 inf 时调 `DllRegisterServer` 注册 MMDeviceEnumerator 等 COM 类，否则 WASAPI 的 `CoCreateInstance` 落空。
