# 不确定点和技术风险

> 本文档列出 Wine on HarmonyOS 移植的剩余不确定点。

---

## P0 — 已验证解决 ✅

以下初始阻塞问题已全部验证通过：

| # | 问题 | 结论 |
|---|------|------|
| U01 | OHOS 是否支持 x86_64 目标 | ✅ `x86_64-linux-ohos` 完整可用 |
| U02 | `arch_prctl` syscall | ✅ 头文件完整，运行时正常 |
| U03 | OHOS NDK 可用性 | ✅ Clang 15.0.4 + LLD |
| U04 | `program_invocation_name` 缺失 (vkd3d) | ✅ 已 patch |
| U05 | `dladdr1`/`dlinfo` 双缺失 | ✅ 已 patch |
| U06 | `libc.so.6` 硬编码名称 | ✅ 已修复 |

---

## P1 — 待验证

### U11: `/proc` 文件系统兼容性
- **问题**: Wine 读取 `/proc/self/maps`, `/proc/self/pagemap`, `/proc/cpuinfo` 等
- **影响**: OHOS 的 `/proc` 格式可能与标准 Linux 有差异
- **状态**: 🟡 部分验证，Box64 已处理主要差异

### U15: Box64 fork 子进程 RWX 失败
- **问题**: Box64 fork 子进程 (depth 2) 无法创建 PROT_EXEC 内存
- **影响**: wineserver 等子进程的 mmap(RWX) 调用失败
- **缓解**: wineserver 使用 NAPI 原生 fork 绕过

---

## P2 — 低优先级

### FreeType 矢量字体
- **问题**: Box64 dlopen ARM64 原生 `libfreetype.so.6` 失败
- **影响**: 只能使用 Wine 内置位图字体，无法使用系统矢量字体
- **状态**: 位图字体已足够基础使用

### dnsapi / nsiproxy
- **问题**: musl 不兼容 (`_res` 全局变量, `sys/socketvar.h`)
- **影响**: DNS 解析和网络状态查询功能受限
- **状态**: `make -k` 跳过，不影响基础功能

### services.exe
- **问题**: `start_services_process error 267`
- **影响**: Wine 服务进程无法启动
- **状态**: 不影响 notepad 等基础应用运行
