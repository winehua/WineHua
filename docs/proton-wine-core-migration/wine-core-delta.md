# Wine Core Delta：WineHua wine vs Valve Proton Wine

> 对照对象：`winehua/wine@dc5204ecb0c`（Wine 11.10 分叉）vs `ValveSoftware/wine@dc26e618`（proton_11.0，Wine 11.0）
> 口径：只统计 WineHua 自己写的提交（`--author` 过滤），不做全仓 diff
> 复现：`tools/wine-delta-audit.sh`（见 `README.md`）

## 1. 总量

| 指标 | 数值 |
| --- | --- |
| WineHua 署名提交（`origin/master` 上） | **61** |
| 只在当前分支上的提交 | **7**（含 `dc5204ecb0c`） |
| 这些提交触碰的文件（并集） | **101** |
| ├ 在 Valve 树里**不存在**（必须新增） | **40** |
| └ 在 Valve 树里**存在**（改动上游文件，必须逐个重放） | **61** |
| Valve 树文件总数 | 11149 |
| Valve 树里有无任何 `ohos` / `winehua` 文件 | **没有**（`grep -i ohos\|winehua` 命中 0） |

**关键判断**：WineHua 的改动集中在**平台层**，而且**40/101 是全新文件**——
这意味着迁移不是"打几个补丁"，而是"把一整套 OHOS 平台层重新落到新基线上"。
但反过来，**61 个改动上游文件的条目里，很多只是被上面那 40 个新文件牵动的注入点**，
真正需要逐行重放的代码量远小于 61 个文件的字面含义。

## 2. NEW：Valve 树里不存在（40 个，必须在 W1 新增）

### 2.1 OHOS 音频后端（10）

```text
dlls/wineohos.drv/Makefile.in
dlls/wineohos.drv/wineohos.spec
dlls/wineohos.drv/ohos.c
dlls/wineohos.drv/ohos_audio_client.c
dlls/wineohos.drv/ohos_audio_client.h
dlls/wineohos.drv/ohos_midi.c
dlls/wineohos.drv/ohos_midi.h
dlls/wineohos.drv/audio_ipc_protocol.h
dlls/wineohos.drv/tml.h          # 第三方 MIDI 音源（gin-h 之外自带）
dlls/wineohos.drv/tsf.h
```

### 2.2 OHOS 进程 broker / NCP（6）

```text
dlls/ntdll/unix/ohos_broker.c / .h     # SPAWN 协议、命名多 fd、__env 转发
dlls/ntdll/unix/ohos_file.c   / .h     # drive→unix 路径映射与文件语义回退
dlls/ntdll/unix/ohos_virtual.c / .h    # noexec / JIT / sigchain / SMC
```

### 2.3 winewayland.drv 的 OHOS 绑定与拆分（8）

```text
dlls/winewayland.drv/wayland_surface_ohos.c / .h   # OHOS surface 私有扩展
dlls/winewayland.drv/winehua-toplevel.xml          # 私有协议描述（virtual desktop 坐标等）
dlls/winewayland.drv/modal.c                       # WineHua 模态对话框探测
dlls/winewayland.drv/opengl_diag.c / .h            # 诊断框架拆分
dlls/winewayland.drv/opengl_readback.c / .h        # readback 管线拆分
```

### 2.4 其它平台件与工具

```text
dlls/win32u/opengl_diag.c / .h             # win32u 侧诊断框架
dlls/winebus.sys/bus_ohos.c                # OHOS 手柄总线
dlls/mciqtz32/mciqtz_waveout.c / .h        # waveOut 后端拆分
dlls/mciqtz32/minimp3.h                    # 第三方 MP3 解码
dlls/dnsapi/libresolv_musl.c               # musl 下无 libresolv 的解析回退
server/musl_compat.c                       # musl 兼容层（wineserver 用）
programs/winehua_keep/{Makefile.in,main.c} # 桌面 keep-alive 辅助进程
programs/winehua_dinput_probe/{Makefile.in,main.c}   # 诊断探针（可后置）
programs/winehua_graphics_smoke/{main.c,graphics_runtime_env.h}  # 诊断探针（可后置）
configure / .gitignore                     # 构建入口与忽略规则
```

## 3. BOTH：两边都有，属改动上游文件（61 个，必须逐个重放）

### 3.1 ntdll（19）—— 最大的一块

```text
dlls/ntdll/Makefile.in          dlls/ntdll/heap.c        dlls/ntdll/loader.c
dlls/ntdll/ntdll.spec           dlls/ntdll/ntdll_misc.h  dlls/ntdll/version.c
dlls/ntdll/signal_arm64.c       dlls/ntdll/signal_arm64ec.c
dlls/ntdll/signal_i386.c        dlls/ntdll/signal_x86_64.c
dlls/ntdll/unix/env.c           dlls/ntdll/unix/file.c   dlls/ntdll/unix/loader.c
dlls/ntdll/unix/process.c       dlls/ntdll/unix/server.c dlls/ntdll/unix/signal_arm64.c
dlls/ntdll/unix/signal_x86_64.c dlls/ntdll/unix/virtual.c dlls/ntdll/unixlib.h
```

### 3.2 winewayland.drv / win32u（各 8）

```text
dlls/winewayland.drv/{Makefile.in,opengl.c,wayland.c,wayland_pointer.c,
                      wayland_surface.c,waylanddrv.h,waylanddrv_main.c,window.c}
dlls/win32u/{Makefile.in,dce.c,driver.c,freetype.c,opengl.c,
             sysparams.c,vulkan.c,window.c}
```

### 3.3 server / wow64 / 其它

```text
server/{Makefile.in,directory.c,main.c,mapping.c,object.h,process.c,winstation.c}
dlls/wow64/{process.c,syscall.c,wow64.spec}
dlls/mciqtz32/{Makefile.in,mciqtz.c,mciqtz_private.h}
dlls/setupapi/{install.c,queue.c}     dlls/shell32/shlfolder.c
dlls/mountmgr.sys/unixlib.c           dlls/mmdevapi/client.c
dlls/kernel32/thread.c                dlls/winebus.sys/bus_sdl.c
dlls/dnsapi/Makefile.in               programs/wineboot/wineboot.c
include/winternl.h                    po/zh_CN.po
configure.ac                          tools/makedep.c
```

## 4. 问题 1 的回答：Proton Wine 能直接替代什么

从文件清单可以确定的（Valve `proton_11.0` 树里**已有**）：

| 能力 | 证据 | 含义 |
| --- | --- | --- |
| ARM64EC 基础设施 | `dlls/ntdll/signal_arm64ec.c`、`dlls/msvcrt/except_arm64ec.c` 存在 | 我们的 ARM64EC 改动要**在它之上重放**，而不是重写 |
| unixlib 机制 | 全树 59 个 `unixlib` 相关文件 | `wineohos.drv` 这类新后端可直接用现成机制接入 |
| WoW64 | `dlls/wow64/{process.c,syscall.c,wow64.spec}` 存在 | 我们的 wow64 改动（HODLL、4G 下限等）要在它之上重放 |
| winewayland 基线 | `dlls/winewayland.drv/*` 存在且被我们改过 | 上游文件也会漂移，必须逐个人工重放 |

**但注意一个方向性问题**：Valve 是 **11.0**、我们是 **11.10**。
把 Core 换成 Valve，在上游版本上是**倒退**——`signal_arm64ec.c`、`wow64`、
`ndr_stubless.c` 这些文件在新上游里都有 2026 年的改动。
W0 的结论因此包含一条：**W1 不是"免费获得 Proton 的修复"，而是"用 Proton 的补丁集换掉 10 个月的上游进展"**，
两边都要按 Gate 实测，不能只看版本号。

## 5. 问题 2/3 的回答（摘要，明细见台账）

- **必须迁（NEED_PORT / AUDIT_AND_PORT）**：OHOS 平台层的全部 40 个新文件，
  加上 61 个上游文件里与它们对接的注入点。
- **应删除而不是迁移（DROP_IF_REDUNDANT）**：调试/清理类提交（多笔 `清理:`、
  临时 BMP dump、OHOS-DBG 日志、WoW64 诊断日志）。
- **必须在新基线上重新复现后再定（REVALIDATE / REVALIDATE_LAST）**：
  `WINEHUA_WINDOWS_VERSION`（移植自 CrossOver）、explorer/shell 细节、
  以及所有游戏专用 workaround。

## 6. 本文件的边界

- 只做了**文件级**对照；`BOTH` 的 61 个文件里，WineHua 到底改了哪些行、
  与 Valve 的对应文件差异多大，**没有**逐个人工比对（W1 抽样）。
- 没有验证 Valve 树在 ARM64EC/OHOS 方向上的可编译性（那是 W1 的构建风险，见 `build-risk-map.md`）。
- 数字会随两个仓库推进而变化，重跑 `tools/wine-delta-audit.sh` 即可刷新。
