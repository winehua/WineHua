# 开鸿首启崩溃：wineboot SIGSEGV(SEGV_ACCERR)（未解决）

> 状态：**未解决**，已排除一条假设。记录于 2026-09-12。
> 平台：KaihongOS 6.1 (OpenHarmony-6.1.0.35) / x86_64 真机 / deviceType='pc'

## 现象

全新安装应用后（`bm uninstall` → 装 → 首次启动），wineboot 建 prefix 约 8 秒后崩溃：

```
Process name: app.hackeris.winehua
Timestamp: 2026-09-12 19:12:26.459
Pid: 4584   Name: wineboot.exe   Process life time: 8s
Reason: Signal:SIGSEGV(SEGV_ACCERR)@0x00006ffffeadbbd0
LastFatalMessage: Failed to unwind stack, try to get unreliable call stack from #02 by reparsing thread stack.
```

同一时刻还有 `explorer.exe` ×2、`rundll32.exe` ×1 也 SIGSEGV。

之后 `winedbg` 接管并**挂住**，日志停在 `[Launch-Async] wineboot still initializing (240 s)`，
引擎始终起不来（崩溃计数稳定 5 次、`still initializing` 稳定 14 条）。

**二次启动正常**：force-stop 后重启应用，因 prefix 已就绪，`desktop root #1 appId=explorer.exe.desktop-shell`
与 `evt:desktop-ready` 都正常出现，进桌面、开始菜单均可用。

## 复现

```
hdc shell "bm uninstall -n app.hackeris.winehua"
hdc file send <hap> /data/local/tmp/winehua.hap && hdc shell "bm install -p /data/local/tmp/winehua.hap"
hdc shell "hilog -r"; hdc shell "aa start -a EntryAbility -b app.hackeris.winehua --ps winehua.autoStart 1"
# 等 ~150s，然后看 hilog 的 "CRASH pid=" / "desktop root:" / "still initializing"
```

崩溃报告：`/data/log/faultlog/temp/cppcrash-<pid>-<ts>`。
沙箱 stderr：`/data/app/el2/100/base/app.hackeris.winehua/temp/wine_stderr_<date>.log`。

## 已排除的假设

### ❌ `ohos_map_exec_section` 未设 PROT_EXEC（2026-09-12 实测无效）

**假设**：`thirdparty/wine/dlls/ntdll/unix/ohos_virtual.c` 的 `ohos_map_exec_section()` 只把可执行节
建成 `PROT_READ|PROT_WRITE` 匿名页、从不设 `PROT_EXEC`；调用方 `virtual.c` 传的保护
（`VPROT_COMMITTED|VPROT_READ|VPROT_WRITECOPY`）也不含 EXEC。两边都没有 → 可执行节实际不可执行 →
执行即 `SEGV_ACCERR`。

**验证**：在 `pread` 之后补 `ohos_mprotect_exec(..., PROT_READ|PROT_EXEC)`，
`touch ohos_virtual.c` 强制重编 wine，确认构建日志有 ntdll 编译记录、hap 重新打出后，卸载重装验证。

**结果**：崩溃次数与修复前**完全一致**（CRASH 5 次、`still initializing` 14），desktop root 依旧不出现。
→ 崩溃点不在这条路径上。改动已撤销。

## 下一步方向（均未验证）

1. **先定位地址**：`0x6ffffeadbbd0` 属于哪一段映射？wine 的 PE 映像通常在 `0x140000000`(64位)
   或 `0x400000`(32位)，该地址两者都不是 —— 当初直接跳到"PE 节权限"是找错了方向。
2. **栈溢出假设（优先）**：`SEGV_ACCERR` 与 `Failed to unwind stack` 同时出现，很像撞到栈 guard page。
   wine 建 prefix 时 rundll32 递归加载可能撑爆默认栈。可查崩溃线程 SP 是否贴近栈边界，
   或调大栈保留值对照测试。
3. **拿到真实栈**：`LastFatalMessage` 说栈无法 unwind —— 要么栈已损坏，要么 PC 处没有 FDE，
   两种都该单独查清，而不是继续靠地址特征猜。

## 排查陷阱（踩过的坑）

**stamp 假阳性会让"修复无效"的结论本身失真**：
第一版验证时 hap 里的 wine **根本没重编** —— `build/.stamps/wine-x86_64`(19:53) 比源文件(19:30) 新，
make 判定已最新直接跳过，而 stamp 是构建被打断后仍被写入的。

**规矩**：验证 wine 改动前，先 `grep <改动的文件名> <构建日志>` 确认它真被编译过，
否则会拿旧二进制验证新代码。另外**不要用 `pkill -f "make NATIVE_ARCH"` 停构建** ——
它匹配不到 wine 目录里的子 `make -j16`，会留下孤儿构建继续吃 CPU。
