# 第三方 OHOS 参考采用决定（§11 / §12.5）

> 参考仓库：`caidingding233/aetherium-lite-runtime-using-wine-version-not-the-qemu-emulator-version`
> 固定提交：`8bfb459bb71400eb759a35e790d217615174febc`
> 本地是否可得：**否**（本机未克隆；判断依据为方案 §2.4 / §11 的静态审查）

## 采用规则

复用「问题识别与适配边界」，**不复制另一套运行架构**。参考代码暴露的坑，先在
WineHua / VintagePomeloPro 里查已有解决方案；确有缺失才在对应层做最小修改。

## 逐项决定

| 参考项 | 参考做法 | 本架构落点 | 决定 | 理由 |
| --- | --- | --- | --- | --- |
| 库加载 / `dlopen` 顺序 | loader 内 dlopen/dlsym + 诊断 | 现有启动与打包层、Wine Unix 库加载边界 | `已存在且通过，不改代码` | WineHua 已有 `load_so_dll` / `__wine_unixlib_handle` 链路 |
| auxv / argv / env 所有权 | 平台 shim 传递 | NCP + broker | `已存在且通过，不改代码` | `setup_wine_env` / broker 转发已有 |
| SHM（`shm_open`） | 避免 Android 失败桩 | FEX UnixLib OHOS 适配 | `允许小范围源代码适配，等待测试` | 当前 FEX 版本尚无 UnixLib，需先解决版本问题 |
| libc / pthread 兼容头 | mutex 类型降级、信号别名 | OHOS ABI 适配层 | `只借鉴测试或故障线索` | 不把 Android/Bionic 宏分支当 OHOS ABI |
| 子进程 / FD / 退出 | 请求文件、假 PID、线程退出 | —— | `禁止照搬` | 破坏真实进程、FD 与退出状态契约 |
| ntdll 固定偏移补丁（skip wineboot / SIGSYS / uxtheme） | 二进制偏移修改 | —— | `禁止照搬` | 版本脆弱；不得当作可维护的源码补丁层 |
| `win32u` nodrv / `CreateWindow` 成功桩 | 跳过用户回调、返回成功 | —— | `禁止照搬` | 不代表真实窗口，不能用于性能验收 |
| `PROT_EXEC` 全局清除 / 先 unmap 再抢固定地址 | 全局处理 | —— | `禁止照搬` | 不适用于 native ARM / ARM64EC / JIT |
| Box64 汇总补丁整体套用 | 一揽子补丁 | —— | `禁止照搬` | 不整体应用；只按问题定位最小改动 |
| HSP 内同进程承载整套 x86_64 Wine | 运行架构 | —— | `不适用本架构` | 与 ARM Wine + FEX + ARM64EC 主线冲突 |
| 共享库作为 NCP 内加载形式 | `.so` 形式 | 可保留 | `允许` | "使用 .so" 与"把游戏放进 UI 进程"不是一回事 |
| rawfile 索引 / 打包清单 | 路径大小索引 | 打包层 | `只借鉴测试或故障线索` | 可参考跳过规则，不作为验收依据 |

## 结论

本轮**不引入**该仓库的任何代码或补丁。它只用于校验我们预检矩阵的覆盖面
（尤其 SHM、auxv、libc/pthread、信号、prefix 这几类坑）。任何实际改动都必须
来自本仓库可复现的源码修改与测试。
