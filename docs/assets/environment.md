# 环境资产

> 适用场景：换机器搭环境、换构建目标、找不到某份证书或某个配置时。
> 最后核实：2026-09-24
> 相关文档：[../build/env.md](../build/env.md)（从零搭环境的完整步骤）、[../build/guide.md](../build/guide.md)（构建流程）、[../build/release.md](../build/release.md)（打包发版）

搭建步骤本身（装哪些系统包、Python 包、怎么修 SDK 的兼容问题）在 [../build/env.md](../build/env.md)，本文只列**有哪些东西、在哪里**。

## SDK 与工具链

| 项 | 位置 | 说明 |
|---|---|---|
| OHOS SDK | `/apps/harmony/sdk/default/openharmony` | 默认路径，可用环境变量 `OHOS_SDK` 覆盖 |
| 工具根目录 | `/apps/harmony` | `TOOL_HOME`，`hvigorw`、`ohpm`、`node` 都在它的 `bin` 下 |
| 交叉编译工具 | `$OHOS_SDK/native/llvm/bin/` | `clang`、`llvm-ar`、`llvm-strip` |
| SDK 头文件与库 | `$OHOS_SDK/native/sysroot` | 交叉编译用 |
| 宿主 wayland-scanner | `/usr/local/bin/wayland-scanner` | 预装，或者由依赖构建脚本生成 |

SDK 自带的链接器有个已知的兼容问题（它依赖 `libxml2.so.2`，而新系统里只有更高版本），修法在 [../build/env.md](../build/env.md) 的「libxml2 兼容性修复」一节。

### 目标架构

用 `NATIVE_ARCH` 选：

| 值 | 用途 |
|---|---|
| `arm64-v8a` | 真机（平时用这个） |
| `x86_64` | 模拟器、x86_64 设备 |
| `all` | 双架构 HAP，只用于打包 |

Wine 那一层（模拟 x86_64 的部分）目标固定是 `x86_64-linux-ohos`，不随这个开关变。

### 构建环境容器

仓库里有现成的容器定义，换机器时不用从零装：

- 根目录 `Dockerfile`（基础镜像 + 依赖）
- `ci/Dockerfile.buildenv`（CI 用的构建环境）
- CI 实际用的是私有镜像 `ghcr.io/panedioic/winehua-buildenv:test`

## 签名材料与配置（`.ohos/` 目录）

这个目录被版本管理忽略，**不会提交，也不会被误删**——但它也意味着换机器要自己带过去。

| 内容 | 用途 |
|---|---|
| 调试签名材料（`.p12` / `.cer` / `.csr` / `.p7b` 各一份） | `keyAlias` = `debugKey` |
| `material/` | 调试签名材料对应的密码文件（DevEco 的那套加密格式） |
| `release/` 下的发布签名材料（`.p12` / `.cer` / `.csr`） | 与同目录下的 `.p7b` profile 配套，`keyAlias` 见 `build-profile.json5` |
| `release/` 下的 profile（`.p7b`） | 发布用的 profile |
| `release/material/` | 发布签名材料对应的密码文件 |

**签名由构建工具按配置自动选，不需要手工签**：`hvigorw` 的签名任务会读根 `build-profile.json5` 里 `signingConfigs` 指的那套材料。配置里写 debug 材料就产出调试签名，写 release 材料就产出发布签名。

两条证书链的根证书和中间证书相同，**只有叶子证书不同**（发布是 `...,Release`，调试是 `...,Development`），所以验证签名时要看到叶子证书才能区分——具体命令见 [../build/release.md](../build/release.md)。

## build-profile 的几个版本

根目录的 `build-profile.json5` 是**单一文件、被版本管理忽略**，但不同分支需要的格式不一样，所以 `/.ohos/` 下存了几份备份：

| 文件 | 什么情况下用 | 特征 |
|---|---|---|
| `.ohos/build-profile.json5` | `master` / `main-ui` 的日常构建 | SDK 版本写 `"6.1.0(23)"`（带引号的字符串），`runtimeOS` = `HarmonyOS`，调试签名 |
| `.ohos/build-profile.debug.json5` | 同上，另一份备份 | 与上面内容相同 |
| `.ohos/build-profile.release.json5` | 打发布包 | 发布签名，构建模式只有 release |
| `.ohos/build-profile.ohos.json5` | `kaihong` 分支的构建 | SDK 版本写 `23`（数字），`runtimeOS` = `OpenHarmony` |

用错格式的报错是 `unable to update targetSdkVersion`：构建脚本按带引号的形式去匹配版本号，数字形式匹配不上。

**切换之前先把当前那份存回 `.ohos/`**，因为这个文件不在版本管理里，覆盖了就找不回来。

## 构建用的网络代理

构建的依赖阶段要从国外源下载压缩包，直连很慢（实测有些源只有十几 KB/s，看起来像卡死）。解决办法是在启动构建**之前**设置 HTTP 代理：

```bash
export http_proxy=http://<局域网代理地址>:<端口>
export https_proxy=http://<局域网代理地址>:<端口>
make NATIVE_ARCH=arm64-v8a
```

代理必须在 `make` 启动前设置好——它是子进程读的环境变量，构建跑起来之后再改已经晚了。

判断是不是卡在下载上：看 `ps -ef | grep curl`，以及构建日志里下载进度条的速度。

## CI

`.github/workflows/build.yml`，触发条件是推送到 `master` / `main-ui`、打 `dev-*` 或 `rc-*` 的标签、提 PR，也可以手工触发。

几个要了解的点：

- **CI 里构建的是未签名的包**（它写一份空的签名配置，让构建工具跳过签名），产物名字里带 `-unsigned`。
- CI 会检查打进包里的 `wine-data.zip`，确认里面的图形后端清单和 submodule 的实际提交对得上，避免"代码更新了但包里还是旧的"。
- 打 `dev-*` 标签的版本不占「Latest」标记，`rc-*` 的占。
