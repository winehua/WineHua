# P2：UnixLib 回移与构建（已实测）

> 执行：2026-09-11，工作树 `/home/liufeng/src/WineHua-proton-parity`
> 提交：`bba5d2e build(fex): 回移 UnixLib 并接入 build_fex.sh (P2)`
> 结论：**四个 FEX 产物全部构建成功**，UnixLib 的 aarch64 属性与 Wine 所需导出均已断言。

## 1. 参考工程怎么做（Proton `Makefile.in` @ `5b89db94`）

```make
FEX_COMMON_CMAKE_ARGS = \
  -DENABLE_FEXCORE_PROFILER=True -DCMAKE_BUILD_TYPE=Release -DENABLE_LTO=False \
  -DBUILD_TESTING=False -DTUNE_CPU=none -DRANGES_NATIVE=OFF \
  -DOVERRIDE_VERSION=$$(cat $(FEX_SRC)/.git_describe) \
  -DOVERRIDE_HASH=$$(cat $(FEX_SRC)/.git_rev)

FEX_CMAKE_ARGS = $(FEX_COMMON_CMAKE_ARGS) \
  -DCMAKE_TOOLCHAIN_FILE=$(FEX_SRC)/Data/CMake/toolchain_mingw.cmake
FEX_aarch64_CMAKE_ARGS = -DMINGW_TRIPLE=aarch64-w64-mingw32 -DCMAKE_INSTALL_LIBDIR=.../lib/wine/aarch64-windows
FEX_arm64ec_CMAKE_ARGS = -DMINGW_TRIPLE=arm64ec-w64-mingw32 -DCMAKE_INSTALL_LIBDIR=.../lib/wine/aarch64-windows

$(eval $(call rules-cmake,fex,aarch64,windows))
$(eval $(call rules-cmake,fex,arm64ec,windows))

# 关键：UnixLib 是**独立 CMake 工程**，且**不带 mingw toolchain**
FEX_UNIXLIB_CMAKE_ARGS = $(FEX_COMMON_CMAKE_ARGS)
FEX_UNIXLIB_aarch64_CMAKE_ARGS = -DCMAKE_INSTALL_LIBDIR=.../lib/wine/aarch64-unix

$(eval $(call rules-source,fex_unixlib,$(SRCDIR)/FEX/Source/Windows/UnixLib))
$(eval $(call rules-cmake,fex_unixlib,aarch64,unix))
```

要点：两个 PE DLL 走 mingw toolchain，装到 `aarch64-windows/`；
**两个 UnixLib `.so` 是 aarch64 Unix 产物，走原生工具链，装到 `aarch64-unix/`**。
这也解释了为什么在当前 WineHua 基线上"只编两个 DLL"永远拿不到 UnixLib。

## 2. Wine 侧如何消费（已核对，无需改动）

```text
dlls/ntdll/unix/virtual.c:6243  case MemoryWineLoadUnixLibByName / ...Wow64:
                                → load_unixlib_by_name( name, &handle )
                                → get_unixlib_funcs( handle, wow, &funcs, &entry )
dlls/ntdll/unix/loader.c:1367   load_unixlib_by_name(): 组装 <dll_path>/aarch64-unix/<name>.so
                                再退化为 <dll_path>/<name>.so，逐个 dlopen
dlls/ntdll/unix/virtual.c:849   get_unixlib_funcs(): dlsym __wine_unix_call_funcs / ..._wow64_funcs
dlls/ntdll/unix/virtual.c:872   #ifdef __OHOS__: dlopen 只取文件名, 让系统 linker 搜 libs/
```

FEX 侧 `Source/Windows/UnixLib/FEXUnixLib.cpp` 正好导出
`extern "C" const unixlib_entry_t __wine_unix_call_funcs[]`，并提供：

```text
prctl(PR_GET_MEM_MODEL / PR_SET_MEM_MODEL)      硬件 TSO 探测与开关
prctl(PR_ARM64_SET_UNALIGN_ATOMIC)              未对齐原子
prctl(PR_SET_VMA, PR_SET_VMA_ANON_NAME)         匿名映射命名
shm_open/ftruncate/mmap/shm_unlink               SHM 统计创建/扩容/清理
```

并在新机制不可用时**回退**到旧的 `MemoryWineUnixFuncs` 路径（`FEXUnixLib.cpp:105,118`）。

## 3. 回移内容

补丁：`scripts/patches/fex-unixlib-backport.patch`（14 个文件，不含上游 `.github/*`）。

```text
126bcd365  winternl: Update enums          （补 MemoryWineLoad*/Unload 枚举, 8 行）
dbaf22372  Windows: Adds empty Linux side unix library
c09225f86  Windows: Load unixlib if possible
201bb7398  Windows/UnixLib: Adds support for Hardware TSO support
954581c75  Windows/UnixLib: Adds remaining helpers
6c58fef22  Windows/UnixLib: Fix loading with new MemoryWineLoadUnixLibByName mechanism
```

> 第一版漏了 `126bcd365`（它只改 `winternl.h`，不在我最初的路径过滤范围内），
> 表现为 `FEXUnixLib.cpp:105: use of undeclared identifier 'MemoryWineLoadUnixLibByName'`。
> 这正是方案 §8 要求的"逐个 cherry-pick 并编译验证"的价值所在。

应用顺序：`fex-missing-includes` → `fex-winapi-locale-stubs` → `fex-unixlib-backport`，
三者文件不重叠，互不冲突。

## 4. 构建

`scripts/build_fex.sh` 新增 `build_fex_unixlib()`：把
`$FEX_SRC/Source/Windows/UnixLib` 当独立工程 configure，
用 OHOS SDK clang（`--target=aarch64-linux-ohos --sysroot=$SYSROOT -fuse-ld=lld`）
构建并做两项断言。

```bash
# 在构建容器内（需要 OHOS SDK；路径按实际挂载调整）
docker run --rm \
  -v /home/liufeng/src/WineHua-proton-parity:/home/liufeng/src/WineHua-proton-parity \
  -v /home/liufeng/src/WineHua-arm64ec:/home/liufeng/src/WineHua-arm64ec:ro \
  -v /home/liufeng/opt/harmony/command-line-tools-6.1.1.290:/apps/harmony:ro \
  -e LLVM_MINGW=/home/liufeng/src/WineHua-arm64ec/.temp/llvm-mingw-20260826-ucrt-ubuntu-22.04-x86_64 \
  -w /home/liufeng/src/WineHua-proton-parity \
  winehua-dev bash -lc 'bash scripts/build_fex.sh'
```

> 容器必须按**同一绝对路径**挂载工作树：`thirdparty/fex/.git` 里是相对 gitdir，
> 换挂载点会报 `fatal: not a git repository`。
> 另：容器内以 root 构建，会在 `thirdparty/fex` 留下 root 所有的新目录，
> 清理需用容器 `rm -rf`（宿主 `git clean` 会 permission denied）。

## 5. 产物

| 产物 | 格式 | 大小 | SHA-256 |
| --- | --- | --- | --- |
| `build/fex-ec/Bin/libarm64ecfex.dll` | COFF-ARM64EC | 41 811 968 | `2e72742a9ab560b52d9f117aa40f8035b88940f55e6383fbfc290b2dafdf38f4` |
| `build/fex-pe/Bin/libwow64fex.dll` | COFF-ARM64 | 40 939 520 | `f00179a60d079a374980f5f8336709e0ddd7adff6231f02f432319843fb8f6ad` |
| `build/fex-unixlib/libwow64fex.so` | ELF64 AArch64 DYN | 16 880 | `9e09210b7303911cc145299d8ff13acee3ebe21e3650acb38c979f70f04033eea` |
| `build/fex-unixlib/libarm64ecfex.so` | ELF64 AArch64 DYN | 16 880 | `73045aead305f14ab3971959170b1832e4a98bb846faf7f30ed90281b031fcd4` |

断言输出（脚本内建）：

```text
OK: libwow64fex.so 为 aarch64 ELF
OK: libwow64fex.so 导出 __wine_unix_call_funcs
OK: libarm64ecfex.so 为 aarch64 ELF
OK: libarm64ecfex.so 导出 __wine_unix_call_funcs
FEX 构建完成 (arm64ecfex + wow64fex + 两个 UnixLib)
```

动态依赖：

```text
NEEDED  libc++_shared.so
NEEDED  libc.so
SONAME  libwow64fex.so
```

已验证现有 HAP 的 `libs/arm64-v8a/libc++_shared.so` 存在，依赖可满足。

## 6. 打包归位（已接入 assemble.sh，尚未真机验证）

UnixLib 的 `.so` 与两个 PE DLL **去向不同**：

- PE DLL → 运行时包 `wine-data.zip` 的 `bin/<pe_dir>/`（现状）。
- UnixLib `.so` → 应与 `ntdll.so` / `winevulkan.so` / `win32u.so` 同级，
  即 `entry/libs/arm64-v8a/`。候选路径由 `load_unixlib_by_name` 决定：
  `<dll_path>/aarch64-unix/<name>.so`，退化为 `<dll_path>/<name>.so`。

`scripts/assemble.sh` 的方案③分支已加入两行拷贝：把
`$BUILD_DIR/fex-unixlib/lib{wow64fex,arm64ecfex}.so` 与其它 Wine unix `.so`
一起放进 `$NATIVE_LIBS`（即 `entry/libs/<arch>/`）。文件缺失时只 `warn`，不中断。

**仍未改动 `wine_env*.cpp` / `wine_child.cpp`**：`WINEDLLPATH` / `WINEUNIXDIR`
的实际取值必须先在设备上确认。在没确认前改环境可能破坏当前可用的链路
（`docs/ARM64_SCHEME3_PERF_HANDOFF.md` §4.2 已记录过同类踩坑）。

## 7. 风险与未验证

| 项 | 说明 |
| --- | --- |
| 未上真机 | 四个产物都未在设备上加载验证；`hardware_tso` / SHM 统计的真实返回值未知 |
| 能力迁移 | 回移同时把旧的内联 TSO / SHM 实现从 `Allocator.cpp`(-224 行)、`SHMStats.cpp`(-79 行) 移走，改为经 UnixLib；`FEXUnixLib.cpp` 保留了到旧 `MemoryWineUnixFuncs` 的回退，但回退是否在 OHOS 生效未验证 |
| 生效配置 | 仍无 `FEX_Config.json`；`ProfileStats` / `MaxInst` / `X87ReducedPrecision` 仍是默认值 |
| 构建类型 | `build_fex.sh` 仍是 `RelWithDebInfo`；Release 对齐只在 `p2-release-probe.md` 里单独验证过 |
