# Proton-Wine-OHOS 首次真机启动成功（2026-09-12）

> 分支：`thirdparty/wine-valve` 的 `ohos-port`（基线 `dc26e618` = ValveSoftware/wine proton_11.0）
> 设备：MLR-AL10（HarmonyOS 7.0.0.105），无线 hdc `192.168.180.71:36875`
> 结论：**Proton 基础 + WineHua OHOS 平台层的运行时，已经在真机上完成 wineboot 初始化并起桌面**

## 1. 路线（用户裁定：以 Proton 为基础）

- 放弃"把 Proton 补丁倒灌进主库 11.10"的合并方案：实测 134 个文件冲突、207 个文件
  变成"半新半旧"的混合体（`server_protocol`、win32u 私有头、driver 接口全部错位），
  编译都过不去。证据留在 `thirdparty/wine-proton`（分支 `proton-wine-merge`）。
- 采用：**Valve proton_11.0 为基线**，把主库的 WineHua 提交逐个重放（110 个），
  再用"以主库改动前的版本为基线"的三方合并，把 OHOS 增量精确叠上去（30 处冲突手工解）。

## 2. 真机上卡死过的三个沙箱问题（全部已修）

设备侧 app 沙箱用 seccomp 过滤 syscall，命中即 **SIGSYS(signal 31)** 秒杀进程。

| # | 现象 | 根因 | 处理 |
| --- | --- | --- | --- |
| 1 | `wine: could not exec the wine loader` | Valve 树没有我们 loader 的 `__OHOS__` 守卫 → `reexec_loader()` 去 exec `wine-preloader` | 随平台层重放已修（W-05） |
| 2 | wineboot 起来 50ms 后进程消失，无任何输出 | `dlls/ntdll/unix/virtual.c` 的 `kernel_writewatch_init()` 调 `userfaultfd`（Valve 的写监视特性），沙箱禁止 | `USE_UFFD_WRITEWATCH` 增加 `!defined(__OHOS__)` | 
| 3 | wineserver 启动 13ms 后 SIGSYS，wineboot 报 `wine server failed to run` | `server/fsync.c` 的 `fsync_check_support()` **无条件**探测 `futex_waitv`（syscall 449），沙箱禁止 | 改成"只有显式设 `WINEFSYNC` 才探测"，且 `#if !defined(__OHOS__)`；`dlls/ntdll/unix/fsync.c` 同款守卫 |

定位手段（已固化在代码里，后续排障可复用）：`server/main.c` 的 `sigsys_diag()`
捕获 SIGSYS 后把 `si_syscall/si_arch/si_code` **直接写进 prefix**
（`$WINEPREFIX/.winehua-wineserver-sigsys.log`）——stderr 走的是 app 侧管道，
进程一死管道内容会丢，必须落盘。

首次报出：`SIGSYS syscall=449 arch=-1073741641 code=1` → aarch64 `__NR_futex_waitv`。

## 3. Wine Mono：不打包（对齐产品做法）

- 症状：wineboot 卡在"Wine Mono 安装器"窗口（app 已把它上屏），60s 超时判失败。
- 根因：我们的包把 `share/wine/mono/wine-mono-11.1.0-x86.msi`（83MB）打进去了，
  wineboot 首启会跑交互式安装器；无头/无人应答 → 永久阻塞。
- 产品工作树的 `staging/wine-data/share/wine/mono/` **是空的**，即产品本来就不装 mono。
- 处置：`assemble.sh` 已有开关，改用 **`BUILD_WINE_MONO=0`**（同时不带 `.cpl`，
  避免 `mscoree` 走 `control.exe appwiz.cpl install_mono` 的模态阻塞）。
  产物从 288MB 降到 204MB。

## 4. 真机验证结果（2026-09-12 22:45）

```text
prefix:      system32 811 个文件；system.reg / user.reg / userdef.reg / dosdevices 全部生成
完成标记:    .winehua-wineboot-init-status / .winehua-init-in-progress 被 app 成功路径 unlink
桌面:        DesktopAbility 前台化；explorer 会话带完整图形栈拉起
             （virgl + guest_gfx + VTEST + DXVK legacy + venus ICD）
合成器:      [MW-COMMIT] toplevel #3 frame 1280x20 / #5 frame 640x480 stored=1228800
             （640*480*4，真实位图提交上屏）
进程:        wineserver + wineboot + explorer 链路共 20 个 wine 进程
```

结论：**broker / compositor 链路本身没有问题**（它甚至把 mono 安装器窗口正确上屏了），
之前"桌面没起来"纯粹是 wineboot 被 mono 安装挡住、没走到 explorer。

## 5. 复现构建（容器 `wineohos-build` 内）

```bash
cd /data/src/winehua
WINE_SRC=/data/src/winehua/thirdparty/wine-valve bash scripts/w1-m4-build-merged.sh   # 编译
BUILD_WINE_MONO=0 bash scripts/w1-m2-assemble.sh                                      # 打包（不带 mono）
bash scripts/w1-m3-build-hap.sh                                                       # 出 HAP
```

> 注意：`w1-m2-prepare-assemble.sh` 里的产品树路径在容器内是 `/data/prod`
> （已改成 `PROD="${PROD:-/data/prod}"` 并回退宿主路径）。

## 6. 下一步

1. Gate W0：`cmd.exe` / `reg.exe` 能不能跑、窗口输入是否正常；
2. 桌面交互：鼠标/键盘/窗口管理（合成器 ↔ winewayland 私有扩展）；
3. 再往上是音频、网络 TLS，然后才是 Steam 客户端。
