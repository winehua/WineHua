# 诊断开关登记表

排查设备问题时"有哪些开关、在哪一侧、怎么开"的索引。所有开关都是**环境变量**，
不改变产品行为（默认关闭/默认档），只增加诊断输出或切换验证档位。

配套工具：
- `bash scripts/collect-diag.sh -t <设备>` — 一键采集现场 (hilog/进程/版本)
- `bash scripts/strip-smoke.sh` — main-ui 摘除 smoke 设施

---

## 1. 两条注入通道

一切开关经 `winehua.diag_env` want 参数传入，一份 spec 走两条**互相独立**的路：

```
aa start -a EntryAbility -b app.hackeris.winehua \
    --ps winehua.diag_env "WINEHUA_FRAME_TRACE=1;WINEDEBUG=-all,+sync"
                          └───────┬────────┘ └──────┬──────┘
                                  │                 │
                    ① 宿主侧 setHostDiagEnv   ② 子进程侧 diagEnvironment()
                      → setenv(app 进程)          → runWineProgram.environment
                      → compositor/egl_renderer      → __env 段 → wine 子进程 setenv
```

| | ① 宿主侧 | ② 子进程侧 |
|---|---|---|
| 作用对象 | app 进程自身 | wine / box64 子进程 |
| 实现 | `bridge/napi_init.cpp` `SetHostDiagEnv` | `WineEnvService.diagEnvironment()` → `proc/wine_child.cpp` `apply_entry_param_env_overrides` |
| 键名限制 | **前缀白名单**：`WINEHUA_` `VKR_` `VN_` `DXVK_` `MESA_` `VKD3D_` | 无限制 |
| 时机约束 | 必须**早于消费方首次 getenv**（部分开关有 static 缓存） | 进程启动时 |
| 生效范围 | 本次 App 生命周期 | 本次启动的进程（重启引擎仍生效，App 重启失效） |

**为什么必须分开**：NCP 子进程**不继承宿主 environ**（wine 子进程 env 的唯一权威
通道是 entryParams 的 `__env` 段），所以在宿主 setenv 不会传进 wine，反之亦然。

**覆盖优先级**：子进程侧 `apply_entry_param_env_overrides` 执行在 `setup_wine_env`
**之后** → 注入值**覆盖**代码写的值。这就是"用环境变量控制一切"能成立的原因。

**注不了的情况**：桌面内双击启动的程序、wineserver 等由 wine 自己 spawn 的进程
不走 `runWineProgram`，吃不到 ②（宿主侧 ① 仍作用于 App 自身）。这类进程的 env
来自会话 overlay（`wine/env_profiles.cpp`），要改得改代码。

---

## 2. 常用开关（按排查场景）

### 2.1 卡死 / 无响应

| 键 | 侧 | 作用 |
|---|---|---|
| `WINEHUA_FRAME_TRACE=1` | ① 宿主 | 帧级诊断：渲染循环每帧尺寸/letterbox/上屏判定（`perf_utils.h` 门控，关闭时零开销） |
| `WINEHUA_GFX_DIAG=1` | ① 宿主 | 向 guest 下发下面两个键（门控闸） |
| `WINEHUA_GL_STALL_DIAG=1` | ② 子进程 | guest GL 各阶段 stall（≥1s）/ slow（≥50ms）检测，走 stderr |
| `WINEHUA_OPENGL_DIAG=1` | ② 子进程 | guest OpenGL 诊断总开关（`win32u/opengl_diag.c`） |

卡死定位主要靠 **hilog 心跳**（`WL-STAT` 30s 资源快照、`Input-DROP` 60s 丢帧、
`MW-RNDR` 尺寸告警）。注意 `/proc/<pid>/task` 在设备上**不可枚举**（详见
`scripts/collect-diag.sh` 头部的能力边界），拿不到线程栈。

### 2.2 渲染 / 出图异常

| 键 | 侧 | 作用 |
|---|---|---|
| `VKR_WINEHUA_SHADOW_TRACE=<档>` | ①→guest | shadow 传输诊断档选择器，值同时是帧级诊断门 |
| `WINEHUA_VIRGL_HOST_LOG_PATH=<路径>` | ① 宿主 | 宿主 virgl 日志落盘位置 |
| `WINEHUA_VTEST_FRONTBUFFER_LOG=<路径>` | ② 子进程 | mesa frontbuffer 无 display target 采样日志（每 120 次追加） |
| `WINEHUA_VTEST_PRESENT_PERF_SUMMARY=1` | ② 子进程 | vtest present perf 汇总 |
| `WINEHUA_VKR_TRACE_PRESENT_IMAGE=1` | ② 子进程 | venus present 帧序跟踪（`[VENUS-ORDER]`） |
| `WINEHUA_VENUS_GPU_FRAME_PROFILE=1` | ② 子进程 | venus 帧级 GPU timestamp 采样 |

### 2.3 wine 自身日志

| 键 | 侧 | 作用 |
|---|---|---|
| `WINEDEBUG=-all,+<通道>` | ② 子进程 | 上游标准通道过滤（默认 `-all`）。如 `+waylanddrv`、`+sync` |
| `WINEDEBUG=+relay` | ② 子进程 | 全 API 调用跟踪 — **量大**，需应用内导出（hilog 会被冲垮） |

stderr 去向：`WineChild-stderr` 转发到 hilog（带 `pid=` / `exe=` 标识），同时
写沙箱 `temp/wine_stderr_YYYYMMDD.log`（沙箱对 hdc 不可读，只能走 hilog）。

### 2.4 图形后端 / 档位

| 键 | 侧 | 作用 |
|---|---|---|
| `WINEHUA_PERF_PROFILE=<档>` | ② 子进程 | guest shadow 传输档（默认 `shadow-precise-dirty-ring-inline-upload-coverage-sort`） |
| `winehua.perf.profile` (want 名) | ① 宿主 | host 侧 shadow 档，默认同上（见 `WineEnvService.perfProfile` 注释） |
| `WINEHUA_D3D_BACKEND` / `winehua.d3d_backend` | both | `vkd3d_limited_500k` / `dxvk_modern_2_6` / `dxvk_legacy` / `wined3d` |

### 2.5 输入 / 指针

输入链路的日志（`CLICK-PIPE` / `KBD-PIPE` / `WL_Input`）**常开**，无需开关。
相对指针模式的开关见 `entry/src/main/ets/components/DesktopLayer.ets` 的
tapPositioning 相关字段（ArkTS 侧设置，非环境变量）。

---

## 3. 启动参数速查（非环境变量）

这些是 `--ps` 直传的启动开关，与 `winehua.diag_env` 平级：

| 参数 | 值 | 作用 |
|---|---|---|
| `winehua.diag_env` | `K=V;K2=V2` | 本表主通道（两条路同时下发） |
| `winehua.program` | exe 路径 | 启动指定程序 |
| `winehua.d3d_backend` | 见上 | 覆盖 D3D 后端 |
| `winehua.dxvk_backend` | `dxvk_legacy` / `dxvk_modern_2_6` / `auto` | 覆盖 DXVK |
| `winehua.desktopMode` | `fusion` / `virtual` | 覆盖桌面形态（不持久化） |
| `winehua.fusionHost` | `subwindow` / `ability` | 覆盖多窗口承载 |
| `winehua.prefix` | 名字 | 指定 prefix |

---

## 4. 内部状态键（**不要手动设**）

下列键由系统在启动链中写入，手动注入会与代码逻辑打架（部分虽有覆盖效果，但语义
是"系统状态"而非"用户开关"）：

| 键 | 写入点 | 用途 |
|---|---|---|
| `WINEHUA_DESKTOP_MODE` | `wine/wine_env.cpp` | 桌面模式（虚拟桌面 vs 独立窗口） |
| `WINEHUA_GRAPHICS_BACKEND` | `graphics/graphics_broker.cpp` | 图形后端状态（guest 侧判私有 present 能力） |
| `WINEHUA_WAYLAND_READBACK` | `graphics_broker.cpp` | OpenGL readback driver 切换（恒 1） |
| `WAYLAND_DISPLAY` | `wine_env.cpp` / `wayland_server.cpp` | Wayland 直连加载标记 |
| `PROCESSBROKER` / `WINEBINDIR` / `WINEUNIXDIR` | `proc/wine_child.cpp` | 宿主-子进程 spawn 协议 |
| `WINESERVERSOCKET` | `proc/wine_child.cpp` | per-process wineserver fd |
| `WINEPREFIX` | `wine_env_baseline.h` | 会话 prefix（改它请用 `winehua.prefix`） |
| `WINEHUA_ZERO_COPY_READY_DIR` | `graphics_broker.cpp` | 零拷贝握手目录 |
| `WINEHUA_SIMULATE_RESOLUTION` | `wine_env.cpp` | 游戏分辨率请求回放 |
| `WINEWAYLAND_ENTER_SILENT` | `wine_env.cpp` | 相对模式 enter 静默校准 |

---

## 5. 登记表的维护约定

新增诊断开关时：

1. **键名必须有前缀**（`WINEHUA_` / `VKR_` / `VN_` / `DXVK_` / `MESA_` / `VKD3D_`）
   —— 宿主通道按前缀白名单放行，无前缀的键注不进去；
2. **默认关闭**：`getenv` 到未设置/`"0"` 时零开销、零行为变化；
3. **门控而非常开**：诊断输出（尤其文件写入）不许无条件常开 —
   `graphics_broker.cpp` 曾把 `WINEHUA_VTEST_FRONTBUFFER_LOG` 路径无条件塞进
   guest env，导致 mesa 侧的门控形同虚设（每 120 次调用 fopen/写/fclose，
   文件无上限增长），2026-09 修为门控；
4. **登记到本表**：写清侧别、设置点、消费方、默认值。

排查完请**关掉开关**（重启 App 即失效，spec 不持久化）。
