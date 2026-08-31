#!/bin/bash
# build_fex.sh — 构建 FEX 模拟器 DLL (arm64 原生 wine 转译 x86_64 / x86 应用)
#
#   libarm64ecfex.dll : x86_64 模拟 (必需) — arm64ec ABI, 由 Wine 的
#                       load_arm64ec_module() 在 ARM64EC/WoW64 层内加载 (HODLL64)
#   libwow64fex.dll   : i386 (32 位 x86) 模拟 — aarch64 ABI, 由 Wine 的
#                       get_cpu_dll_name() 在 WoW64 层内加载 (HODLL)
#
# 产物:
#   build/fex-ec/Bin/libarm64ecfex.dll  (assemble.sh 归位到 aarch64-windows/)
#   build/fex-pe/Bin/libwow64fex.dll    (assemble.sh 归位到 aarch64-windows/)
# 前置: LLVM_MINGW (llvm-mingw, 需 LLVM ≥ 18 支持 arm64ec) + thirdparty/fex
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/env.sh"

# 仅 arm64 原生 wine 需要 FEX (x86_64 模拟器同目标, 不需要转译)
[ "$WINE_ARCH" = "aarch64" ] || { log "FEX 仅 arm64 原生需要 (WINE_ARCH=$WINE_ARCH)，跳过"; exit 0; }

FEX_SRC="$ROOT/thirdparty/fex"
OUT_DIR="$BUILD_DIR/fex-ec/Bin"

test -d "$FEX_SRC" || err "FEX 源码缺失: $FEX_SRC (git submodule update --init)"
test -f "$FEX_SRC/Data/CMake/toolchain_mingw.cmake" || err "FEX toolchain_mingw.cmake 缺失"
test -x "$LLVM_MINGW/bin/arm64ec-w64-mingw32-clang" || err "llvm-mingw 缺失 arm64ec 支持: $LLVM_MINGW (需 LLVM ≥ 18)"
test -x "$LLVM_MINGW/bin/aarch64-w64-mingw32-clang" || err "llvm-mingw 缺失 aarch64-w64-mingw32-clang: $LLVM_MINGW"

export PATH="$LLVM_MINGW/bin:$PATH"

# 随构建走的 FEX 补丁 (子模块锁定 86ff33bbe 且指向 FEX-Emu/FEX 上游,
# 非 fork 不能推分支, 按 glib-format-security 先例 patch 化):
#   fex-missing-includes.patch   — 上游 08031a2767 "Add missing includes",
#     StringConv.h 等 3 文件缺 <cstdlib>/<stdarg.h>, 旧 llvm-mingw (20260616)
#     的 libc++ 靠传递 include 侥幸能编, 20260826 起不行.
#   fex-winapi-locale-stubs.patch — 20260826 libc++ 的 locale_win32.cpp.obj
#     引用 GetACP/GetLocaleInfoEx, 上游 master 在 WinAPI/Misc.cpp 以
#     UNIMPLEMENTED 桩解决, 回补到本树同名文件.
for PATCH in \
    "$SCRIPT_DIR/patches/fex-missing-includes.patch" \
    "$SCRIPT_DIR/patches/fex-winapi-locale-stubs.patch"; do
    if ! git -C "$FEX_SRC" apply --reverse --check "$PATCH" 2>/dev/null; then
        git -C "$FEX_SRC" apply "$PATCH"
        log "已应用 patch: $(basename "$PATCH")"
    fi
done

# ---- libarm64ecfex.dll (x86_64 模拟, arm64ec ABI) ----
build_fex_ec() {
    local build="$BUILD_DIR/fex-ec"
    mkdir -p "$build"
    cd "$build"
    if [ ! -f CMakeCache.txt ]; then
        # BUILD_TESTING=False: FEX 用 CTest 的 BUILD_TESTING (非 BUILD_TESTS)
        # 控制 unittests/, 开着会在 configure 阶段 enable_language(ASM_NASM)
        # 硬依赖 nasm — 我们只编 dll 目标, 显式关掉
        cmake -DCMAKE_BUILD_TYPE=RelWithDebInfo \
            -DCMAKE_TOOLCHAIN_FILE="$FEX_SRC/Data/CMake/toolchain_mingw.cmake" \
            -DENABLE_LTO=False \
            -DMINGW_TRIPLE=arm64ec-w64-mingw32 \
            -DBUILD_TESTING=False \
            "$FEX_SRC"
    fi
    make -j"$JOBS" arm64ecfex

    local dll="$OUT_DIR/libarm64ecfex.dll"
    test -f "$dll" || err "arm64ecfex 构建失败: $dll 不存在"
    # arm64ec 验证: file 显示 "x86-64" 是 magic 误识别, 以 llvm-readobj 为准
    local readobj="$LLVM_MINGW/bin/llvm-readobj"
    if "$readobj" --file-headers "$dll" 2>/dev/null | grep -q "COFF-ARM64EC"; then
        log "OK: libarm64ecfex.dll 为 arm64ec PE"
    else
        warn "架构异常: $("$readobj" --file-headers "$dll" 2>/dev/null | grep -m1 'Format:')"
    fi
    log "产物: $dll (assemble.sh 归位到 aarch64-windows/)"
}

# ---- libwow64fex.dll (i386 / 32 位 x86 模拟, aarch64 ABI) ----
# 与 arm64ecfex 使用不同 MINGW_TRIPLE (aarch64-w64-mingw32), 必须用独立 build
# 目录 (fex-pe), 避免 CMake 缓存与 arm64ec 配置互相覆盖。
build_fex_pe() {
    local build="$BUILD_DIR/fex-pe"
    mkdir -p "$build"
    cd "$build"
    if [ ! -f CMakeCache.txt ]; then
        cmake -DCMAKE_BUILD_TYPE=RelWithDebInfo \
            -DCMAKE_TOOLCHAIN_FILE="$FEX_SRC/Data/CMake/toolchain_mingw.cmake" \
            -DENABLE_LTO=False \
            -DMINGW_TRIPLE=aarch64-w64-mingw32 \
            -DBUILD_TESTING=False \
            "$FEX_SRC"
    fi
    make -j"$JOBS" wow64fex

    local dll="$build/Bin/libwow64fex.dll"
    test -f "$dll" || err "wow64fex 构建失败: $dll 不存在"
    local readobj="$LLVM_MINGW/bin/llvm-readobj"
    if "$readobj" --file-headers "$dll" 2>/dev/null | grep -q "COFF-ARM64"; then
        log "OK: libwow64fex.dll 为 aarch64 PE"
    else
        warn "架构异常: $("$readobj" --file-headers "$dll" 2>/dev/null | grep -m1 'Format:')"
    fi
    log "产物: $dll (assemble.sh 归位到 aarch64-windows/)"
}

build_fex_ec
build_fex_pe
log "FEX 构建完成 (arm64ecfex + wow64fex)"
