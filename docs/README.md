# 项目文档

把 Wine 移植到 HarmonyOS，让 Windows 程序能在鸿蒙设备上运行。技术路线是：box64 把 x86_64 代码转译到 ARM64，Wine 跑在转译层之上，自研的 Wayland 合成器负责把画面送上屏幕。

## 这套文档怎么用

- **按任务查**：先看下面的「按任务查找」表，找到对应文档；日常命令直接看 [cheatsheet.md](cheatsheet.md)。
- **按主题查**：不确定看哪篇时，翻「文档地图」按目录找。
- **文档里的状态说明**：每篇开头标了适用场景和最后核实日期。代码变化快，超过半年没核实的内容，用之前先对一下代码。
- **历史材料**：已完成的方案、调研、状态快照都在 [archive/](archive/)，只作记录，不代表当前情况。

## 按任务查找

| 我要做的事 | 看这些 |
|---|---|
| 第一次接触这个项目 | [architecture/overview.md](architecture/overview.md) → [architecture/wine-internals.md](architecture/wine-internals.md) |
| 搭开发环境 | [build/env.md](build/env.md) |
| 构建、打包、查构建参数 | [build/guide.md](build/guide.md)、[cheatsheet.md](cheatsheet.md) |
| 部署到设备、看日志 | [cheatsheet.md](cheatsheet.md)、[debugging/observability.md](debugging/observability.md)、[debugging/remote-hdc.md](debugging/remote-hdc.md) |
| 遇到白屏、卡死、无声等问题 | [debugging/troubleshooting.md](debugging/troubleshooting.md) |
| 查性能问题 | [debugging/performance.md](debugging/performance.md) |
| 理解图形后端和档位 | [architecture/graphics-matrix.md](architecture/graphics-matrix.md) |
| 理解画面合成、窗口显示 | [architecture/compositor.md](architecture/compositor.md) |
| 理解鼠标键盘输入的链路 | [architecture/input.md](architecture/input.md) |
| 理解进程启动与环境变量 | [architecture/process-model.md](architecture/process-model.md) |
| 理解音频链路 | [architecture/audio.md](architecture/audio.md) |
| 理解跨仓库的私有协议 | [architecture/contracts.md](architecture/contracts.md) |
| 处理鸿蒙平台的限制（权限、沙箱、字体等） | [architecture/platform-ohos.md](architecture/platform-ohos.md) |
| 改 wine / dxvk / box64 等 submodule | [assets/submodules/](assets/submodules/)、[assets/submodule-maintainability.md](assets/submodule-maintainability.md) |
| 写自动化测试用例 | [engineering/testing-cases.md](engineering/testing-cases.md) |
| 了解自动化测试设施 | [engineering/testing-design.md](engineering/testing-design.md)、[../automation/README.md](../automation/README.md) |
| 提交代码、走分支流程 | [engineering/workflow.md](engineering/workflow.md) |
| 查代码规范和日志写法 | [engineering/coding.md](engineering/coding.md) |
| 发布版本 | [engineering/quality.md](engineering/quality.md)、[build/guide.md](build/guide.md) |
| 查设备信息 | [assets/devices.md](assets/devices.md) |
| 查证书、SDK、构建环境资产 | [assets/environment.md](assets/environment.md) |
| 查某个设计为什么这么定 | [decisions/](decisions/) |
| 遇到不认识的词 | [glossary.md](glossary.md) |

## 文档地图

### architecture/ — 架构与原理

| 文档 | 讲什么 |
|---|---|
| [overview.md](architecture/overview.md) | 总览：四大块（Wine / 合成器 / 音频 / 图形）怎么拼起来，进程之间怎么连 |
| [wine-internals.md](architecture/wine-internals.md) | Wine 内部的 PE 与 Unix 分层、wineserver 通信、合成器模块结构、日志纪律 |
| [graphics-matrix.md](architecture/graphics-matrix.md) | Direct3D 版本、渲染后端档位、GPU 通道三者的对应关系；档位怎么定 |
| [compositor.md](architecture/compositor.md) | 画面怎么拼出来、窗口叠加顺序、显示形态、桌面窗口识别 |
| [input.md](architecture/input.md) | 鼠标、键盘、滚轮、修饰键四条链路；相对指针模式、输入法 |
| [audio.md](architecture/audio.md) | 音频的控制面与数据面、多进程混音 |
| [process-model.md](architecture/process-model.md) | 进程怎么启动、环境变量怎么传（broker 机制） |
| [contracts.md](architecture/contracts.md) | 跨仓库私有协议一览（共享内存页、present 协议、环境变量约定等） |
| [platform-ohos.md](architecture/platform-ohos.md) | 鸿蒙平台适配：页面权限、noexec、沙箱路径、中文界面、设备能力差异 |
| [platform-memory-noexec.md](architecture/platform-memory-noexec.md) | noexec 文件系统上加可执行权限的处理（已解决，记录设计依据） |
| [platform-memory-ohos.md](architecture/platform-memory-ohos.md) | 鸿蒙内存映射权限的实测矩阵 |
| [opengl-virgl.md](architecture/opengl-virgl.md) | OpenGL / VirGL 通道的设计与环境变量约定 |

### build/ — 构建与发布

| 文档 | 讲什么 |
|---|---|
| [guide.md](build/guide.md) | 构建步骤、Makefile 各阶段、产物说明 |
| [env.md](build/env.md) | 从零搭构建环境（Docker / WSL2） |

### debugging/ — 日志、性能与排查

| 文档 | 讲什么 |
|---|---|
| [observability.md](debugging/observability.md) | 日志通道、日志标签速查、沙箱路径映射、崩溃定位 |
| [performance.md](debugging/performance.md) | 卡顿和性能问题的分析方法、性能统计开关 |
| [troubleshooting.md](debugging/troubleshooting.md) | 按问题现象查排查步骤 |
| [remote-hdc.md](debugging/remote-hdc.md) | hdc 跨机共享配置 |

### engineering/ — 工程规范

| 文档 | 讲什么 |
|---|---|
| [coding.md](engineering/coding.md) | 代码规范：日志、注释、判据收口、跨仓库协议修改规则 |
| [quality.md](engineering/quality.md) | 改什么跑什么、验证流程、发布前检查 |
| [workflow.md](engineering/workflow.md) | 分支模型、提交与审查、submodule 流程、协作约定 |
| [testing-design.md](engineering/testing-design.md) | 自动化测试设施的设计 |
| [testing-cases.md](engineering/testing-cases.md) | 怎么写一个测试用例、怎么挂进套件 |

### assets/ — 资产台账

| 文档 | 讲什么 |
|---|---|
| [devices.md](assets/devices.md) | 设备清单、形态差异、部署注意点 |
| [environment.md](assets/environment.md) | SDK、证书签名、构建配置、网络代理 |
| [submodule-maintainability.md](assets/submodule-maintainability.md) | 各 submodule 的分支现状与维护策略 |
| [submodules/wine.md](assets/submodules/wine.md) | wine fork 改了哪些文件（显示驱动、ntdll、wineserver 等） |
| [submodules/dxvk.md](assets/submodules/dxvk.md) | DXVK（Legacy 分支）的改动 |
| [submodules/box64.md](assets/submodules/box64.md) | box64 的改动（musl 适配等） |
| [submodules/mesa.md](assets/submodules/mesa.md) | mesa 的改动（Venus、VirGL 相关） |
| [submodules/virglrenderer.md](assets/submodules/virglrenderer.md) | virglrenderer 的改动 |
| [submodules/libepoxy.md](assets/submodules/libepoxy.md) | libepoxy 的小改动 |

### decisions/ — 决策记录

| 文档 | 讲什么 |
|---|---|
| [0001-self-built-compositor.md](decisions/0001-self-built-compositor.md) | 为什么继续维护自研合成器，不换成 weston / wlroots |
| [0002-d3d-backend-profiles.md](decisions/0002-d3d-backend-profiles.md) | Direct3D 后端档位怎么选（哪些设备用哪个） |

（后续补充：三方案共存、平板默认虚拟桌面等）

### 其他

| 文档 | 讲什么 |
|---|---|
| [cheatsheet.md](cheatsheet.md) | 常用命令与路径速查 |
| [glossary.md](glossary.md) | 术语表 |
| [archive/](archive/) | 已完成的方案、调研、状态快照、真机证据（只作记录） |

## 相关位置

- `../.claude/rules/` — 给 AI 助手用的操作约定（构建命令、部署流程、注意事项）
- `../automation/README.md` — 自动化测试设施的使用说明
- `../README.md` — 项目门面：功能状态、目录结构、关键适配点
