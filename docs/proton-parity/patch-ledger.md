# 补丁台账（§8）

> 目的：从第一天起就分层记录差异，避免"等性能达标再整理"。
> 规则：新增源文件必须进版本控制并参与构建输入哈希；稳定分支不直接被覆盖。

## 分层定义

| 层 | 范围 | 允许进产品或仅实验 |
| --- | --- | --- |
| L1 Wine OHOS 平台适配 | `thirdparty/wine/dlls/.../ohos_*.c`、`signal_arm64ec.c`、`loader.c` 的 HODLL/overlay | 产品补丁（需回归） |
| L2 FEX UnixLib OHOS 适配 | FEX `Source/Windows/UnixLib/*`、`Common/FEXUnixLib.*` | 待评估 |
| L3 DXVK / vkd3d 设备兼容补丁 | `thirdparty/dxvk*`、`thirdparty/vkd3d-proton` | 本轮不动 |
| L4 打包 / prefix / 运行时选择 | `Makefile`、`scripts/assemble.sh`、`wine_env*.cpp`、`setup/` | 分层提交 |
| L5 临时诊断与实验 | `build-logs/`、`docs/proton-parity/`、一次性脚本 | 不进产品包 |

## 台账

| ID | 层 | 文件 / 位置 | 变更原因 | 影响组件 | 依赖版本 | 验证方法 | 回退方式 | 保留为产品补丁 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| L1-01 | L1 | `thirdparty/wine/dlls/ntdll/loader.c`（HODLL/HODLL64、ARM64X overlay 搜索） | OHOS 下按环境变量选择 CPU DLL | Wine ntdll | wine `dc5204ecb` | 真机启动日志 `[WineChild] final` | 撤销覆盖搜索顺序 | 是 |
| L1-02 | L1 | `thirdparty/wine/dlls/ntdll/unix/ohos_virtual.{c,h}` | sigchain / SMC / PROT_EXEC 恢复 | Wine ntdll unix | 同上 | 受控 SEH / 保护页 / SMC 回归 | 关闭 ohos_virtual 挂钩 | 是 |
| L1-03 | L1 | `thirdparty/wine/dlls/ntdll/unix/ohos_{broker,file}.{c,h}` | NCP 子进程与文件语义 | Wine ntdll unix | 同上 | OHOS-05 / OHOS-06 | 回退到通用 unix 路径 | 是 |
| L2-01 | L2 | `thirdparty/fex` → 升级到 `1cc4b93e` 或回移 UnixLib 增量 | 产生两个 AArch64 UnixLib、接通硬件 TSO / SHM 统计 | FEX CPU DLL + UnixLib | FEX `1cc4b93e` | `readelf -h` 断言 AArch64；运行时加载证据 | 保持 `86ff33bbe` 不合并 | 待定 |
| L2-02 | L2 | `scripts/build_fex.sh` 新增 UnixLib 构建目标 | 当前脚本只产两个 PE DLL | 构建层 | llvm-mingw 20260826 + 原生 clang | 产物存在 + ELF machine + 符号表 | 删除新增目标 | 是 |
| L4-01 | L4 | `scripts/assemble.sh` / `Makefile` 打包 UnixLib `.so` | 运行时包需要 Unix 侧库 | 打包层 | — | 解包核对文件名/路径/哈希 | 还原打包清单 | 是 |
| L4-02 | L4 | `scripts/build_fex.sh` 构建目录隔离（`fex-ec`/`fex-pe` → `build/parity/*`） | 旧 CMakeCache 让 `-DBUILD_TESTING=False` 失效 | 构建层 | — | 复核 CMakeCache 每项 | 还原目录名 | 是 |
| L4-03 | L4 | 新增 FEX 参考配置文件（Proton 参考配置） | 当前无配置文件，默认 MaxInst=5000 | 配置层 | FEX 版本而定 | 导出配置文件哈希 + 进程生效值 | 删除配置文件 | 待定 |
| L5-01 | L5 | `docs/proton-parity/*` | 本阶段记录 | 文档 | — | 人工评审 | 丢弃目录 | 否 |

## 禁止混在同一次"FEX 提速"提交里的变更

- CPU 后端升级 + Host 展示 / 同步策略变化
- Wine 接口补齐 + 未经验证的 GPU 能力宣告
- 统计接口修复 + 额外启动线程 / 异步执行流改造
- 架构变更 + 无法复现的手工替换 DLL

## 本次（第一轮）只提交

`docs/proton-parity/` 下的记录文件。**不改产品代码，不改子模块指针。**
