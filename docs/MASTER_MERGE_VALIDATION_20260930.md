# master 必要修改合入 feature/proton-wine-ohos 与验证记录（2026-09-30）

分支：`feature/proton-wine-ohos`。合并前 HEAD `fd428d6`（备份分支
`backup/pre-master-merge-fd428d6`），合入 `origin/master`（`60e26c7`，领先 34 提交）。

## 合并内容

master 侧 34 个提交的构成：55 例 win32 回归测试体系（programs/ 用例 + 编排扩展）、
wine 子模块指针更新（`thirdparty/wine` → `f6492f8`，企业微信装饰浮层黑窗修复）、
wayland-scanner 落位项目内 host-tools、hdc 多目标定向修复、显示路线重构 spec/plan 文档。

合并冲突 6 处及取舍：

| 文件 | 取舍 |
| --- | --- |
| `scripts/env.sh` | 取 master：wayland-scanner 统一走 host-tools，删 feature 旧内联回退 |
| `scripts/build_wayland.sh` | 取 master：同上（SCANNER 变量重构） |
| `scripts/package.sh` | 取 feature：两边同一修复，feature 版多 [Fail] 输出扫描（hdc 假成功防护） |
| `automation/smoke.py` | 双方独立功能全保留（SUITE_VARS/hdc_sandbox_shell + 档位契约拦截/ensure_app_running） |
| `.claude/rules/smoke-regression.md` | 取 feature 语义：本树 wine_child.cpp 在 entryParams 覆盖后会重选 WINEDEBUG 档位，`WINEHUA_WINEDEBUG` 可用；恢复被自动合并丢掉的“产品不为测试让步”条目 |
| `entry/src/main/ets/smoke/SmokeRunner.ets` | 双方全保留（注入编排 + 性能基准窗口） |

## 合并后修复（4 个提交）

1. `f42eee4` merge 本体。
2. `cb0f427` **wine-valve 移植 0010 补丁**：master 的 f6492f8 修 `is_window_managed`
   把带 owner 的纯装饰浮层（LAYERED+NOACTIVATE+TOOLWINDOW 三件套）判受管导致企业微信
   黑窗；本树构建用 wine-valve，故以 `patches/wine/0010-winewayland-decoration-owner-floating.patch`
   幂等应用，子模块仍钉在 4d3ec031。`thirdparty/wine` 子模块同步检出 f6492f8。
3. `eacec89` SmokeRunner.ets 补回注入 if 的闭括号（合并拼接丢失，ArkTS 编译失败暴露）。
4. smoke.py 去重 `--desktop-mode` 注册（两侧各自加过一个，argparse 冲突）+
   `smoke/programs/proc/proc_pipe.c` 线程签名对齐 `_beginthreadex_proc_type`
   （本项目 llvm-mingw 20260826 下为硬错误）。

## 构建与验证结果

- 增量编译：winewayland.drv.so（含 0010）、`w1-m2-assemble.sh` rc=0、
  `w1-m3-build-hap.sh` rc=0；runtime closure PASS、candidate verification PASS；
  payload 哈希 `fc6f66df479c1b31a9adfb487cda970c48698d14ee148d6e9af77922de4c6862`。
- smoke payload：全量 170 exe 构建通过（含 master 新用例）。
- 主机测试：direct_viewport 87 项、锁序 18/18（entry-contract 负对照按预期 SIGABRT）、
  benchmark_statistics、env_baseline 88 项、input_state 84 项、presenter/controller/
  display_policy 等全部通过。既有 `toplevel_event_test` 编译失败为合并前已单列问题
  （backup..HEAD 对 host_tests/ 与 compositor/ 的 diff 为空），未计入本轮回归。
- 设备 `5KPBB25818203996`：覆盖安装新 HAP，运行时 manifest 与 payload 哈希一致；
  `core` 套件 4/4 PASS（opengl/dns 双架构）；`win32` 套件 103/134 PASS。

## win32 套件 31 例失败归类（feature 支线对新用例的既有缺口，非本次合并引入）

合并未触碰下列路径的运行时代码（除 winewayland 的 is_window_managed 判定外）；
这些用例在本支线（wine-valve + FEX/ARM64EC）属首次运行：

- 异常/SEH：`seh` 除零异常码不符（exit=0x17 ≠ 0xC0000094）
- 图形：`wgl-basic` 清屏色错、`d3d9-offscreen` 离屏像素未画、`d3d-smoke` d3d10.dll
  加载失败、`screen-bitblt`/`win-layered` 屏幕回读/分层窗口像素（依赖 capture backfeed）
- 音频：`audio-waveout` 播放位置不推进
- 注册表/进程：`reg-wow64` 64 位视图读回 32 位值、`toolhelp-snapshot` x64 guest 父 pid 恒 0
  （用例自身注释已标注该缺口）、`mem-virtual-x64` NCP 退出码 29（x86 通过）
- 输入/注入：`input-capture`、`input-relative`、`dinput-mouse`、`e2e-drag`、
  `shell-dialogs` —— 带 `--desktop-mode virtual` 冷启动定向复测（runId
  `r20260930-211942`，10/10）仍全部失败，注入链在该支线未打通，与桌面模式无关

## 环境备注

- smoke.py 需在 Windows python 下跑（WSL python + Windows hdc.exe 的本地路径拼接损坏）；
  `build/automation-logs` 属主需为 ubuntu（容器 root 属主会让 UNC 写入被拒）。
- 设备端 `-b` 沙箱通道要求应用在运行。
