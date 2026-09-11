# OHOS 移植预检（§12）

> 采样：2026-09-11，本机只读检查。**设备侧项目全部未取证。**
> 原则：先盘点，再补测试，最后修有证据的缺口。不为"完成表格"重写平台层。

## 分级

| 级别 | 含义 | 本轮处理 |
| --- | --- | --- |
| G0 | 核心阻塞（native 不能执行、UnixLib 不能加载、地址被覆盖、进程/异常语义错误、prefix 初始化失败） | 不能进入有效性能结论 |
| G1 | 观测阻塞（统计不可读、版本/产物不清、配置不可确认） | 可做隔离烟测，不可宣布 parity 完成 |
| G2 | 可选差异（硬件 TSO、内核原子加速不可用但回退已验证） | 记录实际状态，可继续 |

## 矩阵

| ID | 检查范围 | 级别 | 现状 | 已有实现入口 | 证据 / 待办 |
| --- | --- | --- | --- | --- | --- |
| OHOS-01 | 源码与产物闭环 | G0 | `pass_existing`（部分） | — | 已记录 HEAD / 子模块 / 哈希。**注意 `thirdparty/fex` 为脏**，二进制不能只由 `86ff33bbe` 复现 → 需记录其 diff |
| OHOS-02 | 工具链与 ABI | G0 | `not_tested` | `scripts/build_fex.sh` 内含 `llvm-readobj` 架构断言 | 需产出"每个 DLL/ELF 的 target/PE machine/ELF machine"清单 |
| OHOS-03 | 原生库加载 | G0 | **`pass_new`** | `build/fex-unixlib/*.so` → `entry/libs/<arch>/` | 真机确认：探针构造函数在两个 wine 进程中触发，证明 `load_unixlib_by_name` dlopen 成功 |
| OHOS-04 | 启动上下文（argv/env/auxv） | G0 | `not_tested`（实现已存在） | `entry/src/main/cpp/wine/env_profiles.cpp`、`proc/wine_child.cpp` | 中文/空格/空参数、两个实验配置不串环境 |
| OHOS-05 | 子进程与 FD | G0 | `not_tested`（实现已存在） | `dlls/ntdll/unix/ohos_broker.c` + broker 中继 | 真实 readiness / 退出码 / 失败回收 |
| OHOS-06 | 路径与 prefix | G0 | `not_tested`（实现已存在） | `ohos_file.c`、Wine prefix 管理 | 隔离根目录、空 prefix 首启、盘符语义 |
| OHOS-07 | 低地址与固定映射 | G0 | `not_tested`（有专门工作） | Wine `win32u/vulkan.c` wow64 remap、`ohos_virtual.c` | 冲突哨兵不覆盖原页；返回码真实 |
| OHOS-08 | native / JIT / guest 权限 | G0 | `not_tested`（实现已存在） | `ohos_virtual.c`（`ohos_map_exec_section` / JIT enable） | 分别验证 native ELF、native PE/ARM64EC、FEX JIT |
| OHOS-09 | 信号与异常 | G0 | `not_tested`（实现已存在） | `signal_arm64ec.c`、`ohos_virtual.c` sigchain | 受控 SEH / 保护页 / 无 SIGSEGV 循环 |
| OHOS-10 | libc / pthread 语义 | G0/G2 | `not_tested` | Wine OHOS 适配 + musl 侧 | 按实际用量测 typedef / 锁 / TLS / 条件时钟 |
| OHOS-11 | 内核可选能力（TSO / 未对齐原子） | G2 | **`unsupported_with_verified_fallback`** | UnixLib 已接通并实测 | `PR_GET_MEM_MODEL`→`0xffffffff/errno=22`，`PR_ARM64_SET_UNALIGN_ATOMIC`→`-1/errno=22`；回退为软件 TSO |
| OHOS-12 | FEX 正式统计 | G1 | `not_tested` | UnixLib 内 SHM 实现已回移 | 仍需编入 profiler（当前 `ENABLE_FEXCORE_PROFILER=OFF`）+ 配置文件 + 打包归位 |
| OHOS-13 | 打包与部署 | G0/G1 | `not_tested` | `Makefile` / `scripts/assemble.sh` | 文件清单 / SHA256 / 架构 / 资源版本；最终 HAP 与测量记录对应 |
| OHOS-14 | 真实窗口与测量边界 | G0 | `not_tested` | 既有 Wayland compositor + presenters | 需要真机；记录分辨率/画质/场景/同步/缓存/日志开关 |

## 单项记录样例（§12.4 格式）

```yaml
id: OHOS-12
status: blocked
required_for: observability
reference: A13/S13/S16
existing_implementation: thirdparty/fex FEXCore/Utils/SHMStats.cpp (86ff33bbe)
runtime_manifest: docs/proton-parity/runtime-manifest.json
process_role: ncp
os_build: null
sdk_api: null
probe: shm_create_read_grow_cleanup
actual_return: null
errno: null
ntstatus: null
fallback: null
expected_semantics: null
evidence_path: docs/proton-parity/wine-fex-interface-audit.md
change_required: 需要 FEX UnixLib 通道 + ENABLE_FEXCORE_PROFILER 编入 + 生效配置
```

```yaml
id: OHOS-11
status: not_tested
required_for: optional
reference: S13/S14
existing_implementation: null
runtime_manifest: null
process_role: ncp
os_build: null
sdk_api: null
probe: prctl_get_set_mem_model
actual_return: null
errno: null
ntstatus: null
fallback: null
expected_semantics: 启用成功才标记硬件 TSO 可用；失败返回真实错误
evidence_path: null
change_required: 依赖 FEX UnixLib 接入
```

## 允许状态

`not_tested` / `pass_existing` / `pass_new` / `unsupported_with_verified_fallback` /
`not_applicable` / `blocked`。空值表示尚未取证。

## 第一轮应提交 / 不应提交（§12.5）

**应提交：** 固定参考快照、现有能力盘点、关键平台最小测试结果、必要的可回退小补丁、
四个 FEX 配套产物的加载证据、有版本的正式统计样本。

**不应提交：** 参考仓库补丁整包合并、HSP 同进程游戏架构、私有二进制 patch 管道、
假的启动/等待/窗口成功、一次性重做所有可选功能。
