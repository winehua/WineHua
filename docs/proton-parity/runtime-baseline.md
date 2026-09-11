# 运行时基线冻结（P0）

> 采样时间：2026-09-11（本机，未接设备）
> 目的：让后续每一个对比结果都能追溯到明确的源码版本、构建参数、产物与运行配置。

## 1. 冻结对象

| 对象 | 位置 | 状态 |
| --- | --- | --- |
| 产品源码工作树 | `/home/liufeng/src/WineHua-arm64ec` | 保持不动（含未提交改动） |
| 对齐实验工作树 | `/home/liufeng/src/WineHua-proton-parity` | 本次新建，干净 |
| 现有 HAP | `WineHua-arm64ec/entry/build/default/outputs/default/` | 只读引用 |
| FEX 现有产物 | `WineHua-arm64ec/build/fex-{ec,pe}/Bin/` | 只读引用 |
| 运行时包 | `entry/src/main/resources/rawfile/wine-data.zip` | 已记录哈希 |

## 2. 源码基线

### 2.1 主仓库

```text
路径      /home/liufeng/src/WineHua-arm64ec
分支      feature/arm64-heaven-port
HEAD      2728523fe1aaaa2509d3e22dc73923dfdf23461f
上游      origin/feature/arm64-heaven-port（github.com/winehua/WineHua）
工作树    脏（见 2.3）
```

与审查方案 §2.1 的 `feature/arm64-heaven-port@2728523` 一致。

### 2.2 子模块提交（工作树实际检出值）

| 子模块 | 提交 | describe / 备注 |
| --- | --- | --- |
| `thirdparty/wine` | `dc5204ecb0c3c1bf6624542ecf42f931fcc1a3b0` | 无 tag；`fix(ntdll): search arm64x DXVK overlay before x64` |
| `thirdparty/box64` | `16515448b24ccbec20ce477a8f6b6e88e00f0176` | 分支 `feature/arm64-heaven-port` |
| `thirdparty/fex` | `86ff33bbe299cd8959a6610198c169b67ec419db` | `FEX-2605-2-g86ff33bbe` |
| `thirdparty/mesa` | `2939cbb816d3a3c5fcf3147fb8436e105d0a0ee8` | `origin/feature/vkd3d-capability-probe` |
| `thirdparty/dxvk` | `f6dd62e6dc4bb62a3aaaa4fad824e6cef5655afc` | `v1.10.3-34-gf6dd62e6` |
| `thirdparty/dxvk-modern` | `05a0a66d74cc41e6a90a8cb62d459c1c6a28c783` | `v2.6.2-4-g05a0a66d` |
| `thirdparty/virglrenderer` | `05d4ded5c4bbd46078676a3f423454abaad90d27` | |
| `thirdparty/vkd3d-proton` | `3e5aab6fb3e18f81a71b339be4cb5cdf55140980` | |

### 2.3 未提交改动（必须保留，不得清理）

主仓库 `M`：

```text
entry/src/main/cpp/graphics/venus_surface_presenter.cpp
entry/src/main/cpp/proc/wine_child.cpp
entry/src/main/ets/game/GameHook.ets
entry/src/main/ets/service/WineEnvService.ets
scripts/build_ohos_guest_vulkan.sh
scripts/build_wine.sh
```

子模块脏（`m`）：`dxvk`、`fex`、`glib`、`gmp`、`gstreamer`、`libtasn1`、`libunistring`、
`mesa`、`pcre2`、`wine`。

未跟踪：`build-hap-arm64ec-*.log`、`build-logs/`、`docs/HEAVEN_MASTER_ARM64EC_DIFF_ANALYSIS.md`、
`thirdparty/dxvk-modern`（子模块目录尚未登记）。

> `thirdparty/fex` 为脏状态，说明现有 FEX 二进制**不能**只用 `86ff33bbe` 复现；
> 复现必须先 `git -C thirdparty/fex status` 记录本地改动内容。

## 3. 构建环境

| 组件 | 版本 / 位置 | 备注 |
| --- | --- | --- |
| 构建容器 | Docker 镜像 `winehua-dev`（`0a010ac5c5db`） | 产物属主为 root，说明实际在容器内构建 |
| HarmonyOS 命令行工具 | `command-line-tools 6.1.1.290` → 容器内 `/apps/harmony` | `OHOS_SDK=/apps/harmony/sdk/default/openharmony` |
| 容器内 cmake / meson / ninja | `4.2.3` / 有 / 有 | |
| 宿主机 cmake / python | `3.22.1` / `3.10.12` | 宿主机 `ninja` 落到 depot_tools 包装器，不作为构建入口 |
| llvm-mingw | `.temp/llvm-mingw-20260826-ucrt-ubuntu-22.04-x86_64` | `env.sh` 默认 `LLVM_MINGW_VERSION=20260826` |
| llvm-mingw（旧） | `.temp/llvm-mingw-20260616-...` | 保留在目录中，勿混用 |
| hdc | **宿主机不可用** | 真机操作见 `docs/REMOTE_HDC.md`（远程 hdc server） |

构建入口：仓库根目录 `Makefile`（唯一入口），宿主机侧编排在 `setup/`
（`source setup/proxy_env.sh && bash setup/ci_build_hap.sh` 经容器执行）。

> 注意：`setup/` 不在 git 跟踪范围内（被 `.gitignore` 忽略），新工作树里没有它，
> 需要时从原工作树复制，不要提交。

## 4. 现有产物

| 产物 | 大小（字节） | 时间 |
| --- | --- | --- |
| `entry-default-signed.hap` | 403 590 755 | 2026-09-09 17:10 |
| `entry-default-unsigned.hap` | 402 069 148 | 2026-09-09 17:10 |
| `build/fex-ec/Bin/libarm64ecfex.dll` | 40 300 544 | 2026-09-08 07:33 |
| `build/fex-pe/Bin/libwow64fex.dll` | 40 972 288 | 2026-09-09 16:56 |
| `wine-data.zip`（rawfile） | 见 `runtime-manifest.json` | |

哈希见 `artifact-sha256.txt`。

## 5. 测量上下文

**本轮未取设备数据。** 已有性能数字（20.39 FPS / 8.3 FPS / `hardware_tso=false`）
均为历史口述值，按方案 §2.3 只作为待复核输入。

设备侧基线（进程位数、CPU 后端、DXVK 档、Guest Vulkan 架构、prefix 状态、
分辨率 / 画质 / 场景 / 同步 / 冷热缓存 / 日志开关）需要在接设备后按
`ohos-preflight.md` 的 `OHOS-14` 补齐。

## 6. 证据边界

- 本文所有结论来自**本机只读检查**，不含真机复测。
- 原工作树脏改动内容未逐行审计，只记录了文件清单。
- `thirdparty/fex` 的本地未提交改动未展开；在 P2 复现前必须先记录其 diff。
