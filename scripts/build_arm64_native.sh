#!/bin/bash
# build_arm64_native.sh — Wayland compositor 所需的 ARM64 二进制依赖
# 产物: HonWine/entry/libs/arm64-v8a/ (.so) + HonWine/entry/src/main/cpp/include/ (头文件)
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/env.sh"

ARM64_TARGET="aarch64-linux-ohos"
HONWINE_LIBS="$HONWINE/entry/libs/arm64-v8a"
HONWINE_INC="$HONWINE/entry/src/main/cpp/include"
ARM64_BUILD="$BUILD_DIR/arm64_native"
ARM64_SYSROOT="$OHOS_SDK/native/sysroot"

log "=== 构建 ARM64 Wayland 依赖 (Compositor) ==="

mkdir -p "$HONWINE_LIBS" "$HONWINE_INC" "$ARM64_BUILD"

# ── ARM64 meson cross file ──
gen_arm64_cross() {
    local cross="$ARM64_BUILD/ohos-arm64-cross.txt"
    local ffi_prefix="$ARM64_BUILD/libffi/install"
    cat > "$cross" << XEOF
[binaries]
c = '$OHOS_SDK/native/llvm/bin/clang'
cpp = '$OHOS_SDK/native/llvm/bin/clang++'
ar = '$OHOS_SDK/native/llvm/bin/llvm-ar'
strip = '$OHOS_SDK/native/llvm/bin/llvm-strip'
pkg-config = '/usr/bin/pkg-config'

[built-in options]
c_args = ['--target=$ARM64_TARGET', '--sysroot=$ARM64_SYSROOT', '-I$ffi_prefix/include']
c_link_args = ['--target=$ARM64_TARGET', '--sysroot=$ARM64_SYSROOT', '-fuse-ld=lld', '-L$ffi_prefix/lib']

[host_machine]
system = 'linux'
cpu_family = 'aarch64'
cpu = 'aarch64'
endian = 'little'
XEOF
    echo "$cross"
}

# ── 1. libffi (ARM64) ──
build_libffi_arm64() {
    if [ -f "$HONWINE_LIBS/libffi.so.8" ]; then
        log "libffi (ARM64) 已就绪，跳过"
        return 0
    fi

    log "--- libffi (ARM64) ---"
    local src="$ROOT/thirdparty/libffi"
    local build="$ARM64_BUILD/libffi"
    mkdir -p "$build"
    cd "$build"

    "$src/autogen.sh" 2>/dev/null || true
    CC="$OHOS_SDK/native/llvm/bin/clang --target=$ARM64_TARGET --sysroot=$ARM64_SYSROOT" \
    CFLAGS="-O2 -fPIC -D__MUSL__" \
    LDFLAGS="-fuse-ld=lld" \
    "$src/configure" --host=aarch64-linux-gnu --prefix="$build/install" --disable-docs

    make -j$JOBS && make install

    # 只保留 SONAME
    cp "$build/install/lib/libffi.so.8.1.4" "$HONWINE_LIBS/libffi.so.8"
    ln -sf libffi.so.8 "$HONWINE_LIBS/libffi.so"
    log "libffi (ARM64) → $HONWINE_LIBS"
}

# ── 2. wayland (ARM64, server + client) ──
build_wayland_arm64() {
    if [ -f "$HONWINE_LIBS/libwayland-server.so.0" ]; then
        log "wayland (ARM64) 已就绪，跳过"
        return 0
    fi

    log "--- wayland (ARM64) ---"
    local src="$ROOT/thirdparty/wayland"
    local build="$ARM64_BUILD/wayland"
    local cross
    cross="$(gen_arm64_cross)"

    # libffi 头文件/库已在 cross file 的 c_args/c_link_args 中
    # 但 pkg-config 检测也需要能找到 libffi.pc
    export PKG_CONFIG_PATH="$ARM64_BUILD/libffi/install/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
    meson setup "$build" "$src" \
        --cross-file "$cross" \
        -Ddocumentation=false -Dtests=false -Dscanner=false

    ninja -C "$build"

    # 安装 .so 到 HonWine libs
    cp "$build/src/libwayland-server.so.0.22.0" "$HONWINE_LIBS/libwayland-server.so.0"
    cp "$build/src/libwayland-client.so.0.22.0" "$HONWINE_LIBS/libwayland-client.so.0"
    ln -sf libwayland-server.so.0 "$HONWINE_LIBS/libwayland-server.so"
    ln -sf libwayland-client.so.0 "$HONWINE_LIBS/libwayland-client.so"

    # 安装头文件 (源码头 + 构建生成)
    cp "$src/src/wayland-server-core.h" \
       "$src/src/wayland-server.h" \
       "$src/src/wayland-client-core.h" \
       "$src/src/wayland-client.h" \
       "$src/src/wayland-util.h" \
       "$HONWINE_INC/"
    # 以下在构建目录中生成
    cp "$build/src/wayland-server-protocol.h" \
       "$build/src/wayland-client-protocol.h" \
       "$build/src/wayland-version.h" \
       "$HONWINE_INC/"

    log "wayland (ARM64) → $HONWINE_LIBS"
}

# ── 3. xdg-shell + wayland 协议文件 ──
build_protocols() {
    if [ -f "$HONWINE/entry/src/main/cpp/xdg-shell-protocol.c" ]; then
        log "协议文件已就绪，跳过"
        return 0
    fi

    log "--- 生成 Wayland 协议文件 ---"
    local scanner="/usr/local/bin/wayland-scanner"

    # wayland core protocol
    local wl_xml="$ROOT/thirdparty/wayland/protocol/wayland.xml"
    "$scanner" server-header "$wl_xml" "$HONWINE_INC/wayland-server-protocol.h"
    "$scanner" client-header "$wl_xml" "$HONWINE_INC/wayland-client-protocol.h"
    "$scanner" code "$wl_xml" /dev/null  # 不需要 server/client code, libwayland 内置了

    # xdg-shell protocol
    local xdg_xml="$ROOT/thirdparty/wayland-protocols/stable/xdg-shell/xdg-shell.xml"
    local cpp_dir="$HONWINE/entry/src/main/cpp"
    "$scanner" server-header "$xdg_xml" "$HONWINE_INC/xdg-shell-server-protocol.h"
    "$scanner" client-header "$xdg_xml" "$HONWINE_INC/xdg-shell-client-protocol.h"
    "$scanner" private-code "$xdg_xml" "$cpp_dir/xdg-shell-protocol.c"

    log "协议文件 → $HONWINE_INC + $cpp_dir"
}

# ── main ──
build_libffi_arm64
build_wayland_arm64
build_protocols

log "ARM64 Wayland compositor 依赖就绪"
log "  libs:  $HONWINE_LIBS"
log "  inc:   $HONWINE_INC"
log "  proto: $HONWINE/entry/src/main/cpp/xdg-shell-protocol.c"
