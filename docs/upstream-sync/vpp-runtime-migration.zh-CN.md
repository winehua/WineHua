# VPP 非 UI 运行时迁入 WineHua

后续范围更新：用户已授权为新增后端补充界面。DXVK9/Zink 的选择、保存及启动链路见 [graphics-backend-ui.zh-CN.md](graphics-backend-ui.zh-CN.md)；产品身份、资源及其他页面保持原样。下文“UI 不变”描述初次运行时迁移范围。

## 范围与来源

本分支 `feature/vpp-runtime` 从 WineHua `master@40b747714c109a7cb13e38b3af93ec77e1d38594` 建立，按功能迁入 VPP `release/2.0.4@043a1cc0cab8e52b011faf2c611d3c16562ecb0a` 的运行时源码、补丁和构建配方。2026-11-17 再次查询 GitHub，WineHua master 仍为该基线。

这是运行时迁移候选：已完成源码重放、Wine 完整源码编译、8 个 ARM64 应用 native 模块、ArkTS/Hvigor 整包及有界 Mini 功能验证。FEX 与部分依赖按最终产物校验后复用，未宣称全依赖重新源码编译；完整设备门禁仍未通过。VPP 的历史设备结果只说明来源，不能作为本分支的性能或兼容性结论。本次不推送、不合入 master、不上架。

保持 WineHua 的 ArkTS UI、AppScope、资源、模块权限、产品身份及签名示例配置不变；不迁入会员、云函数、Steam 分发/解压模块或 VPP 产品专属装包门槛。Wine 内的通用 Steam/CEF 进程兼容逻辑保留，和 Steam 产品分发无关。诊断子库默认关闭，不启用全量追踪或实验图形开关。

## 已迁入的运行时

| 组件 | 迁入内容与边界 |
| --- | --- |
| Proton/Wine | OHOS 平台/ABI、进程与 broker、prefix 初始化、WOW64/FEX 接口、字体/DPI、X11/Wayland、剪贴板、输入、媒体及通用程序兼容补丁；以完整适配配方为准 |
| FEX | 锁定来源与 21 个适配提交；ARM64 原生 Wine 默认使用 FEX；保留精确存储配置，不启用 tracing、strict-mul24 或 ARM64EC 实验路线 |
| 图形 | VirGL、Mesa/Zink、Venus、DXVK Legacy/Modern、VKD3D 的源码锁和实际构建补丁；包括 DXVK9 窗口后备缓冲保留、Zink shadow buffer/image 传输修复、WineD3D 旧式雾生成修复及呈现/队列生命周期修复 |
| 显示与输入 | native compositor、X11/Wayland 坐标和生命周期、IME、输入队列、失焦释放、多手柄桥接及协议；不移入 VPP 页面或设置 UI |
| 音频与进程 | native 音频通道、子进程分发、IPC、退出/重入及日志预算等运行时基础设施 |
| 构建与校验 | Make 源码准备、增量身份、依赖产物与最终包配对、Mono/双架构 Gecko、JSON5 读取和签名身份检查；沿用 WineHua 装包入口 |

保留原 `thirdparty/wine` 作为旧路线参考；新增 `wine-valve` 和 `fex` 的公开来源，不把不可公开取得的本地适配提交写成 gitlink。Box64 保留为原有可选路线，ARM64 默认路线为原生 Wine + FEX。默认 native ABI 仍遵循 WineHua 的 `NATIVE_ARCH ?= x86_64`，ARM64 构建需显式选择。

## 可重建源码身份

| 组件 | 公开上游提交 | 完整适配提交 | 最终树 | 补丁数 |
| --- | --- | --- | --- | --- |
| Wine | `e8ca5806eec488f5916e83d655ffc5349b6b1702` | `527cd6255d4f26c21ff0a7111fc0c37e35eac9ba` | `bd2d6662a1d32f4347829428b6c7c055afde0797` | 86 |
| FEX | `ab135df091d005cc98dfddc56c3dc9c9e4bd501d` | `c3b2b2306347e16da04e512a52432cb9c921d221` | `fe8e476f8828d261dfa7f4f338a85144177b8058` | 21 |

`scripts/prepare_runtime_sources.py` 根据 `docs/upstream-sync/*-candidate.lock.json`、适配提交原始内容及顺序补丁，在 `build/runtime-sources/{wine,fex}` 重建源码，并逐步验证补丁 SHA-256、parent、tree 和 commit。不会在 raw submodule 上重复施加旧 overlay；拒绝 dirty、外来或符号链接目标，完全匹配的外部缓存可只读复用。

其余公开 gitlink：

| 组件 | 提交 |
| --- | --- |
| Box64 | `e970ee6f21799724697fc7c5f275f5442e7d035a` |
| DXVK Legacy | `5dff1d7b3dc3c2cd7f51356b99213bcc427b1af9` |
| DXVK Modern | `01febe2d48fdce76b581369a1d526d2d33b75c7a` |
| Mesa | `557fa3e781ea6e78fcff3ee759c25f60c3055064` |
| VirGLRenderer | `bb33e52aa03199926f662e50ff22bd00d19d7e0a` |

以上七个新增/变更 gitlink 均通过 GitHub 公开 commits API 核对可取得。完整 Wine/FEX 补丁已在任务专属 bare index 重放，最终 commit/tree 与锁完全一致；测试只借用已有 Git objects，没有复制共享源码树。

## WineHua 旧接口兼容

- 保留 master 的 76 个 native 导出及其全部 `libentry/Index.d.ts` 声明；新增 runtime 接口只声明实际实现，不引入已排除模块的接口。
- `runtime_compat.{h,cpp}` 恢复 `setClipboardText`、`setClipboardUpdateCallback`、`imeRearm` 和 `notifyToplevelMove`，调用新的实际 runtime；剪贴板使用 300ms 版本快照轮询，支持注销和环境销毁。
- master 的 `setOpenTargetCallback` 返回 `void`，新 runtime 支持同步 boolean 接受/拒绝。兼容层将已执行且返回 `undefined` 的旧回调认作已接收，避免系统已发起打开后再次 Wine fallback；异常、过期、取消仍拒绝。回调执行期间释放请求锁，允许在回调内注销/取消而不死锁。
- 旧 void 回调的确认只表示调用已交给既有 ArkTS 异步流程，不能证明系统 `startAbility` 最终成功。后续若需最终结果确认，需要另行定义异步回执，并配合 UI 层修改；本次保持 master 行为。
- master 已有但未实现的 `termRun/termSend/termResize/termClose` 声明原样保留，不属于本次新增功能。

## 构建与验收入口

仅在既有 `winehua-dev` Docker 内通过 Makefile 构建，源码和增量产物放在 Linux ext4。API 23、guest graphics/Vulkan 及产品身份检查保留。首次完整构建前需初始化公开 submodules，并准备本项目已有 SDK/依赖；不要把本机只读依赖链接当作已提交的构建产物。

```sh
# 以下在 winehua-dev 容器内、仓库根目录执行。
# 原始公开子模块不承载本地适配提交。
git submodule update --init --recursive

# 必须先提交所有有效源码；prepare 会拒绝未登记输入。
make NATIVE_ARCH=arm64-v8a assemble

# 有效 Wine/FEX 源码就绪后运行主机验证。
make runtime-host-check
make runtime-protocol-check
make runtime-zink-check
make runtime-native-check
```

`runtime-native-check` 编译 8 个默认 OHOS native 模块；`runtime-protocol-check` 另需宿主 Wayland server 库或 `RUNTIME_HOST_WAYLAND`。完整装包继续使用既有 Make/Hvigor 入口，需要 WineHua 本地合法签名配置，本次仅在忽略目录复用既有合法 debug 签名材料，不生成或提交密钥。

`build_wine.sh` 验证完整适配身份后复用增量构建；FEX stamp 包含锁、提交历史、顺序补丁和构建配方。`assemble` 在依赖 producer 完成后、装配前捕获 `local_source_receipt.py`，将有效 overlay 与实际产物绑定；Hvigor 生成的 native 模块在最终包阶段配对核验。`wine_runtime.version` 写入适配后的实际 Wine/FEX 身份。提交的构建配方不依赖 VPP 基础 HAP，也未引入 Steam 产品门槛或全树删除对象文件。此次本地验证按最终 VPP 2.0.4 HAP 的 SHA 和成员字节复用部分依赖；没有导入其 UI 或 Wine 二进制，不能据此宣称全部依赖独立重编。首次 Hvigor 清理了根 build 下的运行时缓存，现已将运行时增量目录隔离至 workspace_temp/runtime-build，复用本轮已验证的 Wine 字节，并重编实际受影响模块。

## 本轮验证结果

机器可读证据见 [vpp-runtime-validation.json](vpp-runtime-validation.json)。原始日志保留在本任务的忽略目录，单文件上限 32 MiB；报告只入库摘要与 SHA-256。

| 验证 | 结果与限制 |
| --- | --- |
| Wine/FEX 完整源码重放 | 86/21 补丁全部通过；最终 commit/tree 精确匹配；不是编译验证 |
| master UI/身份/API 边界 | 通过；受保护路径不变，76 个导出和原类型声明全部保留 |
| 默认 ARM64 OHOS native | 8 模块编译链接并纳入最终 HAP；使用已校验第三方依赖，未重编所有依赖 |
| master 主机回归与 runtime-host-check | 通过；含输入、窗口、环境、控制器、WHGP、recipe 防篡改/复用、构建身份及源码产物校验 |
| DXVK9 后备缓冲策略 | 32 策略用例及负对照通过；非本分支真机画面验收 |
| 多手柄 | 实际桥接多客户端、四路独立输入、拔插、slot 复用及定向震动主机测试通过；未接四个真实手柄 |
| 剪贴板 | 生产 callbacks + 真 Wayland server 的 focus/FD/空文本/EOF/超时/取消/限制/重启测试通过 |
| 系统打开 | OPEN transport 及实际生产 callback 的 void/boolean/异常/过期/回调内取消测试通过；未调用真实系统应用 |
| Zink buffer/image | 384 buffer、648 image 用例通过，包含 ASAN/UBSAN 和旧代码负对照；不等于真实 GPU 图像验证 |
| WineD3D 旧式雾 | 实际 shader generator 96 个组合通过；旧代码同一断言检出 2 个失败 |
| ARM64/x86_64 构建图与脚本 | 两条 assemble 构建图可解析；shell/Python 语法通过；x86_64 native 尚未编译 |

本地构建与 Mini 记录见 [winehua-mini-runtime-validation.zh-CN.md](winehua-mini-runtime-validation.zh-CN.md)。已修复首启 NAPI 注册冲突、最终包 GStreamer 库身份和 X11 显示统计兼容问题。尚未完成：合法 READ_PASTEBOARD profile 下的系统剪贴板、当前严格路线归属门禁、全部第三方依赖重新源码编译、真实游戏与 Heaven/PAL4 性能测量、APP 上架包及其他设备验收。本轮不承诺帧率提升。
