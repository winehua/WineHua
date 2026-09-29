# Direct 架构分支保存与远端同步（2026-09-29）

分支：`feature/proton-wine-ohos`。本次用户明确要求同步并合并同名远端分支；不向其他架构分支合并。

## 保存范围与现状

- 平板：保存 Direct Wine surface IPC、共享 NativeBuffer/fence、独立 Vulkan context、fusion presenter、Vulkan 桌面合成、resize/窗口状态和 USER/surface 锁顺序修复，以及对应宿主测试、性能工具与实机证据。
- 手机：保存早期 fork server、系统 Vulkan 离屏探针、共享图像双向 P0 和明确的失败阶段。Direct 上屏仍未打通，**按用户决定继续原 Venus 路线**。反向 Vulkan allocation 进入 HDI/SAMGR 等待，0 个共享帧；失效继承 IPC 状态是待验证假设，不能写成已证明的唯一根因。
- 实测手机 Venus + DXVK 2.6.2 冷启动立方体 871 帧通过；平板关闭采样器的 AMD64/FEX A/B 共 12 次冷启动通过，Direct resize 878 帧通过。Direct 仍为 opt-in，整体性能、真实游戏和完整异常矩阵尚未完成。
- 详细手机问题与保留策略：[Mate 80 反向导出报告](mate80-direct-reverse-export-20260929.md)。整体实施状态：[ARM64 Direct 实施记录](arm64-direct-implementation.md)。

## 重建完整性修正

固定 Wine 子模块仍为 `4d3ec031c42a232e15a860274f743f756e047e8d`。核对发现本地 Direct WSI 和 Vulkan resize smoke 的六个文件尚无重建补丁，现补入 `patches/wine/0009-direct-ohos-wsi-and-resize-smoke.patch`，由 `scripts/build_wine.sh` 幂等应用。

同时修正 `ensure_wine_patch()` 的反向检查：GNU patch 在未修改源码上可能自动忽略 `-R` 并返回成功，导致误报“已应用”。反向探测使用 `--force` 保持指定方向；实际 helper 的三项宿主检查验证干净源码、已应用源码和冲突源码。

从固定 Wine HEAD 提取源码并应用构建入口声明的补丁后，13 个涉及文件与当前源码逐字节一致，覆盖全部 11 个 Wine dirty 文件。验证不改变工作中的子模块，也不更新 gitlink。结果见 [补丁复现记录](evidence/branch-sync-20260929/wine-patch-reproduction.json)。Wine 子模块的已应用补丁状态保留；vkd3d/dxvk-modern 的既有嵌套构建残留不纳入此次提交。

## 远端与验证边界

同步前本地 HEAD=`b53ca22fd1a7f8a96c2392f5931f51cd4436d70d`，远端同名分支更新到 `fcc7db10dc1c385fbe7cb08daacd224768eb5192`。双方分别有 18 和 1 个提交；远端新增项只修改部署脚本的显式设备选择与 HDC 失败标记检查。

本轮检查：Direct viewport 87 项、USER/surface 锁 18 项、benchmark statistics、benchmark artifact 5 项、补丁方向 3 项、真实 socket fd 校验全部通过。签名构建与 runtime closure 先前已通过，最新整理包 `1e85b099…ab551` 未安装；手机实测包为 `c56eb3a8…da04`。既有 `toplevel_event_test` 的全量 make test 编译失败仍单列，不把局部检查写成全量通过。[检查摘要](evidence/branch-sync-20260929/checks.json)

实现保存提交：`83014a3`（`feat(direct): save GPU compositor and phone diagnostics`）。文档和证据另作提交，再合并远端同名分支。最终合并提交号与远端同步结果由 Git 历史和本次回复记录。
