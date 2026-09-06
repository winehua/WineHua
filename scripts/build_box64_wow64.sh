#!/bin/bash
# build_box64_wow64.sh — 构建 Box64 的 WoW64 模拟器 DLL (wowbox64.dll)
#
#   wowbox64.dll : i386 (32 位 x86) 模拟 — aarch64 ABI PE, 由 Wine 的
#                  get_cpu_dll_name() 在 WoW64 层内加载 (HODLL=wowbox64.dll)
#
# 参考 Hangover (https://github.com/AndreRH/hangover) docs/COMPILE.md +
# .packaging/ubuntu2204/boxpe/Dockerfile:
#   cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=aarch64-linux-gnu-gcc \
#         -DARM_DYNAREC=ON -DWOW64=ON ..
#   make -j$(nproc) wowbox64
#
# 产物:
#   build/box64-pe/wowbox64-prefix/src/wowbox64-build/wowbox64.dll
#   (assemble.sh 归位到 aarch64-windows/)
#
# 前置:
#   - LLVM_MINGW (aarch64-w64-mingw32-clang/as/dlltool) — wowbox64 子构建的 mingw 工具链
#   - aarch64-linux-gnu-gcc — 顶层 box64 configure 的 C 编译器 (只配置, 不编 box64 二进制)
#   - python3 — 生成 dynacache hashes
#
# 注: thirdparty/box64 是 winehua fork (OHOS 补丁). custommem.c 的 OHOS 专属
# POSIX 代码 (sys/mman.h include, /proc/self/maps 低 4GB 搜索) 已加 #ifndef _WIN32
# 守卫, 使同一源码树既能编原生 OHOS box64, 又能编 wowbox64.dll (Windows PE).
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/env.sh"

# 仅 arm64 原生 wine 需要 (i386 模拟)
[ "$WINE_ARCH" = "aarch64" ] || { log "box64 wow64 仅 arm64 原生需要 (WINE_ARCH=$WINE_ARCH)，跳过"; exit 0; }

BOX64_SRC="$ROOT/thirdparty/box64"
BUILD="$BUILD_DIR/box64-pe"

test -d "$BOX64_SRC/wine/wow64" || err "box64 源码缺失 wow64 支持: $BOX64_SRC/wine/wow64 (git submodule update --init)"
command -v aarch64-linux-gnu-gcc >/dev/null 2>&1 \
    || err "缺少 aarch64-linux-gnu-gcc (顶层 box64 configure 用; apt install gcc-aarch64-linux-gnu)"
test -x "$LLVM_MINGW/bin/aarch64-w64-mingw32-clang" || err "llvm-mingw 缺失 aarch64-w64-mingw32-clang: $LLVM_MINGW"
test -x "$LLVM_MINGW/bin/aarch64-w64-mingw32-dlltool" || err "llvm-mingw 缺失 aarch64-w64-mingw32-dlltool: $LLVM_MINGW"

export PATH="$LLVM_MINGW/bin:$PATH"

mkdir -p "$BUILD"
cd "$BUILD"
if [ ! -f CMakeCache.txt ]; then
    log "--- cmake configure (box64 wow64) ---"
    cmake -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_COMPILER=aarch64-linux-gnu-gcc \
        -DARM_DYNAREC=ON \
        -DWOW64=ON \
        "$BOX64_SRC"
fi
require_cmake_not_debug CMakeCache.txt "box64-pe"
require_cmake_flag_var CMakeCache.txt CMAKE_C_FLAGS_RELEASE "box64-pe CMAKE_C_FLAGS_RELEASE"
make -j"$JOBS" wowbox64

# 产物定位 (ExternalProject 子构建目录; hangover boxpe/Dockerfile 的路径)
DLL="$BUILD/wowbox64-prefix/src/wowbox64-build/wowbox64.dll"
test -f "$DLL" || err "wowbox64 构建失败: $DLL 不存在"
WOW_CACHE="$BUILD/wowbox64-prefix/src/wowbox64-build/CMakeCache.txt"
if [ -f "$WOW_CACHE" ]; then
    require_cmake_not_debug "$WOW_CACHE" "wowbox64.dll"
    require_cmake_flag_var "$WOW_CACHE" CMAKE_C_FLAGS_RELEASE "wowbox64.dll CMAKE_C_FLAGS_RELEASE"
fi

# aarch64 PE 验证 (llvm-readobj, mingw magic 不误判)
local_readobj="$LLVM_MINGW/bin/llvm-readobj"
if "$local_readobj" --file-headers "$DLL" 2>/dev/null | grep -q "COFF-ARM64"; then
    log "OK: wowbox64.dll 为 aarch64 PE"
else
    warn "架构异常: $("$local_readobj" --file-headers "$DLL" 2>/dev/null | grep -m1 'Format:')"
fi
log "产物: $DLL (assemble.sh 归位到 aarch64-windows/)"
