# 决策记录 0003：三套运行方案共存

> 决策日期：2026-08-17
> 决策结论：**同时支持三套方案**，不收敛到某一套；构建目录按架构隔离。
> 最后核实：2026-09-24
> 相关文档：[../architecture/overview.md](../architecture/overview.md)（总架构）、[../architecture/graphics-matrix.md](../architecture/graphics-matrix.md)（图形侧）、[../build/guide.md](../build/guide.md)（构建流程）

## 一、三套方案是什么

| 方案 | 怎么构建 | 跑起来是什么 | 定位 |
|---|---|---|---|
| ① x86_64 原生 | `make NATIVE_ARCH=x86_64` | 宿主本身是 x86_64，Wine 直接以原生 ELF 运行，**没有指令转译层** | 模拟器和 x86_64 设备（含开鸿 PC） |
| ② box64 + Wine | `make NATIVE_ARCH=arm64-v8a`（`master` 上的默认） | 宿主是 arm64；box64 把 x86_64 的 Wine 和 Windows 程序转译到 ARM64 执行 | **当前主力**：绝大多数鸿蒙设备是 arm64 |
| ③ 纯 arm64 | `make NATIVE_ARCH=arm64-v8a`（`feature/arm64` 分支上的默认） | Wine 本身是 arm64 原生（PE 侧 aarch64/arm64ec），x64 应用交给 FEX 转译 | **未来的方向**：省掉 Wine 自身那一层转译 |

选择的实质是「**转译谁**」：

- 方案①不转译——宿主和程序同架构。
- 方案②转译一切——Wine 本体和 Windows 程序都是 x86_64，整条链由 box64 翻译。
- 方案③只转译应用——Wine 是 arm64 原生的，只有 x86_64 的 Windows 程序需要 FEX 转译。

## 二、为什么不做成一套

**x86_64 那套是必需的，它对应的是真实存在的设备。** 模拟器跑在 x86_64 的开发机上，开鸿 PC 本身就是 x86_64 的机器。这些设备上跑 arm64 的 Wine 没有意义（要么转译、要么根本跑不起来），直接用 x86_64 原生最省事、性能也最好。

**box64 这套是当前唯一能在 arm64 设备上跑 x86 Windows 程序的办法。** 手机、平板这些主战场都是 arm64，而用户要跑的程序几乎全是 x86_64 的 PE 文件。没有 box64，这些设备上一款程序也跑不起来——这就是它当主力的原因。

**纯 arm64 是未来方向，因为它省掉了一层。** 方案②里，Wine 自己的代码（PE 侧、Unix 侧）也要被逐条转译，这部分开销是纯粹的浪费——Wine 是我们自己编译的，本来就可以直接编成 arm64。改成 arm64 原生之后，只有应用那部分需要转译，理论性能和启动速度都更好。现在不切过去，是因为这条路还新（FEX、arm64ec 这些还在成熟中），而 box64 这条路已经踩稳了。

## 三、代价：构建目录不能共用

三套方案共用同一个源码树，但**中间产物必须按架构隔离**，否则会互相污染。这不是理论担忧，是做这个决策时实测踩出来的：

- `build/wine-ohos` 和 `build/wine_server/libwineserver.so` 不带架构后缀时，切换方案会出现 configure 的缓存不复用、产物路径却复用，一边的 `.so` 被另一边的覆盖。
- 依赖库的 `pkg-config` 目录如果共用，会出现「x86_64 的构建链接到了 aarch64 的库」，报 `incompatible with elf_x86_64`。

现在的做法：构建目录带架构后缀（`wine-ohos-<架构>`、`wine_server-<架构>`），依赖的 stage 目录也按架构分开。切换方案时各走各的目录，互不干扰。（带后缀是 `feature/arm64` 上的做法；`master` 上 Wine 恒为 x86_64，目录名不带后缀。）

**另有一条**：`make NATIVE_ARCH=all`（出双架构 HAP）和方案共存是冲突的——一次构建只能有一个 Wine 架构，没法同时满足 arm64 原生和 box64 的组装要求。要双架构包就分别构建。

## 四、当前落地状态

**`master` 上是方案①和方案②**：

```bash
make NATIVE_ARCH=x86_64        # 方案①
make NATIVE_ARCH=arm64-v8a     # 方案②（box64 + x86_64 Wine）
```

**方案③在 `feature/arm64` 分支上**，那条分支多了 `WINE_ARCH` 这个开关：

```bash
# 以下命令在 feature/arm64 分支上
make NATIVE_ARCH=x86_64                    # 方案①
make NATIVE_ARCH=arm64-v8a WINE_ARCH=x86_64  # 方案②
make NATIVE_ARCH=arm64-v8a                 # 方案③（默认 aarch64）
```

**注意两个分支上 `make NATIVE_ARCH=arm64-v8a` 的含义不同**：`master` 上是方案②，`feature/arm64` 上是方案③。在哪个分支构建、build 目录里的产物属于哪套方案，动手之前先确认清楚。

三条路线都做过完整构建验证（2026-08-17：方案① 305M、方案② 403M、方案③ 325M，方案③还在真机上跑通了桌面）。
