# 不确定点和技术风险

> 更新: 2026-06-22

---

## P0 — 已验证解决 ✅

| # | 问题 | 结论 |
|---|------|------|
| U01 | OHOS 是否支持 x86_64 目标 | ✅ `x86_64-linux-ohos` 完整可用 |
| U02 | `arch_prctl` syscall | ✅ 头文件完整，运行时正常 |
| U03 | OHOS NDK 可用性 | ✅ Clang 15.0.4 + LLD |
| U04 | `program_invocation_name` 缺失 | ✅ 已 patch |
| U05 | `dladdr1`/`dlinfo` 双缺失 | ✅ 已 patch |
| U06 | `libc.so.6` 硬编码名称 | ✅ 已修复 |
| U15 | Box64 fork 子进程 RWX 失败 | ✅ NCP appspawn + .so dlopen 替代 fork |

---

## P1 — 待验证

### U11: `/proc` 文件系统兼容性
- **问题**: Wine 读取 `/proc/self/maps`, `/proc/self/pagemap`, `/proc/cpuinfo` 等
- **状态**: 🟡 部分验证，Box64 已处理主要差异

### U16: Box64 dlopen 原生 .so 失败
- **问题**: `libfreetype.so.6`, `libxkbcommon.so.0`, `libxkbregistry.so.0`, `libxml2.so.2` 在 Box64 下找不到
- **影响**: 矢量字体回退到位图，键盘输入部分功能受限
- **状态**: 🟡 非致命，需调查 BOX64_LD_LIBRARY_PATH

### U17: xkbcommon include path 错误
- **问题**: `xkbcommon: ERROR: failed to add default include path /usr/share/X11/xkb`
- **影响**: XKB 数据查找失败，应指向 rawfile 解压路径
- **状态**: 🟡 非致命

---

## P2 — 低优先级

### dnsapi / nsiproxy
- **问题**: musl 不兼容 (`_res` 全局变量, `sys/socketvar.h`)
- **状态**: `make -k` 跳过，不影响基础功能

### services.exe
- **问题**: `start_services_process error 267`
- **状态**: 不影响基础应用运行

### 音频
- **问题**: 未实现
- **状态**: 待开发
