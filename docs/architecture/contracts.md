# 跨仓库的约定（Cross-Fork Contracts）

> 最后核实：2026-09-24
> 记录 wine / mesa / virglrenderer 三个 submodule 之间"两边各写一份、内容必须
> 一模一样"的配合约定。**改任何一处之前，先来查这里。**

## 0. 为什么会有这些东西

wine、mesa、virglrenderer 是三个互不相干的独立项目，各有各的上游（WineHQ /
Mesa / freedesktop），以后要各自合并上游更新。所以我们**不能**在它们之间共享
代码 —— 共享一个头文件，合并上游时就会把别的项目的东西卷进来、天天冲突。

不共享代码，又得协同工作，唯一的办法就是：**两边各自实现一份说好的协议**。
这就是本文档记录的东西。这些协议跟项目内普通接口最大的区别是：

- 项目内接口有编译器把关（头文件改了全员重编译，签名不对直接编译失败）；
- 仓库之间的协议**没有编译器**，两边写错一个字都不会有人提醒，画面就悄悄坏了。

所以每条协议都带了"认不出你就报错"的机制（魔数、版本号、长度、序列号）——
这也是出问题时最先检查的地方。

## 1. 协议一览

| # | 协议 | 谁跟谁 | 通过什么传递 | 对不上怎么发现 |
|---|------|--------|-------------|---------------|
| C1 | present surface 共享内存页 | wine ↔ mesa | 一个共享内存文件 | 魔数 `0x57535053` + 版本号 |
| C2 | `VCMD_WINEHUA_PRESENT` 命令 | mesa ↔ virglrenderer | vtest socket | 版本号（当前 4） |
| C3 | `WINEHUA_*` 环境变量 | 主仓库 ↔ wine/mesa | 环境变量 | 没有，读不到就走默认值 |
| C4 | `.ready` 零拷贝标记 | 主仓库 ↔ wine | 一个标记文件 | 文件在不在 |
| C5 | present 回调函数 | virglrenderer ↔ 主仓库 | dlopen/dlsym | 找不到符号就打印 WARN |
| C6 | virgl IPC 协议 | 主仓库 ↔ virgl_child | OH_IPC | 魔数 `0x57484950` + 版本 9 |
| C7 | Vulkan 私有 surface 标记 | winewayland.drv ↔ win32u（wine 仓库内部） | 句柄高位 tag | 高位 tag `0x574853` 匹配 |
| C8 | `vn_winehua_present` 函数 | win32u ↔ mesa venus | dlopen/dlsym 直接调用 | 符号缺失打印 WARN |
| C9 | `VCMD_WINEHUA_VK_PRESENT` 命令 | mesa venus ↔ virglrenderer | vtest socket | 版本号（当前 1） |
| C10 | Vulkan 呈现 / 设备释放回调 | virglrenderer ↔ 主仓库 | dlopen/dlsym | 找不到符号就打印 WARN |

C1–C5 是 OpenGL 渲染链路用的（背景见 [opengl-virgl.md](opengl-virgl.md)），
C6 是主进程和 virgl 子进程之间的控制通道，C7–C10 是 Vulkan（DXVK）链路用的
（背景见 [PHASE2_DXVK_STATUS_MEMO.md](../archive/PHASE2_DXVK_STATUS_MEMO.md)）。
DXVK 本身不参与这些协议 —— 它只通过标准 Vulkan 接口跟 wine 打交道，见 [§12](#12-dxvk-为什么不用参与)。

---

## 2. C1: present surface 共享内存页（wine ↔ mesa）

**它是干什么的**

Windows 程序调 SwapBuffers 时，wine 的 `opengl_readback.c` 要处理这帧。zero-copy
模式下帧内容不再走 Wayland 协议传像素，但 host 侧（virglrenderer）得知道"这帧
属于哪个窗口"才能送到对的屏幕上。窗口编号（wl_surface id）由 wine 写进一个
共享内存文件，同进程内的 guest Mesa 在换帧时读出来用。wine 和 Mesa 在同一个
进程里（用户程序进程），但它们是两个互不相属的库，没有共享头文件，所以用
文件传。

**两边怎么约定的**

- 文件名：`$TMPDIR/winehua_present_surface_<pid>.shm`（pid 是 guest 用户进程的 pid）
- 内容 16 字节：

| 偏移 | 字段 | 含义 |
|------|------|------|
| 0 | magic（u32） | 固定 `0x57535053`（"WSP"），用来识别这是我们的文件 |
| 4 | version（u32） | 固定 1，结构体变过就要 +1 |
| 8 | surface_id（u32） | 当前要呈现的 wl_surface id，没有就是 0 |
| 12 | reserved（u32） | 占位，恒 0 |

- 时序：wine 在 swap 前写入 surface_id（`__ATOMIC_RELEASE`），swap 后清 0；
  Mesa 用 `__ATOMIC_ACQUIRE` 读，读到非 0 就认为有帧要呈现。

**代码在哪儿**

| 端 | 位置 | 干什么 |
|----|------|--------|
| wine（写） | `thirdparty/wine/dlls/winewayland.drv/opengl_readback.c:54-63` | 结构体定义 |
| | `opengl_readback.c:75-119` | 创建共享页（75-102）、写 id / 清 0（104-119） |
| | `opengl_readback.c:349-369` | zero-copy 分支：swap 前后包住 begin/finish |
| mesa（读） | `thirdparty/mesa/src/gallium/winsys/virgl/vtest/virgl_vtest_socket.c:47-82` | 打开文件、校验 magic/version，不对就丢掉 |
| | `virgl_vtest_socket.c:783-796` | `winehua_vtest_get_present_surface_id`，换帧时调 |
| 用在哪 | `virgl_vtest_winsys.c:688-697` | 读到非 0 → 发 C2 的 present 命令 |

**出问题什么样**

| 现象 | 排查 |
|------|------|
| 文件打不开 / magic 对不上 → Mesa 读到的永远是 0 → 不发 present 命令 → 自动走老路的像素读回（慢，但画面是对的） | 日志搜 `[VTEST-FRONTBUFFER]`，看 `surface=0` |
| 两边结构体不一致（只改了 wine 忘了 mesa）→ 读到错位的字节 → 帧送错窗口，没有编译报错 | 对比两端结构体定义；看 `[VIRGL-PRESENT]` 日志里 surface 和窗口的对应关系 |
| wine 切了 zero-copy 但 Mesa 没读到 → 帧只 swap 不上屏 → 白屏 | 先查两边的 TMPDIR 是否一致 |
| 有帧要呈现但 host 侧没人消费 → swap 正常但没画面 | `[VIRGL-ZC][MAIN] target missing` / `[VIRGL-PRESENT] blit=FAIL` |

---

## 3. C2: `VCMD_WINEHUA_PRESENT` 命令（mesa ↔ virglrenderer）

**它是干什么的**

guest 通知 host："这个资源（host 侧的纹理）已经画好了，请把它显示到窗口 Y"。
这是"命令路径和显示路径分离"的关键：呈现不再把像素拷回去，只传**资源编号**和
**窗口归属**。host 侧验证后把纹理交出去（C5 回调），由宿主决定怎么上屏。
回复里带一个 next deadline —— host 告诉 guest "下次最早什么时候再来"，guest
据此节流，别把 host 撑爆。

**两边怎么约定的**

走 vtest socket，信封 `[命令长度][命令编号]` + 14 个 u32 的命令 + 4 个 u32 的回复。
字段定义在 `thirdparty/mesa/src/virtio/vtest/vtest_protocol.h:88-111`
（virglrenderer 侧有镜像副本 `vtest/vtest_protocol.h`，两处要一致）：

```
命令: VCMD_WINEHUA_PRESENT = 0x57485052（"WHP"）
版本: 当前 4，SIZE 14，FLAGS 必须为 0

位置  字段            含义
0     PROTOCOL_VERSION 必须 = 4，两边格式对不上就靠它报错
1     FLAGS            必须 = 0（保留）
2     RES_HANDLE       guest 侧的资源编号（virgl 资源 id）
3,4   LEVEL, LAYER     纹理层级/层
5     FORMAT           guest 的 virgl 格式
6     BIND             资源的 bind 标志
7,8   WIDTH, HEIGHT    guest 报告的尺寸（host 侧要拿它跟实际纹理核对）
9,10  DRAWABLE_LO/HI   窗口句柄（64 位，拆成两个 u32）
11    SERIAL           递增序号，用来配对请求和回复
12    SURFACE_ID       窗口的 wl_surface id（从 C1 来）
13    CLIENT_PID       guest 用户进程的 pid

回复: SIZE 4
0     STATUS           0 成功；>0 被节流（重试）；<0 失败
1,2   DEADLINE_LO/HI   下次最早的呈现时间（纳秒，64 位）
3     SERIAL           回显请求的序号
```

发送端（mesa）还有一层节流：等 `pacer->next_present_ns` → 发送 → 校验回复
（长度/编号/序号三项，不对就报 `-EPROTO`）→ 被节流就重试，最多 8 次
（`virgl_vtest_socket.c:494-560`，重试循环在 533，校验在 554-558）。

**代码在哪儿**

| 端 | 位置 | 干什么 |
|----|------|--------|
| 协议定义 | mesa `src/virtio/vtest/vtest_protocol.h:88-111`；virglrenderer `vtest/vtest_protocol.h` | 字段常量（两处要保持一致） |
| mesa（发） | `virgl_vtest_socket.c:494-560` | `virgl_vtest_send_winehua_present`：节流、重试、校验回复 |
| | `virgl_vtest_winsys.c:688-741` | `virgl_vtest_flush_frontbuffer` 里触发；开了 present 模式就**跳过**原来的像素读回 |
| virglrenderer（收） | `vtest/vtest_server.c:762-804` | 命令表 `winehua_present_command`，801 行按编号分发 |
| | `vtest/vtest_renderer.c:1059-1160` | 处理函数：长度/版本/flags 校验 → 按资源编号查表 → 取 host 纹理信息 → 核对 payload → 调 C5 回调 → 回写 deadline |

**出问题什么样**

| 现象 | 排查 |
|------|------|
| 版本对不上 → 直接打回 `-EPROTONOSUPPORT`（`vtest_renderer.c:1086`），guest 那边 `present_ret` 非 0 | 日志 `"unsupported WineHua present version"`；对比两边的 VERSION 宏 |
| 资源编号查不到 → `-ESRCH`（`vtest_renderer.c:1095`） | 多半是资源已被 guest 释放但帧还在；日志 `[VTEST-FRONTBUFFER] handle=...` |
| guest 报的尺寸/格式和 host 实际纹理对不上 → **不发回调**，这帧不上屏，日志 `match=0` | `winehua_diag "present count=... match=%d"`（`vtest_renderer.c:1101-1145`，match 打印在 1144） |
| 回复的长度/编号/序号对不上 → 发送端报 `-EPROTO`（`virgl_vtest_socket.c:554-558`） | guest 侧看 `present_ret` |
| host 进程死了 / socket 断了 → 写失败，guest 回落老路径 | `[virgl-child] vtest exited rc=...` |

---

## 4. C3: `WINEHUA_*` 环境变量（主仓库 ↔ wine/mesa）

**它是干什么的**

宿主（GraphicsBroker）启动 Wine 进程时，把图形配置通过环境变量告诉 guest：
用哪个 backend、guest 的 libEGL 在哪、要不要开 zero-copy、vtest socket 在哪。
guest 侧各模块按需读这些变量决定行为。**这是启动时一次性注入的配置通道**，
完整清单以 [opengl-virgl.md](opengl-virgl.md) 的
"宿主 ↔ guest 环境变量契约"一节为准（两侧代码都是直接写死字符串，
没有共享头文件，改的时候**两处都要改**）。

**本链路关键变量**

| 变量 | 谁注入 | 谁读 | 干什么 |
|------|--------|------|--------|
| `WINEHUA_EGL_LIBRARY_PATH` | broker | `win32u/opengl.c:899` | 指定 guest bundle 里的 libEGL 路径，win32u 按它 dlopen |
| `WINEHUA_WAYLAND_READBACK` | broker | `winewayland.drv/opengl.c:262` | 打开 readback 挂钩（换 driver_funcs） |
| `WINEHUA_VTEST_PRESENT=surface-queue` | broker | mesa `virgl_vtest_winsys.c:689` | 让 Mesa 换帧时发 C2 的 present 命令 |
| `WINEHUA_ZERO_COPY_READY_DIR` | broker | `opengl_readback.c:123` | C4 标记文件放哪个目录 |
| `WINEHUA_GL_STALL_DIAG` | broker | `opengl_readback.c:207` | 开卡顿诊断日志 |
| `WINEHUA_VIRGL_SOCKET` / `VTEST_SOCKET_NAME` | broker | mesa winsys / guest probe | vtest socket 的路径 |
| `BOX64_EMULATED_LIBS` | broker | box64 | 哪些 guest 库按 x86_64 ABI 模拟（libEGL/libGLESv2/libwayland-client 等） |

注入实现：`entry/src/main/cpp/graphics/graphics_broker.cpp:897-977`（`AppendWineEnv`，
只有 virgl backend 生效时才注入 GL 相关项）。

**出问题什么样**

| 现象 | 排查 |
|------|------|
| 变量没注入或读不到 → 走默认值或退化成 shm 渲染，不会崩 | 对比 `WINEHUA_GRAPHICS_ACTIVE` / `WINEHUA_GRAPHICS_NOTE` 两个状态变量 |
| 两边字符串写错一个字母 → 静默走默认路径，功能降级 | 直接 grep 两侧字面量，对不上就是它 |
| guest 库加载失败（路径不对）→ win32u 打 `dlopen(...) failed` 日志（`win32u/opengl.c:912`） | 开 `WINEHUA_OPENGL_DIAG` 看详细日志 |

---

## 5. C4: `.ready` 零拷贝标记（主仓库 ↔ wine）

**它是干什么的**

guest 和 host 商量"这个窗口的帧要不要走 zero-copy"。整个流程是：

```
host: EglRenderer 挂好 OH_NativeImage 消费端 → 通过 IPC 把 producer 窗口交给 virgl 子进程
      → 首帧从 SurfaceQueue 到达 → 写 .ready 标记文件
guest: 下次 SwapBuffers 时查到标记 → 切 zero-copy（只 swap，不读回像素）
失败降级: host 连续 8 次取帧失败 → 删掉标记 → guest 自动退回像素读回
      → GPU 恢复了（新帧到达）→ 重新写标记 → 切回 zero-copy
```

跟 C3 不同，这是**运行期随时能变**的开关 —— 不用重启就能在两条路之间切换，
所以用文件标记而不是环境变量。

**两边怎么约定的**

- 文件名：`<WINEHUA_ZERO_COPY_READY_DIR>/winehua_zc_surface_<key>.ready`
- key = (guest 用户进程 pid << 32) | surface_id（和 C1 里的 surface_id 一致）
- 规则：文件存在 = 该窗口已有 zero-copy 消费端

**代码在哪儿**

| 端 | 位置 | 干什么 |
|----|------|--------|
| 主仓库（写） | `graphics_broker.cpp:95-99` | 拼路径（`ZeroCopyReadyPath`） |
| | `graphics_broker.cpp:634-661` | `SetZeroCopySurfaceReady`：删文件 / 写文件 |
| | `graphics_broker.cpp:507-524` | `AttachZeroCopyTarget`：IPC 把 producer 窗口交给子进程 |
| | `graphics_broker.cpp:101-114` | 启动时清理上次遗留的僵尸标记 |
| wine（读） | `opengl_readback.c:121-133` | `winehua_surface_zero_copy_ready`：access 查文件在不在 |
| 切换决策 | `egl_renderer.cpp:277-408` | `UpdateZeroCopyFrame`：连续 8 次失败 → 撤标记；恢复 → 重新标记 |
| | `egl_renderer.cpp:123-276` | `TryAttachZeroCopySurface`：每 100ms 轮询一次有哪些窗口 |

**出问题什么样**

| 现象 | 排查 |
|------|------|
| 没标记 → guest 一直走像素读回（慢，但画面是对的） | 看 `WINEHUA_FRAME_TRANSPORT=wl_shm+cpu_copy+gl_upload` |
| 标记在但 host 消费端断了（IPC 死 / 子进程崩）→ guest 走 zero-copy 但没帧 → 白屏 | `[VIRGL-ZC][MAIN] fallback pending`（8 次失败后会自动恢复）；`[VIRGL-ZC][NCP] blit dropped` |
| 上次进程没退干净留下僵尸标记 → 这次误判 ready | 启动时 `RemoveStaleZeroCopyMarkers` 会清（graphics_broker.cpp:101） |
| 两边 key 算得不一样（pid/surface_id 错位）→ guest 永远查不到 → 一直走老路 | 对比两侧 key 算法（`(pid<<32)\|surface_id`） |

---

## 6. C5: present 回调函数（virglrenderer ↔ 主仓库）

**它是干什么的**

virglrenderer 只管把 guest 的命令翻译成 host 的 GL 调用、把纹理画出来，
**怎么把纹理送上屏幕是宿主的事**。所以 virglrenderer 留了一个回调接口：
"纹理 id 给你，怎么显示你说了算"。主仓库的 virgl_child 进程在启动时
`dlopen` 一个 helper 库（`libwinehua_vtest_server.so`），把回调注册进去。

**两边怎么约定的**

virgl_child 启动时 dlsym 三个导出函数（签名见 `virglrenderer/vtest/winehua_vtest_server.c:18-51`
和 `vtest/vtest.h:34`）：

| 函数 | 回调参数（要点） | 用途 |
|------|-----------------|------|
| `winehua_vtest_set_present_callback` | `(纹理id, 宽, 高, 格式, flags, drawable, 序号, 客户端pid, surfaceId, flags, &deadline, userData) → int` | GL 纹理呈现 |
| `winehua_vtest_set_vulkan_present_callback` | `(contextId, instance, 物理设备, 设备, queue, image, ...) → int` | Vulkan 图像呈现（DXVK 链路） |
| `winehua_vtest_set_vulkan_device_release_callback` | `(contextId, device, phase, waitResult) → int` | Vulkan 设备释放，分两阶段 |

回调返回值约定：`0` 成功；`>0` 被节流（可重试）；`<0` 失败
（`-2` 目标没挂上、`-3` 纹理不可见、`-4/-5` EGL 出错、`-6` 上屏/恢复失败、`-7` fence 失败）。

**代码在哪儿**

| 端 | 位置 | 干什么 |
|----|------|--------|
| virglrenderer（导出） | `vtest/winehua_vtest_server.c:18-51` | 三个导出函数，转发给 vtest 内部 |
| | `vtest/vtest_renderer.c:1052-1057` | 存下回调函数指针 |
| 主仓库（消费） | `entry/src/main/cpp/graphics/virgl_child.cpp:666` | `dlopen` helper 库 |
| | `virgl_child.cpp:704-730` | dlsym 三个函数并注册 |
| | `virgl_child.cpp:351-388` | GL 回调实现：先查纹理可不可见，再调 `PresentVirglSurface` |
| | `virgl_child.cpp:390-467` | Vulkan 呈现回调 / 设备释放回调 |
| 实际显示 | `entry/src/main/cpp/graphics/virgl_surface_presenter.cpp:134-247` | `SurfaceQueueTarget::Present`：把纹理贴到 OHNativeWindow 上 |

**出问题什么样**

| 现象 | 排查 |
|------|------|
| 符号找不到（helper 库版本旧）→ 打印 WARN `"present callback registration missing"`（`virgl_child.cpp:709`）→ C2 回复 `-ENOSYS` → guest 回落老路径 | 看 `[virgl-child]` 日志，对比库版本 |
| 回调来了但纹理不可见（上下文不对 / 纹理已被删）→ 返回 -1，不送上屏 | `[VIRGL-PRESENT][NCP] ... visible=FAIL` |
| 目标还没挂上（IPC attach 比第一帧晚）→ 返回 -2 | GL 侧直接失败；Venus 侧会等 attach（最多 2.5 秒） |
| 上屏失败（EGL 出错）→ 返回 -6 | `[VIRGL-ZC][NCP] blit dropped serial=... gl=... egl=...` |

---

## 7. C6: virgl IPC 协议（主仓库 ↔ virgl_child）

**它是干什么的**

主进程（GraphicsBroker）和 virgl 子进程（virgl_child）之间的控制通道：把 host
配置告诉子进程、把窗口（OHNativeWindow）通过 IPC 传给子进程、查询有哪些窗口
在呈现、设置帧率、关机。跟 C2 不同（那是 guest 和 host 之间的数据通道），
这条是**宿主机内部**的控制通道（走 Binder；phone 模式下退化成 socket 转发）。

**两边怎么约定的**

协议定义唯一权威是 `entry/src/main/cpp/graphics/virgl_ipc_protocol.h`：

```
魔数: kMagic = 0x57484950（"WHIP"）
版本: kProtocolVersion = 9（字段动过就要 +1）
上限: 最多 16 个窗口

请求编号: 配置=1, 挂窗口=2, 摘窗口=3, 关机=4, 查询窗口=5, 设帧率=6

窗口信息: { surfaceKey u64, clientPid u32, surfaceId u32,
           宽 u32, 高 u32, serial u32, flags u32 }
flags:  已挂上=1<<0, Vulkan=1<<1, 本进程内引用=1<<2（仅 phone 模式用）

查询回复: { magic, version, size, count, 窗口信息[16] }

每个请求的第一个 u32 必须是版本号。
```

配置请求（编号 1）的载荷是 11 个字符串：`helperPath | socketPath |
libraryPath | syncMode | logPath | shadowMode | shadowTrace | presentMode |
shadowMergeRanges | descriptorUpdateSerialize | gpuUploadWait`，
前几个都是路径，必须 `/` 开头（防注入）。

**代码在哪儿**

| 端 | 位置 | 干什么 |
|----|------|--------|
| 协议头 | `entry/src/main/cpp/graphics/virgl_ipc_protocol.h`（全文） | 常量 + 数据结构 |
| 主仓库（发） | `graphics_broker.cpp:301-352` | 发配置（`SendVirglConfigureLocked`） |
| | `graphics_broker.cpp:353-431` | 挂窗口（把 OHNativeWindow 写进 parcel 传过去） |
| | `graphics_broker.cpp:432-506` | 设帧率 / 摘窗口 |
| | `graphics_broker.cpp:556-632` | 查询窗口（校验回复的 magic/version/size/count） |
| virgl_child（收） | `virgl_child.cpp:124-261` | `OnVirglIpcRequest`：先校验版本，再分发 |
| | `virgl_child.cpp:523-530` | `NativeChildProcess_OnConnect`：注册 IPC stub |
| phone 模式 | `graphics_broker.cpp:17-30` | 假 proxy → socket 转发（`PhoneVirgl_RelayRequest`） |

**出问题什么样**

| 现象 | 排查 |
|------|------|
| 版本对不上 → 第一个 u32 校验失败，返回 -1 | 看 `OnVirglIpcRequest` 的返回码 |
| 配置非法（路径不是 `/` 开头 / presentMode 不认识）→ -2；配置本身不合理 → -4 | `[VIRGL-ZC][NCP] invalid host config` |
| phone 模式下挂窗口（OHNativeWindow 没法跨 Binder 传）→ 假装失败 → 退化成 shm 渲染 | `[PhoneVirgl] AttachSurface denied`（graphics_broker.cpp:23-28） |
| 子进程没反应 → 发个查询探活，失败就销毁 proxy 重建（`graphics_broker.cpp:1017-1096`） | `[GraphicsBroker] virgl IPC native child process is not responding` |
| 查询回复的魔数/版本/长度/数量对不上 → 当错误处理（`graphics_broker.cpp:609-613`） | 没画面 + 看 `[VIRGL-ZC][MAIN]` 日志 |

---

## 8. C7: Vulkan 私有 surface 标记（winewayland.drv ↔ win32u）

**它是干什么的**

WineHua 把 Vulkan 的窗口系统（WSI）整个私有化了：guest 侧永远拿不到真正的
VkSurfaceKHR —— host 没有 Wayland WSI，只有 OHNativeWindow。所以窗口身份就
用"**高位带标记的假句柄**"传递：窗口的 wl_surface id 藏进句柄的低 32 位，
谁拿到这个句柄就知道它是哪个窗口。

严格说这是 wine 仓库**内部**两个文件（winewayland.drv 和 win32u）之间的约定，
不是跨仓库。但它跟 C8 的 `(pid, surface_id)` 语义配对（DXVK 链路的起点），
而且跟跨仓库约定一样没有编译器把关，所以一并记在这里。

**两边怎么约定的**

```
表面句柄: 0x5748530000000000 | wl_surface_id      （tag = 0x574853 "WS"）
交换链句柄: 0x5748430000000000                    （tag = 0x574843，标识私有 swapchain）
识别: 句柄 & 0xffffffff00000000 == WINEHUA_VULKAN_SURFACE_TAG → 是私有表面
```

配套行为（`WINEHUA_VULKAN_PRESENT` 或 `WINEHUA_GRAPHICS_BACKEND=virgl` 开启时）：
- `p_get_physical_device_presentation_support` 恒返回 VK_TRUE —— 私有表面不是 host
  WSI 对象，所有 queue 都"支持呈现"（调 host 的 Wayland 检查会解引用空指针，
  见 `winewayland.drv/vulkan.c:114-119` 的注释）；
- 扩展映射：`VK_KHR_win32_surface` 直接可用，不给 guest 暴露 Wayland surface 扩展。

**代码在哪儿**

| 端 | 位置 | 干什么 |
|----|------|--------|
| winewayland.drv | `thirdparty/wine/dlls/winewayland.drv/vulkan.c:44` | tag 定义（`WINEHUA_VULKAN_SURFACE_TAG`） |
| | `vulkan.c:64-105` | `wayland_vulkan_surface_create`：私有分支拼 tag 句柄（76-77） |
| | `vulkan.c:46-62` | `winehua_vulkan_present_enabled`：三个开关（环境变量 / present backend / virgl 标记） |
| | `vulkan.c:114-119, 125-151` | presentation support 恒真 + 扩展映射 |
| win32u | `thirdparty/wine/dlls/win32u/vulkan.c:51-52` | surface / swapchain 两个 tag |
| | `win32u/vulkan.c:89-92` | `winehua_private_surface_handle`：按高位 tag 识别 |
| | `win32u/vulkan.c:1668-1671` | 识别出私有表面 → 记下 `winehua_surface_id`（低 32 位） |

**出问题什么样**

| 现象 | 排查 |
|------|------|
| 低 32 位不是 wl_surface id（tag 拼错 / 字节序错）→ 帧送错窗口或找不到窗口 | 对比两端 tag 定义；`[VENUS-PRESENT][NCP] ... surface=...` 和窗口的对应关系 |
| 没走私有分支（开关没开）→ 走标准 wayland surface 路径 → host 没有 Wayland WSI → 创建失败 | `winewayland.drv/vulkan.c:92` 调 `vkCreateWaylandSurfaceKHR` 的 ERR 日志 |
| 开启条件三个开关都读不到 → 退回标准路径 | 检查 `WINEHUA_VULKAN_PRESENT` / `WINEHUA_GRAPHICS_BACKEND` 是否注入 |

---

## 9. C8: `vn_winehua_present` 函数（win32u ↔ mesa venus）

**它是干什么的**

win32u 的私有 swapchain 在 QueuePresent 时，要把图像交给 guest venus 去发 C9
的 present 命令。两边在同一个进程里（用户程序进程），所以直接函数调用：
win32u 从**已经加载**的 `libvulkan_virtio.so` 里取 `vn_winehua_present` 符号。

**两边怎么约定的**

- 符号名：`vn_winehua_present`，签名固定：

```c
int vn_winehua_present(VkQueue, VkImage, uint32_t width, uint32_t height,
                       VkFormat, VkImageLayout,
                       uint32_t client_pid, uint32_t surface_id,
                       uint32_t serial, uint64_t *next_present_deadline_ns);
```

- 返回值：`0` 成功；`-EINVAL` 参数不对；其它负数失败。**`-EAGAIN` 是内部重试信号，
  mesa 绝不会把它返回给 Wine** —— Wine 的 Vulkan thunk 把负数映射成
  `DEVICE_LOST`，一次瞬时重试就能毒化一个本来健康的进程（mesa 侧注释原话）。
- 加载方式：`dlopen("libvulkan_virtio.so", RTLD_NOW|RTLD_LOCAL|RTLD_NOLOAD)` ——
  不触发加载，只在已加载时取句柄（避免二次 dlopen）。

**代码在哪儿**

| 端 | 位置 | 干什么 |
|----|------|--------|
| win32u（调） | `thirdparty/wine/dlls/win32u/vulkan.c:228-232` | 函数指针类型定义 |
| | `win32u/vulkan.c:235-241` | `winehua_present_init`：dlopen(RTLD_NOLOAD) + dlsym |
| | `win32u/vulkan.c:243-256` | `winehua_present_image`：填 pid/surface_id/serial 后调用 |
| mesa（实现） | `thirdparty/mesa/src/virtio/vulkan/vn_renderer_vtest.c:1273-1420` | `vn_winehua_present` 本体 |

mesa 实现内部（从 1273 起）按顺序做四件事：

1. **参数校验**：queue/image/宽高/pid/surface_id 缺一 → `-EINVAL`；
2. **pacing**：等 host 上次回复的 deadline（`queue->winehua_next_present_deadline_ns`）
   —— 而且是在应用画完下一帧、进入 present 之后才等，让等待时间和游戏渲染重叠；
3. **ring drain**：`vn_ring_roundtrip` + `vn_ring_wait_all`（1291 附近）—— 提交命令
   走 Venus ring、present 走 socket，两条道，必须把 ring 排空一次，否则 present
   可能抢在 QueueSubmit 之前拿到 host 的队列锁；
4. **`-EAGAIN` 重试**：最多 8 次，每次失败 `usleep(1ms << attempt)` 指数退避。

**出问题什么样**

| 现象 | 排查 |
|------|------|
| 符号不在（venus 库没被加载）→ WARN `"WineHua Vulkan private present entry is unavailable"`（`win32u/vulkan.c:241`）→ 返回 `-ENOSYS`，swapchain 呈现失败 | 看 win32u WARN；检查 winevulkan 是否真的加载了 venus ICD |
| `-EAGAIN` 泄漏给了 Wine（改 mesa 时破坏了重试逻辑）→ DXVK 报 DEVICE_LOST，进程废掉 | mesa 里搜 `-EAGAIN` 的返回路径，确认全部在内部消化 |
| 两边签名不一致（加了参数没同步）→ 参数错位，行为诡异且无编译错误 | 对比两端函数声明（win32u/vulkan.c:228 ↔ vn_renderer_vtest.c:1273） |

---

## 10. C9: `VCMD_WINEHUA_VK_PRESENT` 命令（mesa venus ↔ virglrenderer）

**它是干什么的**

跟 C2（GL 版）同角色，但有个关键区别：GL 版传的是**资源句柄**，host 端译成
纹理 id 后用 EGL 贴上屏；Vulkan 版传的是 **Venus 对象 id**（queue 和 image），
host 端要按 id 在对象表里查回真实对象、拿 host handle，然后**跟 QueueSubmit
抢同一把队列锁**，最后用 `vkQueuePresentKHR` 直接呈现。

**两边怎么约定的**

字段定义在 `thirdparty/mesa/src/virtio/vtest/vtest_protocol.h:115-137`：

```
命令: VCMD_WINEHUA_VK_PRESENT = 0x57485650（"WVP"）
版本: 当前 1，SIZE 13，FLAGS 必须为 0

位置  字段            含义
0     PROTOCOL_VERSION 必须 = 1
1     FLAGS            必须 = 0（保留）
2,3   QUEUE_ID_LO/HI   Venus queue 对象 id（64 位，拆两个 u32）
4,5   IMAGE_ID_LO/HI   Venus image 对象 id（64 位）
6,7   WIDTH, HEIGHT    图像尺寸（host 侧核对用）
8     FORMAT           VkFormat
9     LAYOUT           VkImageLayout（host 侧有白名单）
10    SERIAL           递增序号，配对请求和回复
11    SURFACE_ID       窗口的 wl_surface id（来自 C7 的 tag）
12    CLIENT_PID       guest 用户进程的 pid

回复: SIZE 4（STATUS + DEADLINE_LO/HI + SERIAL，同 C2）
```

host 侧 `vkr_renderer_winehua_present`（`vkr_renderer.c:306-443`）的处理顺序：
layout 白名单（GENERAL / COLOR_ATTACHMENT_OPTIMAL / TRANSFER_SRC_OPTIMAL /
PRESENT_SRC_KHR，其它 → `-EINVAL`）→ 三层锁（`context_mutex` → `object_mutex`
→ `queue->vk_mutex`，全是 trylock，**拿不到就返回 `-EAGAIN`**）→ 按 queue_id /
image_id 查对象表（对象还没发布 → `-EAGAIN`）→ `image_matches` 核对
（device 归属、2D、尺寸、格式、usage 含 TRANSFER_SRC，不符 → `-EINVAL`）→
释放 context/object 锁 → **保持 queue 锁**调 C10 回调（回调可能阻塞在
`vkQueuePresentKHR`，queue 锁保证它不会跟 QueueSubmit 并发）。

**代码在哪儿**

| 端 | 位置 | 干什么 |
|----|------|--------|
| 协议定义 | mesa `src/virtio/vtest/vtest_protocol.h:115-137`；virglrenderer `vtest/vtest_protocol.h` | 字段常量（两处保持一致） |
| mesa（发） | `vn_renderer_vtest.c:638-699` | `vtest_vcmd_winehua_vk_present`：构造命令、收发回复 |
| | `vn_renderer_vtest.c:1273-1420` | `vn_winehua_present`：drain + 重试后调到这里 |
| virglrenderer（收） | `vtest/vtest_renderer.c:1161-1250` | `vtest_winehua_vk_present`：长度/版本/flags 校验 → 调 `virgl_renderer_winehua_vk_present` |
| | `vtest/vtest_server.c:766-767, 804` | 命令表 `winehua_vk_present_command` + 分发 |
| | `src/venus/vkr_renderer.c:306-443` | `vkr_renderer_winehua_present`：锁、查对象、核对、调回调 |

**出问题什么样**

| 现象 | 排查 |
|------|------|
| 版本不符 → `-EPROTONOSUPPORT` | `winehua_diag "vk_present count=... ret=-125"` 之类的负值 |
| 对象查不到 / 锁拿不到（对方正在 submit）→ `-EAGAIN` → mesa 侧重试 | 这是**成对**的：收端报"还早"，发端等 `usleep(1ms<<attempt)`。反复重试说明对象发布或锁竞争异常 |
| layout 不在白名单 / image 核对不过（尺寸/格式/usage 不符）→ `-EINVAL` → 这帧不上屏 | `vkr_winehua_stage("...")` 各阶段的 trace（`WINEHUA_VKR_PRESENT_STAGE_TRACE` 打开） |
| 回调没注册 → `-ENOSYS` | `[virgl-child]` 的注册日志 |
| 三级锁都过但回调里 `vkQueuePresentKHR` 失败 → 返回回调的错误码 | `[VENUS-PRESENT][NCP]` 的 fail 统计 |

---

## 11. C10: Vulkan 呈现与设备释放回调（virglrenderer ↔ 主仓库）

**它是干什么的**

跟 C5 同族：vtest 把 Venus 对象翻译成 host handle 之后，**呈现和资源释放都
外包给宿主**。比 C5 多一个设备释放回调，分两个阶段 —— 因为 host 的 Vulkan
设备销毁时，呈现器可能还拿着它的 queue/image 在排队，必须先让呈现器收尾、
等 host 队列清空，再动真格的清理。

**两边怎么约定的**

| 函数 | 回调参数（要点） | 用途 |
|------|-----------------|------|
| `winehua_vtest_set_vulkan_present_callback` | 见 C5 表，返回约定同 C5 | Vulkan 图像呈现 |
| `winehua_vtest_set_vulkan_device_release_callback` | `(contextId, device, phase, waitResult) → int` | 设备释放，两阶段：`phase 0 = PREPARE`（呈现器解绑收尾）、`phase 1 = AFTER_WAIT`（host 队列等完后再清理） |

阶段枚举定义在 `vkr_renderer.h:48-50`（`VKR_RENDERER_WINEHUA_DEVICE_RELEASE_PREPARE = 0`
/ `AFTER_WAIT = 1`）。

**代码在哪儿**

| 端 | 位置 | 干什么 |
|----|------|--------|
| virglrenderer（导出） | `src/venus/vkr_renderer.h:24-53` | 回调类型 + 阶段枚举 |
| | `vkr_renderer.h:75-91` | `vkr_renderer_set_winehua_present_callback` / `set_winehua_device_release_callback` / `release_device` |
| | `vkr_renderer.c:94-99` | 存回调指针 |
| 主仓库（消费） | `entry/src/main/cpp/graphics/virgl_child.cpp:721-730` | dlsym 设备释放回调并注册 |
| | `virgl_child.cpp:450-467` | `OnVtestVulkanDeviceRelease`：按 phase 分发到 Prepare/Finish |
| 实际呈现 | `entry/src/main/cpp/graphics/venus_surface_presenter.cpp:276 起` | `Present`：pacing → `vkQueuePresentKHR`（539 行） |
| 设备释放配套 | `virgl_surface_presenter.cpp:536-586` | `PrepareVenusDeviceRelease` / `FinishVenusDeviceRelease`（含退休 target 处理） |

Vulkan 呈现还有一个 GL 版没有的等待机制（`virgl_surface_presenter.cpp:664-699`）：
target 还没 attach 时，`PresentVenusSurface` 会**等 attach 最多 2.5 秒**（有
detach 信号就提前结束），超时返回 `-EAGAIN` —— 配合 C8 的 8 次重试。GL 版不
等（直接失败）是因为 OpenGL 的 present 可以丢帧，DXVK 的 present 不能。

**出问题什么样**

| 现象 | 排查 |
|------|------|
| 设备释放回调漏调 / 顺序错 → host 设备销毁时呈现器还挂着它的对象 → 崩溃 | 看 `PrepareVenusDeviceRelease` / `FinishVenusDeviceRelease` 是否成对出现（`virgl_surface_presenter.cpp:536-586` 有匹配统计日志） |
| attach 等满 2.5 秒 → `-EAGAIN` → mesa 重试 | `[VENUS-PRESENT][NCP] target wait ended ... reason=timeout` |
| phase 传错（不在 0/1）→ 返回 `-EINVAL` | `[VENUS-PRESENT][NCP] invalid device release phase`（`virgl_child.cpp:459-465`） |

---

## 12. DXVK 为什么不用参与

DXVK submodule 在这条链路上**没有任何私有契约**。它只跟 wine 打交道，走的还是
标准接口：D3D11 → Vulkan 翻译，经 `winevulkan.dll`（Wine 的 Vulkan loader）→
win32u 的私有 swapchain。win32u 在 QueuePresent 时偷偷把图像交给 venus（C8），
DXVK 完全不知情 —— 它以为自己还在用普通 `VK_KHR_swapchain`。

所以升级 DXVK 不破坏任何 C1–C10 的协议：只要它还是 D3D11→Vulkan 翻译，输出
还是标准的 vkQueuePresentKHR，链条就照常工作。DXVK 侧的定制只有**运行时行为
开关**（如 `DXVK_WINEHUA_DUAL_SRC_MODE`，见 `dxvk/src/dxvk/dxvk_winehua_trace.h`），
都是环境变量级的，不涉及字节格式，改它们不产生版本漂移。

（DXVK 的补丁清单见 `docs/assets/submodules/dxvk.md`。）

---

## 13. 改这些协议之前（铁律）

1. **改了格式必须升版本号**：`VCMD_WINEHUA_PRESENT_VERSION`、
   `VCMD_WINEHUA_VK_PRESENT_VERSION`、`kProtocolVersion`、共享内存页的
   version —— 版本号是"我认不出你"的唯一报警通道。
2. **对端必须同步改**：改 C2 要同时动 mesa（发的那边）和 virglrenderer（收的
   那边）；改 C1 要同时动 wine 和 mesa；改 C8（函数签名）要同时动 win32u
   （wine）和 mesa；改 C9 要同时动 mesa 和 virglrenderer。改完跑双平台全量回归。
3. **同步更新本文档和 `docs/assets/submodules/*.md`**：该目录记录
   "这一个仓库改了协议的哪一版"，本文档记录"整条协议长什么样"。
4. **魔数、版本、长度、序号这四样缺一不可**：文件/包的身份证、格式版本、
   字段数量、请求-回复配对。
5. **先写文档再写代码**：两端各自开工前，先把格式表定稿到本文档，避免实现
   期间两边漂移。
