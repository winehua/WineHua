# PR #78 集成状态

## 最新状态：2026-11-02 显式读回复验

PR #78 已更新到技术修复 `001eff4`，为 Ready for review，尚未合并。
PR #79 通过 merge 引入该公共修复，仍为 Draft。下文较早的 Draft、待重建和
待迁移表述属于历史阶段，不代表当前状态。

完整重建运行时 `48c0ec3b476948359adedf51359a76216426d1ac48424253c0b74ed55935c497`
已导入并在保留 prefix 的情况下部署。当前技术主机回归 36/36，通过产品分支
补测的主机回归为 38/38。默认 Vulkan invalidate 与 remote 映射读回修复后，
MewGenics、Darkest Dungeon 在产品候选中均进入主菜单，用户确认动画恢复。
参见[根因与负对照回归](../debugging/venus-explicit-shadow-readback.md)。

同一 v15 产品包单轮设备回归：core 4/4、DXVK 3/3、D3D12 2/3、
DXVK D3D9 3/4、Wine Vulkan 3/4。D3D12 第 242 帧等待 fence 失败；
x64 D3D9 虽呈现 50 帧但没有固定帧截图；x86 sampled-only 仍保留
`0xdeadbeef`。这不是全绿发布结论，也尚未证明 D3D12/D3D9 失败与本次
读回修复有因果关系。历史失败记录保留，不自动重跑掩盖。

设备包与界面证据属于产品候选，不能写成已经重建/重测技术 HAP。
CI 的 fork workflow 仍为 `action_required`，只能由上游维护者批准；
当前提交者没有批准权限。完整输入/IME、长跑和其他设备仍有未验证范围。

## 早期集成记录

核实日期：2026-11-01。技术分支 `integration/pr78-upstream` 已合入官方 `master` 的 `a001e6d`，处理 21 个冲突，保留 X11 路线、CreatedAt/Restored/Modal 契约和 DesktopInputSink。PR #78 仍为旧基线 Draft，候选尚未替换 PR head。

独立游戏库页面、Ability 和配置模型已迁往 `integration/pr78-main-ui`，产品配套 [PR #79](https://github.com/winehua/WineHua/pull/79) 的目标为 `main-ui`。产品分支通过 merge 继承公共修复，保留应用库、主题、悬浮桌面、PC 缩放和完整键盘；其完整主机回归及 Native/ArkTS HAP 构建通过，产品实机交互待验。

## 已有集成技术版证据

技术 HAP SHA-256 为 `dfeef6ad69321010abeed8b77edd1bb73c0f217b83a3d8a1bfe478e5bff4e223`，运行时载荷为 `0aa1eac9b72376ea5094441b5d5b27067b1266dcd275c442d5148e0960404e0c`。同一台手机、同一 HAP、显式 Wayland 路线和 `prefix=reuse` 下，core 4/4、Wine Vulkan 4/4、普通 DXVK 3/3、D3D12 3/3 均通过设备结果与主机判定。归档及各包身份见[完工清单](completion-checklist.md)。这些记录使用旧 Wine/guest Mesa，不能代替新运行时或默认 X11 路线验收。

专用 `dxvk_legacy_d3d9` 套件为 2/4：x64 D3D9 和同档位 D3D11 完成呈现与截图，x86 两项仍在加载 DLL 时退出。显式兼容模式已避免请求设备缺失的 BC/multiViewport，默认严格行为和格式能力检查保留。Wine overlay 搜索补丁已通过真实 loader 函数的 WoW64/档位隔离回归，须在新 Wine 下实机确认。

手机首启 SIGSEGV 已由系统栈定位并修复：EGL 探针仅传入 `OHNativeWindowBuffer*` 包装，设备标记为 `pass:payload=OHNativeWindowBuffer`。回归执行实际导入代码，旧源码负对照会触发错误载荷断言。HAP 检查覆盖 staged 库、CMake 生成的 NAPI/子进程模块、16 个 DXVK DLL、清单哈希及 CRC。

## 完整重建进度

隔离 Linux 工作目录的 `make NATIVE_ARCH=arm64-v8a deps wine` 已成功，日志以 `GUEST_RUNTIME_BUILD_READY`、`runtime_build_exit=0` 结束。包括新编的 Wine、wineserver、Guest Mesa/VirGL/Venus、Vulkan Loader、GnuTLS 和 GStreamer/FFmpeg。源码 gitlink、补丁/脚本哈希及工具版本均有 provenance，SDK 使用 Linux LLVM 18 配合独立的 OHOS SDK overlay，未修改原 VM 开发仓库。

`make NATIVE_ARCH=arm64-v8a assemble` 正在同一目录完成其余图形/原生依赖和布局打包。已完成两架构 Legacy/Modern DXVK、VKD3D 及新 smoke EXE 编译；完整组装 ZIP、重打 HAP 和新运行时设备验收尚未完成。此前独立 ARM64 X11/wlroots/Xwayland/Wayland/VirGL 干净构建及试链接已通过。

当前技术分支的 Windows 完整主机验证为 34 阶段通过，产品分支为 36 阶段通过。Linux 上当前 Wine/DXVK 补丁后的函数回归、读回 ASan/UBSan 和 `make test` 通过，包括窗口事件、输入状态、合成像素、控制器和 WHGP 协议检查。旧 Linux 27 阶段归档另有历史范围，不能混记为本次全部证据。

## 数据保护与待验收项

手机只有当前一台可用。保持常亮；未卸载应用、重置 prefix、删除游戏或存档。已备份并哈希核验三份现有 prefix 注册表。HDC shell 无权重命名应用目录，临时维护 HAP 在应用权限内完成了独立测试目录的移出/移回/回读验证；新运行时尚未切换。正式切换须逐文件哈希验证 staging，保留旧管理运行库，并具备失败恢复路径。

仍需新运行时的 core、Wine Vulkan、DXVK、D3D12 和专用 D3D9 验收、默认 X11 路线、输入/IME、现有真实游戏持续运行及产品页面交互。其他设备没有实机证据。#79 CI 当前为 `action_required`，需要上游维护者批准 fork 工作流；当前凭据只有上游读取权限。两 PR 继续保持 Draft。
