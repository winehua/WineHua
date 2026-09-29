# Display-Route M2 阶段 A 验收报告（2026-09-30）

设计 spec：`2026-09-27-display-route-x11-wlroots-design.md`（§4.2 = 重锚落地
形态，§6.4 = M2 阶段 A 实测结论，§8.4 观察项 = 双路线对照数据；本文只记判据
裁定、挂起与 M3 入口，不重复数据）。

分支 `feature/display-route-m0`。**scope ruling（用户，2026-09-30）**：本里程碑
先做阶段 A（T1–T6）+ A 收尾（全量回归 + spec 回填）；阶段 B（T7 IME）与阶段 C
（T8 PC 形态）暂缓，A 收尾后再议。

设备：192.168.1.5:44959（arm64 平板，HarmonyOS）；同构建同设备对照。

## 判据裁定

| # | 判据 | 来源 | 裁定 | 证据 |
|---|---|---|---|---|
| 1 | 输出路径清账（munmap 配对 / 丢帧可见 / 失败归还 / 启动收尾） | T1 | **过** | 代码 + 回归 core 4/4；known-issues §1.1/§1.2 按协议删除（逐条核过代码） |
| 2 | R-ZC ②：host EGL `EGL_NATIVE_BUFFER_OHOS` 导入可用性裁决 | T2 | **过（可用）** | 真机探针 `pass:payload=OHNativeWindowBuffer`（r20260930-013431-t2d）⇒ 零拷贝路线成立 |
| 3 | R1 收口：X 路线 GL 呈现链是否成立 | T3 | **过（判否）** | 三处构建层缺件（Xwayland 无 GLX / guest 无 libGL / winex11 GL 段未编入）⇒ X 路线当前无 OpenGL 呈现；补栈超出阶段 A（known-issues §2.4） |
| 4 | 零拷贝落地：gles2 + OHOS 导入 + GPU 直通输出 | T4 | **过** | copy+flush 段 19011us→1572us、scene 19446us→3202us；判据脚本 20/20 ok（r20260930-044542 归档）；帧率按「33ms 时钟 + 工作」模型推导 ~27.7fps ≥ 25（直测待持续内容源） |
| 5 | Venus SURFACE_ID 重锚 X window id（两端成对 + 销毁即失效） | T5 | **过** | wine fork a1e0d586（私有面）+ 主仓 055df1d（接收侧）；X 路线 present 6/6 PASS、每轮 133~137 帧、`presentFrames=150` `failure=0`；截屏出现逐帧清屏色大色块 ⇒ 真上屏；销毁竞态 4 轮计数正常（1 次 `orphan_frames=1` 走丢弃不泄漏） |
| 6 | dxvk 套件 X 路线出图（出口判据） | T6 | **过** | `dxvk-cube-x64` PASS：`frames=221`、`angleRegressions=0`、`presentHresult=0x0`（r20260930-063322） |
| 7 | 双路线回归不回退（core / wine-vulkan / dxvk） | T9(A) | **过（既有红项除外）** | wayland：core 4/4、d3d12 3/3、wine-vulkan offscreen 3/3、present 1/1；X 路线：wine-vulkan offscreen 1/1、present 6/6、dxvk-cube PASS。既有红项三条见下 |

**阶段 A 总体：达成**（判据 1–7 全过；三条既有红项在 T5/T6 期间以对照实验
逐条与本次改动解耦）。

## 既有红项（非本阶段引入，逐条有对照证据）

| 项 | 现象 | 与本次改动解耦的证据 |
|---|---|---|
| `dxvk-legacy`（d3d11-smoke） | `D3D11CreateDeviceAndSwapChain` 返 `E_INVALIDARG`（`presentResult=-2147024809`） | ①两条显示路线同报文；②同参数同调用的 cube 双路线 PASS（非静态能力拒绝）；③wine fork stash 回 T5 之前完整构建 + 卸载重装后 wayland 2/2 复现（r064027/r064134）。详见 known-issues §2.5 |
| X 路线 `displayroute`（dx-notepad） | 超时/结果文件缺失 | 02:19 的归档（r20260930-021916 / r021440）在我方改动之前已是同形失败；根因方向 = X 路线内容率 ~1.8/s（§6.4） |
| wayland `venus_storage_write` 回读抖动 | 偶发 `value=0xdeadbeef` | 出现窗与设备被本会话 hilog 落盘占满（132GB/6 采集进程/load ~16）重合；清理后同构建 5/5 PASS。样本不足以定论，记观察（§6.4） |

## 挂起项（随分支携带，非阻塞）

完整清单在 `docs/engineering/display-route-known-issues.md`（M3 开工逐条过）。
本阶段新增/改形：

- §2.5 `dxvk-legacy` 既有失败（含三条判据与下次入口：DXVK `CreateSwapChain`
  trace 对齐私有 WSI 的能力返回值）
- §2.6 guest 帧归还队列不带 GPU 侧同步（观察项；升级条件 = 出现撕裂/错帧或引入
  对同步敏感的内容源）
- §2.4 保留：X 路线 GL 呈现链不存在（补栈三件套）
- §2.1/§2.2/§2.3 保留：最后一键、GL 回读带宽差、引擎冷启偶发
- **新观察**：core 首轮 r20260930-064926 卡在第一个用例（app 与 wine 子进程全
  idle、无自旋），重跑即 4/4 PASS —— 与 §2.3 同形（引擎冷启偶发），但这次是
  「停滞」而非崩溃，下次复现按 §2.3 的保现场流程处理

## M3 入口建议（优先级序）

1. **旧合成器退役清单**（M3 主题）：wayland 路线现走 `compositor/` + `egl_renderer`
   的旧链，X 路线走 `display/`（wlroots）。退役前必须先把 X 路线的既有红项
   收掉：①内容率回压（§6.4，影响所有「持续出图」验收）；②GL 呈现链补栈
   （§2.4）或明确裁剪「X 路线不承接 OpenGL 程序」。
2. **dxvk-legacy 定位**（§2.5）：DXVK 侧 trace + 私有 WSI 能力输出比对；它是
   DXVK 矩阵里唯一未绿项。
3. **帧同步收口**（§2.6）：给 guest 帧路径加渲染侧同步（fence 或双缓冲 hold）。
4. 阶段 B（IME 三候选裁决）与阶段 C（PC 形态 output↔系统窗绑定）按 spec §7
   顺位推进；两者都不依赖阶段 A 的残留项。
