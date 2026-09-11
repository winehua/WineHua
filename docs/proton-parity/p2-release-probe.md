# P2 第一步：FEX Release 对齐探针（已实测）

> 执行：2026-09-11，工作树 `/home/liufeng/src/WineHua-proton-parity`
> 目的：在不换 FEX 版本的前提下，验证"官方构建参数"在本机可复现，并产出可计量产物。
> 结论：**可复现。** 两个 CPU DLL 均已用官方参数集构建并通过架构断言。

## 1. 参数（来自方案 §5.2 / Proton Makefile）

```text
CMAKE_BUILD_TYPE        = Release
ENABLE_FEXCORE_PROFILER = True
ENABLE_LTO              = False
BUILD_TESTING           = False
TUNE_CPU                = none
RANGES_NATIVE           = OFF
MINGW_TRIPLE            = arm64ec-w64-mingw32 / aarch64-w64-mingw32
```

对照：当前线上产物用的是 `RelWithDebInfo` + `TUNE_CPU=native` + `RANGES_NATIVE=ON`
+ `ENABLE_FEXCORE_PROFILER=OFF`（见 `fex-effective-config.json`）。

## 2. 前置条件（本次新发现）

1. **FEX 有自己的嵌套子模块**，未初始化时 configure 会失败：

   ```text
   CMake Error at Source/Common/CMakeLists.txt:1
     .../thirdparty/fex/Source/Common/cpp-optparse does not contain a CMakeLists.txt
   ```

   需要 `git -C thirdparty/fex submodule update --init --recursive`
   （`External/{vixl,fmt,xxhash,zydis,Catch2,tracy,range-v3,rpmalloc,...}`、
   `Source/Common/cpp-optparse`）。

2. **必须应用仓库自带的两个 FEX 补丁**，否则 `FEXCore/Source/Common/StringConv.h`
   在 llvm-mingw 20260826 下编译失败：

   ```text
   scripts/patches/fex-missing-includes.patch
   scripts/patches/fex-winapi-locale-stubs.patch
   ```

   应用后 `thirdparty/fex` 会有 4 个 `M` 文件：
   `FEXCore/Source/Common/StringConv.h`、`Source/Windows/Common/CRT/Alloc.cpp`、
   `Source/Windows/Common/CRT/IO.cpp`、`Source/Windows/Common/WinAPI/Misc.cpp`。
   （这与原工作树的"FEX 脏"来源一致，属于预期差异，不是本实验引入的新改动。）

3. **宿主机 cmake 3.22.1 + llvm-mingw 20260826 足够** configure 和构建；
   不必须进容器（容器用于 OHOS SDK 相关的其余组件）。

## 3. 命令

```bash
export LLVM_MINGW=/home/liufeng/src/WineHua-arm64ec/.temp/llvm-mingw-20260826-ucrt-ubuntu-22.04-x86_64
export PATH="$LLVM_MINGW/bin:$PATH"

# FEX 嵌套子模块
git -C thirdparty/fex submodule update --init --recursive

# 两个补丁
git -C thirdparty/fex apply "$PWD/scripts/patches/fex-missing-includes.patch"
git -C thirdparty/fex apply "$PWD/scripts/patches/fex-winapi-locale-stubs.patch"

# arm64ec CPU DLL（x64 模拟）
cmake -S thirdparty/fex -B build/parity/fex-ec-release \
  -DCMAKE_BUILD_TYPE=Release -DENABLE_FEXCORE_PROFILER=True -DENABLE_LTO=False \
  -DBUILD_TESTING=False -DTUNE_CPU=none -DRANGES_NATIVE=OFF \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/thirdparty/fex/Data/CMake/toolchain_mingw.cmake" \
  -DMINGW_TRIPLE=arm64ec-w64-mingw32
make -C build/parity/fex-ec-release -j"$(nproc)" arm64ecfex

# wow64 CPU DLL（x86 模拟）
cmake -S thirdparty/fex -B build/parity/fex-pe-release \
  -DCMAKE_BUILD_TYPE=Release -DENABLE_FEXCORE_PROFILER=True -DENABLE_LTO=False \
  -DBUILD_TESTING=False -DTUNE_CPU=none -DRANGES_NATIVE=OFF \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/thirdparty/fex/Data/CMake/toolchain_mingw.cmake" \
  -DMINGW_TRIPLE=aarch64-w64-mingw32
make -C build/parity/fex-pe-release -j"$(nproc)" wow64fex
```

## 4. 产物与验证

| 产物 | 大小（字节） | `llvm-readobj --file-headers` | SHA-256 |
| --- | --- | --- | --- |
| `build/parity/fex-ec-release/Bin/libarm64ecfex.dll` | 5 103 616 | `COFF-ARM64EC` / `IMAGE_FILE_MACHINE_ARM64EC (0xA641)` | `a064acfa6177aa659c59287cb4f65f5326362ee98dc7bd5789be2988b60bbe63` |
| `build/parity/fex-pe-release/Bin/libwow64fex.dll` | 4 575 232 | `COFF-ARM64` / `IMAGE_FILE_MACHINE_ARM64 (0xAA64)` | `ad3b868fa022283c4be1417213d4c4956b20147062c9a3546ec0ef311ca4861d` |

configure 后的 CMakeCache 已复核，全部参数为官方值：

```text
BUILD_TESTING:BOOL=False
CMAKE_BUILD_TYPE:STRING=Release
ENABLE_FEXCORE_PROFILER:BOOL=True
ENABLE_LTO:BOOL=False
RANGES_NATIVE:BOOL=OFF
TUNE_CPU:STRING=none
```

## 5. 与线上基线的对比（同版本 `86ff33bbe`，仅构建参数不同）

| 项 | 线上基线 | 本次 Release 对齐 |
| --- | --- | --- |
| `libarm64ecfex.dll` | 40 300 544 B | **5 103 616 B** |
| `libwow64fex.dll` | 40 972 288 B | **4 575 232 B** |
| 构建类型 | RelWithDebInfo | Release |
| Profiler | OFF | True |
| TUNE_CPU / RANGES_NATIVE | native / ON | none / OFF |
| BUILD_TESTING | ON（缓存残留） | False |

> 体积差异主要来自 `-g` 调试信息与 `-O2 -g` → `-O2 -DNDEBUG` 的变化。
> **体积不是性能证据，也不是根因结论**；它只说明"脚本声明的参数"与"实际生效参数"之前不一致。
> 这两个 DLL 尚未做功能与性能验证。

## 6. 仍未解决

- **没有 UnixLib**：`86ff33bbe` 这个版本根本不产出 `libwow64fex.so` / `libarm64ecfex.so`，
  所以本探针解决的是"构建参数对齐"，不是 P2 的"成套产物"。
- 未在设备上验证这两个 DLL 的功能正确性（缺 `hdc`）。
- 未引入 FEX 配置文件（`FEX_Config.json`），生效值仍为编译期默认（`MaxInst=5000`、
  `X87ReducedPrecision=false`、`ProfileStats` 受 profiler 开关影响）。

## 7. 下一步

见 `next-steps.md`：先决定 FEX 版本策略（整体切 `1cc4b93e` / 最小回移 UnixLib / 只做观测），
再把 UnixLib 目标接入 `build_fex.sh` 与 `assemble.sh`。
