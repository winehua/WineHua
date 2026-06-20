# Wine ARM64 Native + ARM64EC (FEX) 移植方案

参考: HangOver (https://github.com/AndreRH/hangover)

## 总览

```
┌─────────────────────────────────────────────┐
│  x86_64 Windows App                         │
│    └─ 仿真: libarm64ecfex.dll (FEX PE DLL)   │
├─────────────────────────────────────────────┤
│  ARM64 Wine (aarch64-linux-ohos)             │  native OHOS
│    ├─ Unix .so: native ARM64                 │
│    └─ PE .dll: ARM64EC WoW64 层              │
├─────────────────────────────────────────────┤
│  HarmonyOS ARM64 (aarch64)                   │
└─────────────────────────────────────────────┘

32 位 x86: wowbox64.dll (现有 Box64 OHOS 移植, 不改)
```

## 关键发现

1. **Wine 不需要 patch**: ARM64/ARM64EC 全部在上游 Wine 中。HangOver 的 Wine fork 只有 1 个无关 commit。
2. **FEX PE DLL 不需要 OHOS 适配**: `libarm64ecfex.dll` 是 Windows PE DLL，跑在 Wine 里，用 Windows API。
3. **两个 mingw 工具链**: `llvm-mingw`(Wine PE) + `bylaws-llvm-mingw`(FEX ARM64EC)

## Phase 1: 工具链

### 1.1 llvm-mingw (Wine PE 交叉编译)
- 来源: https://github.com/mstorsjo/llvm-mingw/releases
- 提供: aarch64-w64-mingw32-{clang,clang++} 等
- 用法: `--with-mingw=clang`, PATH 中加入 llvm-mingw/bin
- 提供 ARM64EC target: `--target=arm64ec-w64-mingw32`

### 1.2 bylaws-llvm-mingw (FEX ARM64EC PE 编译)
- 来源: https://github.com/bylaws/llvm-mingw/releases
- 提供: arm64ec-w64-mingw32-{clang,clang++,dlltool,ar,windres}
- 用法: `MINGW_TRIPLE=arm64ec-w64-mingw32`, CMake toolchain_mingw.cmake
- 注意: 需要 `<triple>-clang` 等命名格式, bylaws 版本有 arm64ec triple

### 1.3 OHOS ARM64 sysroot 扩展
- FreeType, Wayland, xkbcommon 交叉编译为 aarch64-linux-ohos
- 安装到 `build/sysroot-ext/usr/lib/aarch64-linux-ohos/`

## Phase 2: Wine ARM64 编译

### 2.1 scripts/env.sh — 架构参数化
- `TARGET` 从 `NATIVE_ARCH` 推导: `arm64-v8a` → `aarch64-linux-ohos`
- `SYSROOT_EXT_LIB` → `$SYSROOT_EXT/usr/lib/$TARGET`
- `LLVM_MINGW` → llvm-mingw 安装路径
- `BYLAWS_MINGW` → bylaws-llvm-mingw 安装路径

### 2.2 scripts/build_wine.sh — ARM64 configure
```bash
export PATH=$LLVM_MINGW/bin:$PATH
# PE 交叉编译用 llvm-mingw
aarch64_CC=$LLVM_MINGW/bin/aarch64-w64-mingw32-clang
arm64ec_CC=$LLVM_MINGW/bin/arm64ec-w64-mingw32-clang

../configure \
  --host=aarch64-linux-ohos \
  --with-mingw=clang \
  --enable-archs=arm64ec,i386 \
  # 继承已有 WINE_CFLAGS + OHOS patches
```
Unix .so → `build-ohos/dlls/*/*.so` (ARM64 ELF)
PE .dll → `build-ohos/dlls/*/arm64ec-windows/` + `i386-windows/`

### 2.3 scripts/assemble.sh — ARM64 布局
- Unix .so → `libs/arm64-v8a/`
- PE .dll (arm64ec-windows + i386-windows) → rawfile zip
- libarm64ecfex.dll → rawfile `bin/arm64ec-windows/`
- 跳过 Box64 构建 (仅 32 位 wowbox64 仍用现有 box64)

## Phase 3: FEX 编译 (libarm64ecfex.dll)

### 3.1 源码
- FEX fork: https://github.com/AndreRH/FEX (arm64ec 分支, 2 commits)
- 或直接用上游 FEX (AndreRH 分支几乎没改动)

### 3.2 编译
```bash
export PATH=$BYLAWS_MINGW/bin:$PATH
# toolchain_mingw.cmake 已包含, 需要 MINGW_TRIPLE
cmake \
  -DCMAKE_TOOLCHAIN_FILE=Data/CMake/toolchain_mingw.cmake \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DMINGW_TRIPLE=arm64ec-w64-mingw32 \
  -DENABLE_LTO=False \
  -DBUILD_TESTS=False \
  ..
make -j$(nproc) arm64ecfex
```
产出: `Bin/libarm64ecfex.dll`

### 3.3 OHOS 适配
**不需要**。FEX 编译为 Windows PE DLL，运行在 Wine PE 环境内，只使用 Windows API (通过 Wine)。

## Phase 4: 部署

### assemble.sh 增加:
```bash
# FEX ARM64EC
cp $FEX_BUILD/Bin/libarm64ecfex.dll $RAW/arm64ec-windows/
# Wine PE DLLs
cp $WINE_BUILD/dlls/*/arm64ec-windows/*.dll $RAW/arm64ec-windows/
cp $WINE_BUILD/dlls/*/i386-windows/*.dll $RAW/i386-windows/
# Wine Unix .so (native ARM64)
cp $WINE_BUILD/dlls/*/*.so $NATIVE_LIBS/
```

## Phase 5: 测试

1. wineboot --init (ARM64 native)
2. notepad.exe (x86_64 via ARM64EC)
3. explorer /desktop

## 与现有 x86_64 构建的关系

两种构建模式共存:
- `NATIVE_ARCH=arm64-v8a`: ARM64 native Wine + FEX ARM64EC
- `NATIVE_ARCH=x86_64`: x86_64 Wine + Box64 (现有, 不变)
