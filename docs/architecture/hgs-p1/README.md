# HGS P1 公共服务边界实现与验收

日期：2026-11-06—2026-11-07。状态：实现、构建和本轮设备验证已完成；Vulkan 稳定性验收仍开放，不能宣称完整无回退。本阶段不迁入真实 X11 后端。

## 实现位置与契约

实施树：`/workspace/WineHua-root/hgs-integration`，HGS 位于其 `hgs/`，基于 WineHua `3355dab96b968cd7e6d3da1e01e1a9c7c0b831fc` / HGS `bad402da1a8bdb56c87c2010a2727c09fc58251b`。原 WineHua/HGS 工作树的已有修改保留。HGS 已发布到 main，WineHua 的集成提交更新了对该提交的 gitlink。

公共 API 版本升到 1.1（`0x10001`）。完整用法与限制见 `/workspace/WineHua-root/hgs-integration/hgs/docs/api/service-api.md`。

| 层 | 目录 | 本阶段职责 |
|---|---|---|
| 公共 C++ 契约 | `src/main/cpp/include/hgs/` | 配置、状态、端点、错误、环境、拥有数据的窗口与事件 |
| 服务运行时 | `runtime/` | 唯一实例、幂等启动、配置冲突、epoch、失败逆序回滚、停止清理 |
| 通用核心 | `core/` | WindowRegistry、EventBus、InputRouter |
| Wayland 适配 | `backends/wayland/` | 复用原 compositor，并将旧事件转换为公共快照 |
| 平台端点 | `platform/unix/` | 明确路径的 Unix 监听 fd、锁、冲突检查与按 inode 清理 |
| NAPI | `bindings/napi/` | 七个新导出、参数验证、拥有数据的异步事件 |
| ArkTS | `src/main/ets/api/` | HgsService、HgsSession、HgsStartError |

新旧入口共享 Runtime；旧 startServer/stopServer 和 SessionControl.Stop 均转发到它。新 start 默认不改进程环境；Wayland 通过实际监听 fd 注册到 wl_display，不依赖现有 XDG_RUNTIME_DIR。clientEnvironment 返回实际端点的 set/unset/inheritedFds，客户端仍由宿主启动。

窗口 ID 为不透明字符串，包含 epoch 范围。底层身份由 backend/protocol/连接 ID/协议对象/对象代次组合；连接 ID 不使用 PID。事件拥有字符串与快照，不保存跨回调裸指针。输入释放跟随按下归属，旧窗口不回退到其他窗口或后端。

WineHua 的 WineEnvService 已用 HgsService.start 与真实 clientEnvironment 接入。BetterSH 使用冻结源码副本 `/workspace/WineHua-root/hgs-bettersh-p1`，保留原默认窗口 host，并在副本中增加独立 Wayland 客户端与契约探针；没有修改 `/workspace/BetterSH`。BetterSH 输入基线为 `160990eda43ee49b762186f95b15875e48426d64`，IEC 为 `d46238fdbf464ab6eb9a9b72a7e660933a9f9907`。

## 已完成验证

原始证据统一保存在 `/workspace/WineHua-root/WineHua/build/hgs-p1/2026-11-06/`。目录沿用阶段开始日期，设备续测 run ID 使用 20261107。

| 项目 | 结果与证据 |
|---|---|
| 公共 Runtime 主机测试 | PASS；身份碰撞/对象复用、epoch、幂等与冲突、结构化失败、逆序回滚、输入配对、事件所有权和异常隔离；`service-host-tests-final.log` |
| 真实 wl_display 端点测试 | PASS；进程环境缺失/错误、端点冲突、普通文件保护、陈旧 socket、替换 inode 保护、重启；同上 |
| WineHua 原主机测试 | 16 个程序 PASS；`winehua-host-tests.log` |
| ARM64 native / NAPI | 编译通过；`native-build*.log` |
| WineHua 最终 HAP | 完整构建、既有本地签名、验签、覆盖安装通过；`winehua-final-hap-build.log` 等 |
| BetterSH 最终 HAP | release 构建，通过 build_hap.sh 自查，既有 debug/JIT Profile 本地签名与验签通过；`bettersh-client-fixed-build.log` 等 |
| WineHua core | 最终包 4/4，`core-p1-final-wayland-core-20261107` |
| WineHua DXVK | 最终包 3/3，`dxvk-p1-final-wayland-dxvk-20261107` |
| WineHua 窗口/输入 | 最终包 7/8；仍为 P0 已记录的 win-zorder-x64；`win32-p1-final-wayland-win32-20261107` |
| WineHua Vulkan | 早期 P1 包 4/4，最终包一次 2/4；需要结合下述冻结 P0 交替对照解释，不能取最好一轮作为结论 |
| BetterSH 独立客户端 | 已观察真实首帧、鼠标按下/释放、A 键按下/释放、480×320→1280×720→480×320、关闭、销毁后重连；`bettersh-client-1.log`、`bettersh-client-2.log`、`bettersh-device-checks.json` 及截图 |
| BetterSH 公共契约 | unsupported-protocol、config-conflict、冲突后 Ready、实际环境、e1-w1 窗口快照、销毁后 Ready；重连在 epoch=1 下由 e1-w1 变为 e1-w2，nativeId=3 复用而 clientId/objectGeneration 从 1 变为 2；`bettersh-contract-*.json` |

所有安装使用 `hdc install -r` 保留应用数据；未调用会卸载应用的 deploy_hap.sh；未继续华为在线签名申请。

独立客户端首次运行因测试程序漏掉协商版本的 wl_pointer.frame 回调退出。已补齐对应回调并重新构建、签名、安装；上述首帧和输入记录来自修正后的测试包，HGS 产品运行时代码没有为此变更。

## Vulkan 对照

控制包从冻结的 P0 WineHua/HGS 源码重建，仅含 P0 测试收集修正，使用与 P1 相同的 guest 引擎归档、测试 payload 与复用前缀。两包交替覆盖安装于同一设备；完整输入见 `vulkan-ab-inputs.json`，逐轮结果见 `vulkan-ab-results.json`。该控制包不是原 P0 HAP 的逐字节重现。

六轮按 P0/P1/P0/P1/P0/P1 顺序完成：

| 轮次 | 包 | 通过数 |
|---|---|---|
| 1 | 冻结 P0 控制包 | 2/4 |
| 2 | 最终 P1 | 3/4 |
| 3 | 冻结 P0 控制包 | 3/4 |
| 4 | 最终 P1 | 1/4 |
| 5 | 冻结 P0 控制包 | 2/4 |
| 6 | 最终 P1 | 3/4 |

两组均为 7/12；offscreen-x64 六轮通过，offscreen-x86、sampled-only-x64、sampled-only-x86 在两组均有失败。失败均为 storage_write/image_fetch 采样结果仍为 0xdeadbeef。**本次没有观察到仅 P1 新增的失败类别；控制组自身不稳定，六轮结果不足以证明 P1 Vulkan 无回退，也不能据此确定故障根因。** 保留为开放问题，不以最好一轮或相同总通过数替代稳定性验收。最终设备安装保留 P1 WineHua 包。

## 能力边界与后续

- 当前仅有真实 Wayland backend。x11/both 请求返回 unsupported-protocol；假后端的 both/回滚测试不能当成双协议产品能力。
- start/stop 的 ArkTS 门面返回 Promise，但 native Wayland 注册仍同步执行。异步 NCP 就绪、启动超时、X11 display/认证/XIM 资源属于 P2。
- 默认 host 与 surface 绑定仍走旧兼容接口；新窗口快照 surfaceGeneration=0 表示未知。popupIdentity 与 crossBackendFocus 均为 false，完整 popup/parent/transient 与混合焦点留给 P4。
- C++ 事件回调不得同步重入 start/stop；应投递给控制线程。JS 队列中的迟到事件需要宿主按 epoch/id 丢弃。
- P0 的完整 Mono 首启失败、X11 帧归还同步失败不在 P1 修复范围内，继续保留在 P0 账本。

## 归档与可恢复性

`final-artifacts.json` 记录最终 WineHua/BetterSH HAP 及其中 libhgs 的大小与 SHA256。`hgs-p1-changed-source.tar.gz` 保存含新增文件的变更源码，`hgs-tracked.patch` 保存已跟踪文件差异，`hgs-source-inventory.json` 记录逐文件哈希，`winehua-service-integration.patch` 保存宿主接入差异。

原 P0 Wayland HAP 当时只在可变构建输出中保留，后来被 P1 构建清理；历史日志与哈希尚在，不能再声称该精确签名产物仍保留。本阶段另存固定命名的最终包，并保留冻结 P0 重建控制包 `wayland-p0-control-signed.hap`。P0 X11 两个签名变体仍留在 P0 证据目录。

关键验收摘要随文档入库：[最终产物哈希](final-artifacts.json)、[Vulkan 对照输入](vulkan-ab-inputs.json)、[逐轮结果](vulkan-ab-results.json)、[BetterSH 真机检查](bettersh-device-checks.json)、[BetterSH 隔离测试探针补丁](bettersh-test-harness.patch)。签名材料、HAP 与完整原始日志仍只保存在本地证据目录。

BetterSH 探针补丁使用零上下文格式以避免补丁自身的空白上下文触发 whitespace 检查；仅在冻结的 BetterSH 基线上使用 `git apply --unidiff-zero bettersh-test-harness.patch`。
