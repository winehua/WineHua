# 图形栈：档位与通道

> 适用场景：加新的渲染后端、改档位默认值、排查"某个程序不出图/画面不对"时；也是理解图形链路的入口。
> 最后核实：2026-09-24
> 相关代码：`entry/src/main/cpp/wine/wine_env.cpp`（档位到环境变量的映射）、`entry/src/main/cpp/wine/env_profiles.cpp`（档位组装管线）、`entry/src/main/ets/entryability/EntryAbility.ets`（选档决策）、`entry/src/main/cpp/graphics/graphics_broker.cpp`（通道决策）
> 相关文档：[contracts.md](contracts.md)（跨仓库协议 C7-C10）、[opengl-virgl.md](opengl-virgl.md)（GL 通道契约）、[../decisions/0002-d3d-backend-profiles.md](../decisions/0002-d3d-backend-profiles.md)（选档决策记录）

## 两个独立的选择

图形栈上有**两套互不相干的开关**，很容易混淆：

1. **D3D 后端档位**——决定 Direct3D 的调用由谁处理（是 Wine 自带的实现，还是第三方翻译层）。这是本文的主要内容。
2. **帧呈现通道**——决定画面怎么从 guest 传到宿主（共享内存拷贝，还是零拷贝直传）。由图形代理（GraphicsBroker）管理，见 [opengl-virgl.md](opengl-virgl.md)。

本文讲第一套。第二套在渲染链路文档里。

## 四个档位

| 档位 | 实际是什么 | 处理哪些 D3D 版本 |
|---|---|---|
| `wined3d` | Wine 自带的 wined3d（D3D 调用转成 OpenGL） | 全部，但性能最差 |
| `dxvk_legacy` | 项目定制的 DXVK 1.10.3 | D3D9/10/11 |
| `dxvk_modern_2_6` | 项目定制的 DXVK 2.6.2 | D3D9/11（2.x 移除了 D3D10 链） |
| `vkd3d_limited_500k` | VKD3D-Proton 2.6（D3D12），配一个独立的 DXVK 轴处理 D3D11 | D3D12 + D3D11 |

**D3D12 只有一个档位能用**（`vkd3d_limited_500k`）。"limited 500k" 指的是 descriptor 堆上限设为 50 万条（上游要求 100 万），这是为设备能力做的妥协。

**D3D11 有独立的轴**：`dxvk_legacy` / `dxvk_modern_2_6` / `auto`。这个轴**只在选了 `vkd3d_limited_500k` 时才被消费**——因为 vkd3d 只管 D3D12，D3D11 还是要 DXVK 来管。

## 档位怎么决定

优先级从高到低：

1. **启动时显式指定**（`aa start` 的启动参数里带档位）——自动化测试用这条。
2. **用户上次的选择**（设置页切换后存在 preferences 里）。
3. **默认值**。

**默认值在两个分支上不同**：

| 分支 | 默认档位 |
|---|---|
| master | `vkd3d_limited_500k` |
| main-ui（rc 发布线） | `dxvk_modern_2_6` |

读代码时注意确认自己在哪个分支。

### DXVK 轴的自动选择：白名单，不做能力推断

`auto` 的解析方式是**证据白名单**，不是推断设备能力：

```
机型 == <已验证可用的机型> 且 系统增量版本 == <已验证可用的版本>  →  dxvk_modern_2_6
其他                                                            →  dxvk_legacy
```

（具体写的是哪个机型、哪个系统版本，见 `entry/src/main/ets/entryability/EntryAbility.ets`。）

这个设计是刻意的：DXVK 2.x 需要 Vulkan 1.3 和若干扩展，而设备系统更新后驱动行为可能变化。所以绑定的是**已经验证过的一整个设备身份**（机型 + 系统版本），系统一升级就自动回退到 1.10.3，直到新身份被验证通过。

**改默认档位时不要按机型推断**——这是明确的约定。

## D3D 版本怎么走到后端

| D3D 版本 | 谁处理 | 说明 |
|---|---|---|
| **D3D12** | vkd3d-proton | 仅 `vkd3d_limited_500k` 档。其他档会走 Wine 自带实现（`d3d12=n` 只在 vkd3d 分支注入） |
| **D3D11** | DXVK | `dxvk_legacy` / `dxvk_modern_2_6` 两档；或 vkd3d 档下的 DXVK 轴。`wined3d` 档走 Wine 自带实现 |
| **D3D10** | DXVK 1.10.3 的完整 d3d10 链 | **只有 legacy 档有**。覆盖必须设成纯 `n`，不能带 `b` 兜底——兜底路径会调用一个 DXVK 没有导出、而 Wine 自带实现才有的函数，必然崩溃 |
| **D3D9** | Wine 自带 wined3d | 打包时只带 DXVK 的 d3d11 / dxgi / d3d10 链，没有 d3d9，所以 D3D9 始终走 OpenGL 通道 |
| **D3D8** | Wine 自带 d3d8 → wined3d | 不受档位影响 |

档位选择上有明确的回退顺序（按设备能力）：`DXVK_MODERN → DXVK_LEGACY → WineD3D`。

## 两条 GPU 通道

D3D 之后，命令还要传到宿主 GPU。有两条通道：

### Vulkan 通道（venus）

guest 侧的 Mesa venus 驱动把 Vulkan 命令编码后，经过 vtest socket 传给宿主的 virglrenderer，再调宿主 Vulkan。

**只在这两个档位可用**：`vkd3d_limited_500k` 和 `dxvk_*`。`wined3d` 档**不注入** Vulkan 运行时的环境变量——所以那个档位下任何 Vulkan 程序都会在创建实例时失败（加载器找不到驱动）。

这条通道的跨仓库协议（私有 swapchain、present 命令、回调）见 [contracts.md](contracts.md) 的 C7-C10。

### OpenGL 通道（virgl）

guest 侧的 Mesa virpipe 把 OpenGL 命令经 vtest socket 传给宿主 virglrenderer。

**与档位无关，始终可用**。`wined3d` 档的 D3D8/9 走这条路，其他档位下的 OpenGL 程序也走这条路。

就绪判断：图形代理会检查 guest 侧的接收包是否齐全（读一个标记文件 + 校验几个库），齐全才启用零拷贝通道，否则退回共享内存拷贝。

## 各设备的适用性

| 设备 | Vulkan 能力 | 能用的档位 |
|---|---|---|
| Maleoon 920（9020） | 1.3.309，有 robustness2、dynamicRendering | `dxvk_modern_2_6` 已验证；全部档位可用 |
| Maleoon 910（9010） | 1.2.275，无 robustness2 | **只能用 `dxvk_legacy`**，Modern 明确不支持 |
| 手机 | 视机型 | 档位可用；可用 `BUILD_WINE_MONO=0` 关掉 wine-mono 来缩小包体积 |
| PC（x86_64） | 宿主原生 Vulkan | 无 box64 转译，档位照常选 |

**判断设备能力只能用应用进程内实测的结果**——`hdc shell ls /system/lib64` 返回权限不足不等于文件不存在，而且宿主 Vulkan 走的是 vtest 通道，本来就不依赖系统那个库。

写自动化测试用例时，**档位必须显式声明**（不写就退回"设备当前设置"，而它由机型和系统版本决定，同一个用例在不同设备上测的不是同一个东西）。

## 已知限制

改档位相关代码前务必看：

1. **vkd3d 只能用 dirty-ring 系的内存同步档**。用 explicit 档必然黑屏——vkd3d 的上传堆会长期保持映射、没有解映射和刷新的时机，内存脏标记产生不出来。

2. **`PERSISTENT_MAP_SYNC` 开关是个两难**（未解决）：打开则 D3D12 出图、但 DXVK 的读回全坏；关闭则反过来。根因是环境变量层面无法区分程序加载的是 D3D11 还是 D3D12，而两者需要的语义相反。治本方案已定（让 vkd3d 在自己提交前刷新），尚未实施。

3. **`no_semaphore_feedback` 必须恒定携带**，不能跟着 DXVK 档位派生。漏掉它会让 vtest 卡在信号量等待、D3D12 白屏；"先切 2.6 再切回"留下的残留档位会掩盖这个问题。

4. **`no_multi_ring` 必须恒定携带**：guest 的 per-thread ring 会破坏项目的远程共享 ring 传输（宿主解码到非法命令长度）。

5. **D3D10 的程序在 modern 档下的行为没有验证过**（modern 没有 d3d10 链，可能撞上和上面第 3 条类似的导入问题）。

6. **DXVK 2.x 不能直接用上游版本**。920 的 Venus 缺几个特性（dualSrcBlend、multiViewport、BC 纹理压缩等），上游 2.6.2 会拒绝启动——项目 fork 里带了整套兼容实现。

7. **DXVK 2.7 及以上暂时不考虑**：需要 descriptor indexing 和 buffer device address，当前 Venus 都没暴露。

8. **ARM64X 的结论**（依据在 `feature/arm64` 分支的提交 `3cecad3`，不在 `master`）：DXVK 系列可以做真双架构，vkd3d **已放弃**（手工脚本产出的"双架构"其实是假的，真 ARM64EC 在转译层会崩），所以 d3d12 只有 x86_64 单架构版本。

9. **`WINEDLLDIR` 的索引必须连续**：Wine 扫描动态库目录时遇到第一个缺失的索引就停止，所以环境变量里的目录序号不能跳号。

10. **`WINEDEBUG` 不能走环境变量注入通道**（会被覆盖），Wine 侧的唯一决策点在 `wine_child.cpp`。

## 未解决的问题

1. **`PERSISTENT_MAP_SYNC` 两难**——治本方案已定（让 vkd3d 在提交前自己刷新映射范围）+ 五层验证计划，待实施。
2. **Venus 首次计算派发的读回竞态**——测试中首次派发读回恒为哨兵值，失败点总是在"该用例第一个执行的派发"。已排除多个方向，根因未定位。
3. **main-ui 分支的 D3D12 白屏回归**——master 正常、main-ui 全白屏，画面已到达宿主，差异怀疑在界面层的画面承载拓扑。相关实验已全部撤回，未根因。
