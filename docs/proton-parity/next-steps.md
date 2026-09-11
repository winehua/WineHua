# 下一步（P2）可执行动作

> 前提：工作树 `/home/liufeng/src/WineHua-proton-parity`，分支 `feature/proton-arm64-parity`。
> 目标：在**不触碰原工作树**的前提下，让"成套 FEX + UnixLib"这件事从"阻塞"变成"有结论"。

## 关键决策（必须显式选择）

当前 FEX 基线 `86ff33bbe`（FEX-2605 线）与参考 `1cc4b93e`（FEX-2607）**分叉**，
不是同一条线上的新旧。两条路各有代价：

| 方案 | 做法 | 代价 | 对比变量 |
| --- | --- | --- | --- |
| A. 整体切 `1cc4b93e` | 把 `thirdparty/fex` 检出到 `1cc4b93e`，按官方参数重编 | 一次跨 321 提交，行为面变化大 | FEX 版本 + 可能的 Wine 接口要求 |
| B. 最小回移 UnixLib | 只把 UnixLib 相关提交 cherry-pick / 回移到 `86ff33bbe` | 需处理分叉冲突；可能牵出 TSO / ARM64EC 依赖 | 仅 FEX 增量 |
| C. 先只做观测 | 不换版本，先把生效配置、加载证据、统计可用性查清 | 不做性能验证，只补观测 | 无 |

建议顺序：**C → B（失败再 A）**。理由：先取证可以避免把"版本差异"当成"性能根因"，
也符合方案 §5 的边界要求。

## 步骤

### 步骤 1（C，低风险，先做）

1. 在隔离目录导出并固化当前编译期能力：

   ```bash
   cp -a build/fex-ec/CMakeCache.txt docs/proton-parity/evidence/fex-ec-CMakeCache.txt
   cp -a build/fex-pe/CMakeCache.txt docs/proton-parity/evidence/fex-pe-CMakeCache.txt
   ```

2. 新增**新目录**的配置探测构建（不改旧目录）：

   ```bash
   cmake -S thirdparty/fex -B build/parity/fex-probe \
     -DCMAKE_BUILD_TYPE=Release \
     -DENABLE_FEXCORE_PROFILER=True \
     -DENABLE_LTO=False \
     -DBUILD_TESTING=False \
     -DTUNE_CPU=none \
     -DRANGES_NATIVE=OFF \
     -DCMAKE_TOOLCHAIN_FILE=thirdparty/fex/Data/CMake/toolchain_mingw.cmake \
     -DMINGW_TRIPLE=arm64ec-w64-mingw32
   ```

   目的：确认这些官方参数在本机 / 容器里能不能 configure 通过，并留下缓存证据。

### 步骤 2（B，最小回移）

> **可行性已实测（2026-09-11）**：5 个 UnixLib 提交在 `86ff33bbe` 上 cherry-pick 全部无冲突，
> 且两版本间 `.gitmodules` 无差异（嵌套子模块 pin 未变）。测试工作树：
> `/tmp/fex-unixlib-test`（`git -C thirdparty/fex worktree add`，分支 `tmp/unixlib-backport`）。
> 另：Wine 侧 `dlls/ntdll/unix/virtual.c` 与 `dlls/wow64/virtual.c` **已实现**
> `MemoryWineLoadUnixLibByName` / `...Wow64` 与 `get_unixlib_funcs`，协议服务端已就绪。

1. 在 `thirdparty/fex` 内新建分支（例如 `feature/proton-parity-unixlib`），
   **不要**直接切到 `1cc4b93e` 交差。
2. 先列出 UnixLib 相关提交：

   ```bash
   git -C thirdparty/fex log --oneline a04b0241c..1cc4b93e -- Source/Windows/UnixLib Source/Windows/Common/FEXUnixLib.cpp Source/Windows/Common/FEXUnixLib.h
   ```

   已确认就是这 5 个：

   ```text
   dbaf22372 c09225f86 201bb7398 954581c75 6c58fef22
   ```

3. 逐个 cherry-pick，编译验证；冲突说明该提交耦合了别的改动 → 拆出来记录。
   **项目约定**：`thirdparty/fex` 指向上游 `FEX-Emu/FEX`，不能推分支，
   因此按 `fex-missing-includes.patch` 先例**导出为 `scripts/patches/fex-unixlib-backport.patch`**，
   由 `build_fex.sh` 统一应用（不要在主仓库登记子模块分支指针）。
4. `scripts/build_fex.sh` 增加 UnixLib 目标，构建目录用 `build/parity/fex-unixlib-*`。
   - 使用**原生**工具链（aarch64-linux-ohos 或容器 gcc），产出 AArch64 ELF `.so`。
   - 断言 `readelf -h` 的 `Machine: AArch64`。
5. `assemble.sh` / `Makefile` 把两个 `.so` 放进运行时包的 Unix 侧目录，
   文件名必须是 `libwow64fex.so` / `libarm64ecfex.so`。

### 步骤 3（验收，G0 → G1）

- [ ] 四个 FEX 产物都存在、架构正确、哈希入册（`artifact-sha256.txt`）。
- [ ] 运行时能在真实 HAP / NCP 内加载到 UnixLib，而不是"游戏进了菜单"。
- [ ] 硬件 TSO / 未对齐原子的**真实返回值**可见；失败不被成功桩掩盖。
- [ ] SHM 统计能创建、读取、按需扩容；线程结束与进程退出行为有验证。
- [ ] 生效配置可导出（编译期能力 + 文件哈希 + 进程生效值三者分开记录）。
- [ ] 没有引入二进制偏移补丁、初始化跳过、假等待、nodrv 成功桩、全局权限降级。

### 步骤 4（P3 / P4 才谈性能）

- x86 对照（A 完整 Wine+Box64 / B wowbox64 / C 旧 FEX / D 对齐 FEX），
  同 Wine、同图形栈、同游戏配置；不能用 32 位结果外推 x64。
- x64 + ARM64EC 图形验证独立开线，先 D3D11，vkd3d 单独验收。

## 环境备注

- 构建在 Docker 镜像 `winehua-dev` 内执行；`setup/` 不在 git 内，
  需要时从原工作树复制（勿提交）。
- `llvm-mingw` 复用原工作树 `.temp/llvm-mingw-20260826-...`；
  新工作树可通过 `export LLVM_MINGW=...` 指定，避免重复下载。
- WSL 访问 GitHub 需走 Windows 侧代理：`http://172.17.80.1:8080`
  （`127.0.0.1:8080` 在 WSL 内不可达）。

## 本轮不做

- 不升级 DXVK / vkd3d / Mesa / virglrenderer。
- 不改产品 `main` / `master`，不改原工作树。
- 不用 32 位 Heaven 结果外推 x64 ARM64EC 收益。
