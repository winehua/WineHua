# 设备清单

> 适用场景：需要在真机上验证时、不确定某台设备有什么限制时。
> 最后核实：2026-09-24
> 相关文档：[../architecture/platform-ohos.md](../architecture/platform-ohos.md)（平台差异的原理）、[../build/release.md](../build/release.md)（打发布包）、[../cheatsheet.md](../cheatsheet.md)（部署命令）

## 先解决连接问题

设备的 IP 由路由器动态分配，**会变**。每次开始之前先看当前连了哪些：

```bash
hdc list targets                          # 看当前连了哪些
hdc tconn <ip>:<port>                     # 没连上就主动连一次
hdc -t <ip>:<port> shell "echo ok"        # 确认能通
```

文档里出现的地址只能当参考，不要照着敲。

> 注意：新版本的 `hdc -s` 是「设置本机 server 的监听配置」，不是「连到远程 server」。要连别的机器上的设备，用 `hdc tconn`。

## 按形态分的几类设备

各形态的行为差异（用哪条进程创建路径、默认什么交互方式）见 [../architecture/platform-ohos.md](../architecture/platform-ohos.md) 的「设备形态差异」一节。这里只列实机情况。

### 平板（`tablet`）

| 设备 | 系统 | 用途 | 要注意的 |
|---|---|---|---|
| 平板真机（arm64） | HarmonyOS | 平板形态的主验证机 | 沙箱写入宽松，主题、DLL 可以直接 `hdc file send` 进去，改这些用它最快 |
| OpenHarmony 2in1 真机（aarch64） | OpenHarmony，系统本身是 2in1，但上报的形态是 `tablet` | 验证平板路径 | uinput 的触摸和手写笔注入无效（只有 `uinput -M` 鼠标能用，且不支持负增量）；截图路径必须以 `.jpeg` 结尾；UI 注入不进去，要用 `aa start --ps` 的 want 参数方式 |

**形态上报值和系统本身可能不一致**（上面那台 2in1 就上报成 `tablet`）。写代码时按上报值走，不要按"它应该是什么"推断。

### PC / 2in1

| 设备 | 系统 | 用途 | 要注意的 |
|---|---|---|---|
| arm64 笔记本（`deviceType` = `2in1`） | HarmonyOS | 界面验证机 | 系统策略不允许 shell 写应用的数据目录（`hdc file send` 和 `hdc shell cp` 都会被拒，设备上也没有 `su`），要换沙箱里的文件只能重新打包安装 |
| 开鸿 PC（x86_64） | KaihongOS，`deviceType` = `pc` | 验证开鸿适配 | 系统禁用了 NCP，进程创建只能走 fork；不支持子窗口，交互固定虚拟桌面；`windowRectChange` 这类接口会报 801，代码里要逐点容错 |

开鸿 PC 的 `deviceTypes` 在 `module.json5` 里没有单独声明 `"pc"`，但实际设备会上报这个值，代码里判断形态时要考虑进去（`kaihong` 分支的做法是判 `2in1` 和 `pc` 之外的都算子窗口承载形态）。

### 手机（`phone`）

走 fork 后端，交互固定虚拟桌面（设置里不显示模式切换），启动时打开自动旋转。可以设 `BUILD_WINE_MONO=0` 关掉 wine-mono 来缩小包体积。

### 模拟器（x86_64）

**它把宿主机的鼠标合成为触摸事件，右键和中键不透传**（拿不到 `MouseButton.Right`）。所以：

- 涉及右键、中键的功能（比如桌面模式的右键直通）**只能在带物理鼠标的平板上验证**，在模拟器上验证不了。
- 模拟器连接的远程 hdc server 地址也是会变的，用之前先确认它还在。

## 部署到设备

```bash
make NATIVE_ARCH=arm64-v8a hap         # 构建
bash scripts/package.sh deploy <ip>    # 安装（内部会 tconn + 卸载 + 推送 + 安装）
```

改过 Wine 的话还要清掉设备上的引擎数据，让它重新解压：

```bash
hdc -t <ip>:<port> shell "rm -rf /data/app/el2/100/base/app.hackeris.winehua/files/.wine \
                                /data/app/el2/100/base/app.hackeris.winehua/files/wine"
```

改 Wine 之后**不一定要清数据**：只改了 Unix 层的运行时逻辑（比如进程创建那部分）可以直接复用原来的环境，省掉一次重新解压的等待；改动涉及 PE 侧的 DLL、注册表内容、`wine.inf` 或者 Wine 版本号时才需要清空重建。

### 安装被拒：版本降级

如果设备上装的版本比要装的包版本高，安装会失败：

```
error: install version downgrade (code:9568263)
```

**部署之前先确认分支和版本对得上**：设备上装的是哪个分支的构建，就在哪个分支构建。版本号在 `AppScope/app.json5`（不在 `entry/src/main/module.json5` 里）。

### 装包签名和设备状态

- 设备上装的是**调试签名**的包时，可以直接覆盖安装，数据保留。
- 设备上装的是**发布签名**的包（比如应用市场版本）时，本地调试包装不上，必须先卸载——卸载会清掉应用数据（包括 Wine 环境）。
- 装了新包之后如果改过 Wine 内容，第一次启动会弹「引擎有更新」，要点「立即应用」→「确定」才会重新解压生效。**不点的话引擎根本不会启动**。

## 设备能力只能实测

判断某台设备有没有某个能力（比如 Vulkan），**只能用应用进程里实测的结果**：

- `hdc shell ls /system/lib64` 返回权限不足，不代表文件不存在。
- 宿主侧的图形能力走的是 vtest/virgl 通道，本来就不依赖系统里那个库。

写自动化测试时，图形档位要在套件里显式声明，不要依赖"设备当前设置"——那是由机型和系统版本决定的，同一个用例在不同设备上测的不是同一个东西。
