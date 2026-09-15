# 开鸿首启崩溃：wineboot SIGSEGV(SEGV_ACCERR)（已解决）

> 状态：**已解决**（2026-09-15）。根因 = OHOS 页权限在创建时确定，mprotect 加 X 无效。
> 平台：KaihongOS 6.1 (OpenHarmony-6.1.0.35) / x86_64 真机 / deviceType='pc'

## 现象（修复前）

全新安装应用后（`bm uninstall` → 装 → 首次启动），wineboot 建 prefix 约 8 秒后崩溃：

```
Process name: app.hackeris.winehua
Timestamp: 2026-09-12 19:12:26.459
Pid: 4584   Name: wineboot.exe   Process life time: 8s
Reason: Signal:SIGSEGV(SEGV_ACCERR)@0x00006ffffeadbbd0
```

同一时刻还有 `explorer.exe` ×2、`rundll32.exe` ×1 也 SIGSEGV。之后 `winedbg` 接管并**挂住**，
日志停在 `[Launch-Async] wineboot still initializing (240 s)`，引擎始终起不来。

## 根因

**OHOS 上页权限在 mmap 创建时确定：`mprotect(RW → RX)` 返回成功（0）但不生效，页保持 `rw-p`，
执行即 SEGV_ACCERR。**

证据链（2026-09-15）：

1. 崩溃 RIP `0x6fffff922ef0` 落在 `0x6fffff911000-0x6fffff95e000 rw-p`（rpcrt4.dll 的 .text 节），
   而 wine 的内存记账是 `c-r-x`（认为已设可执行）。
2. 应用内自检直接验证：对匿名页 `mprotect(RW→RX)` 后 `/proc/self/maps` 仍为 `rw-p`，真实调用即 SEGV；
   而 `mmap(RX)` → `r-xp`、`mmap(RWX)` → `rwxp` 都真实可执行。

wine 的 PE 加载流程中，可执行节由 `ohos_map_exec_section()` 用匿名页承载（OHOS 文件映射不能加 X），
随后 wine 的 `set_vprot(RX)` 靠 mprotect 加 X —— 这条路径在 OHOS 上不生效。

## 修复

`thirdparty/wine/dlls/ntdll/unix/ohos_virtual.c` 的 `ohos_map_exec_section()`：匿名 mmap 时
**直接带 `PROT_EXEC`**（`PROT_READ|PROT_WRITE|PROT_EXEC`）。页创建时即可执行，后续 wine 的
`set_vprot(RX)` 不生效也不影响（页保持 RWX）。

## 验证（2026-09-15）

| 场景 | 结果 |
|------|------|
| 修复前 + 残留 prefix | wineboot 卡死 240s+，25 份崩溃报告（10:04/10:07 两批） |
| 修复后 + 残留 prefix | wineboot 完成 (10 s)，explorer 崩 3 次后自愈，桌面就绪 |
| 修复后 + App 重置（doReset 升级卡） | explorer 崩 4 次后自愈，`state:ready`，桌面就绪 |
| 修复后 + 干净 prefix（`rm -rf .wine` 后冷启动） | **零崩溃**，约 20 秒完成首启 |

**验证首启前注意**：改动 wine 源码后重编会让 `wine-data.zip` 的 sha 变化，App 的
`upgradeNeedsAttention()` 会拦下自动解压并弹"Wine 引擎有更新"卡（避免新数据与旧 prefix 混用），
需点"立即应用"→ 二次确认"确定"才会完整重置。不点这一步引擎根本不会启动。

干净冷启动在 `WINEDEBUG=+seh,+process` 下复测（2934 行 trace）：`SIGSEGV`/`Unhandled`/`page fault`
关键词 **= 0**；`desktop root: #4 appId=explorer.exe.desktop-shell` 与 `evt:desktop-ready` 正常出现。

### 残留 prefix 场景的 3 次 explorer 退出

修复后若 prefix 处于"部分初始化"残留状态，wineboot 自己启动的 `explorer.exe /desktop` 会以
signal=11 退出 3 次（都死在加载 rpcrt4.dll 附近、位置不固定），第 4 个成功。这是**启动竞态**，
干净 prefix 不出现。未生成 cppcrash 报告（wine 自己的 SIGSEGV handler 覆盖了 faultloggerd 的
sigchain handler），wine stderr 也没有 `Unhandled exception` 回溯。

## 已知噪音（非故障）

wine stderr 里这条 ERR 是**预期噪音**，不影响功能：

```
err:virtual:map_image_into_view failed to set 60000060 protection on ... section .text, noexec filesystem?
```

原因：`mprotect(RWX→RX)` 在 OHOS 上返回非 0（假失败），wine 据此打 ERR；但页实际仍是 RWX（可执行），
且 `virtual.c` 对 `set_vprot` 失败走宽容路径（只打日志不中止）。

## 历史排查

### 第一次尝试（2026-09-12，无效）

在 `ohos_map_exec_section` 的 `pread` 之后补 `ohos_mprotect_exec(..., PROT_READ|PROT_EXEC)`，
结果与修复前**完全一致**（CRASH 5 次、`still initializing` 14）。当时据此判定"崩溃点不在这条路径"
是**错的**：那条路径在 OHOS 上本来就不可能生效 —— mprotect 给已存在的 RW 页加 X 无效，
所以补与不补都是同样结果，无法用作排除依据。

### 排查陷阱

- **stamp 假阳性**：验证 wine 改动前，先 `grep <改动的文件名> <构建日志>` 确认它真被编译过。
  第一版验证时 hap 里的 wine 根本没重编（stamp 比源文件新，make 直接跳过）。
- **产物验证要针对正确的库名**：`wine_child.cpp` 编进 `libwine_child.so`（不是 `libentry.so`）。
  查错库会得出"改动没生效"的错误结论（`.cxx` 路径里的 `CMakeFiles/wine_child.dir/` 已指明归属）。
- **不要用 `pkill -f "make NATIVE_ARCH"` 停构建** —— 匹配不到 wine 目录里的子 make，
  会留下孤儿构建继续吃 CPU。
