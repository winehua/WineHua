#!/bin/bash
# build_native.sh — Native compositor (Wayland compositor) 依赖
# 产物: entry/libs/$NATIVE_ARCH/ (.so) + entry/src/main/cpp/include/ (头文件)
# 注意: 协议文件 (xdg-shell-protocol.c 等) 架构无关, 只生成一次
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/env.sh"

NATIVE_TARGET="${NATIVE_TARGET:-aarch64-linux-ohos}"
WINEHUA_INC="$WINEHUA/entry/src/main/cpp/include"
NATIVE_BUILD="$BUILD_DIR/native_${NATIVE_ARCH}"

log "=== 构建 Native 依赖 ($NATIVE_ARCH: $NATIVE_TARGET) ==="

mkdir -p "$NATIVE_LIBS" "$WINEHUA_INC" "$NATIVE_BUILD"

# ── native meson cross file ──
gen_native_cross() {
    local cross="$NATIVE_BUILD/ohos-${NATIVE_ARCH}-cross.txt"
    local ffi_prefix="$NATIVE_BUILD/libffi/install"
    cat > "$cross" << XEOF
[binaries]
c = '$OHOS_SDK/native/llvm/bin/clang'
cpp = '$OHOS_SDK/native/llvm/bin/clang++'
ar = '$OHOS_SDK/native/llvm/bin/llvm-ar'
strip = '$OHOS_SDK/native/llvm/bin/llvm-strip'
pkg-config = '/usr/bin/pkg-config'

[built-in options]
c_args = ['--target=$NATIVE_TARGET', '--sysroot=$SYSROOT', '-I$ffi_prefix/include']
c_link_args = ['--target=$NATIVE_TARGET', '--sysroot=$SYSROOT', '-fuse-ld=lld', '-L$ffi_prefix/lib']

[host_machine]
system = 'linux'
cpu_family = '$NATIVE_CPU_FAMILY'
cpu = '$NATIVE_CPU'
endian = 'little'
XEOF
    echo "$cross"
}

# ── 1. libffi ──
build_libffi() {
    if [ -f "$NATIVE_LIBS/libffi.so.8" ]; then
        log "libffi ($NATIVE_ARCH) 已就绪，跳过"
        return 0
    fi

    log "--- libffi ($NATIVE_ARCH) ---"
    local src="$ROOT/thirdparty/libffi"
    local build="$NATIVE_BUILD/libffi"
    mkdir -p "$build"
    cd "$build"

    "$src/autogen.sh" 2>/dev/null || true
    CC="$OHOS_SDK/native/llvm/bin/clang --target=$NATIVE_TARGET --sysroot=$SYSROOT" \
    CFLAGS="-O2 -fPIC -D__MUSL__" \
    LDFLAGS="-fuse-ld=lld" \
    "$src/configure" --host=${NATIVE_CPU}-linux-gnu --prefix="$build/install" --disable-docs

    make -j$JOBS && make install

    # 只保留 SONAME
    cp "$build/install/lib/libffi.so.8.1.4" "$NATIVE_LIBS/libffi.so.8"
    ln -sf libffi.so.8 "$NATIVE_LIBS/libffi.so"
    log "libffi ($NATIVE_ARCH) → $NATIVE_LIBS"
}

# ── 2. wayland (server + client) ──
build_wayland() {
    if [ -f "$NATIVE_LIBS/libwayland-server.so.0" ]; then
        log "wayland ($NATIVE_ARCH) 已就绪，跳过"
        return 0
    fi

    log "--- wayland ($NATIVE_ARCH) ---"
    local src="$ROOT/thirdparty/wayland"
    local build="$NATIVE_BUILD/wayland"
    local cross
    cross="$(gen_native_cross)"

    # libffi 头文件/库已在 cross file 的 c_args/c_link_args 中
    export PKG_CONFIG_PATH="$NATIVE_BUILD/libffi/install/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
    meson setup "$build" "$src" \
        --cross-file "$cross" \
        -Ddocumentation=false -Dtests=false -Dscanner=false

    ninja -C "$build"

    # 安装 .so 到 Native libs
    cp "$build/src/libwayland-server.so.0.22.0" "$NATIVE_LIBS/libwayland-server.so.0"
    cp "$build/src/libwayland-client.so.0.22.0" "$NATIVE_LIBS/libwayland-client.so.0"
    ln -sf libwayland-server.so.0 "$NATIVE_LIBS/libwayland-server.so"
    ln -sf libwayland-client.so.0 "$NATIVE_LIBS/libwayland-client.so"

    log "wayland ($NATIVE_ARCH) → $NATIVE_LIBS"
}

# ── 3. xdg-shell + wayland 协议文件 (架构无关, 只生成一次) ──
build_protocols() {
    if [ -f "$WINEHUA/entry/src/main/cpp/xdg-shell-protocol.c" ]; then
        log "协议文件已就绪，跳过"
        return 0
    fi

    log "--- 生成 Wayland 协议文件 ---"
    local scanner="/usr/local/bin/wayland-scanner"

    # wayland core protocol
    local wl_xml="$ROOT/thirdparty/wayland/protocol/wayland.xml"
    "$scanner" server-header "$wl_xml" "$WINEHUA_INC/wayland-server-protocol.h"
    "$scanner" client-header "$wl_xml" "$WINEHUA_INC/wayland-client-protocol.h"
    "$scanner" code "$wl_xml" /dev/null

    # xdg-shell protocol
    local xdg_xml="$ROOT/thirdparty/wayland-protocols/stable/xdg-shell/xdg-shell.xml"
    local cpp_dir="$WINEHUA/entry/src/main/cpp"
    "$scanner" server-header "$xdg_xml" "$WINEHUA_INC/xdg-shell-server-protocol.h"
    "$scanner" client-header "$xdg_xml" "$WINEHUA_INC/xdg-shell-client-protocol.h"
    "$scanner" private-code "$xdg_xml" "$cpp_dir/xdg-shell-protocol.c"

    log "协议文件 → $WINEHUA_INC + $cpp_dir"
}

# ── 4. wayland 头文件 (架构无关, 只安装一次) ──
install_headers() {
    if [ -f "$WINEHUA_INC/wayland-server-core.h" ]; then
        log "wayland 头文件已就绪，跳过"
        return 0
    fi

    log "--- 安装 wayland 头文件 ---"
    local src="$ROOT/thirdparty/wayland"
    local build="$NATIVE_BUILD/wayland"

    cp "$src/src/wayland-server-core.h" \
       "$src/src/wayland-server.h" \
       "$src/src/wayland-client-core.h" \
       "$src/src/wayland-client.h" \
       "$src/src/wayland-util.h" \
       "$WINEHUA_INC/"
    # 以下在构建目录中生成
    cp "$build/src/wayland-server-protocol.h" \
       "$build/src/wayland-client-protocol.h" \
       "$build/src/wayland-version.h" \
       "$WINEHUA_INC/"
    log "wayland 头文件 → $WINEHUA_INC"
}

# ── 5. libepoxy (VirGL host 渲染器依赖, EGL 函数加载) ──
build_libepoxy() {
    local epoxy_pc="$NATIVE_BUILD/libepoxy/install/lib/pkgconfig"
    if [ -f "$NATIVE_LIBS/libepoxy.so.0" ] && [ -d "$epoxy_pc" ]; then
        log "libepoxy ($NATIVE_ARCH) 已就绪，跳过"
        return 0
    fi

    log "--- libepoxy ($NATIVE_ARCH) ---"
    local src="$ROOT/thirdparty/libepoxy"
    local build="$NATIVE_BUILD/libepoxy"
    local cross

    [ -d "$src" ] || err "thirdparty/libepoxy is missing"

    cross="$(gen_native_cross)"

    rm -rf "$build"
    meson setup "$build" "$src" \
        --cross-file "$cross" \
        --prefix "$build/install" \
        --libdir lib \
        -Dglx=no \
        -Degl=yes \
        -Dx11=false \
        -Ddocs=false \
        -Dtests=false

    ninja -C "$build"
    meson install -C "$build"

    cp "$build/install/lib/libepoxy.so.0"* "$NATIVE_LIBS/libepoxy.so.0" 2>/dev/null || \
        find "$build/install/lib" -maxdepth 1 -name 'libepoxy.so.0*' ! -name '*.so.0.*' -exec cp {} "$NATIVE_LIBS/libepoxy.so.0" \;
    ln -sf libepoxy.so.0 "$NATIVE_LIBS/libepoxy.so"

    log "libepoxy ($NATIVE_ARCH) → $NATIVE_LIBS"
}

# ── 6. virglrenderer (host VirGL 渲染器 + vtest server) ──
find_python_with_yaml() {
    if python3 -c 'import yaml' >/dev/null 2>&1; then
        command -v python3
        return 0
    fi
    err "python3 with PyYAML is required. Install: pip3 install pyyaml"
}

build_virglrenderer() {
    if [ -f "$NATIVE_LIBS/libvirglrenderer.so.1" ] && [ -x "$NATIVE_LIBS/virgl_test_server" ]; then
        log "virglrenderer ($NATIVE_ARCH) 已就绪，跳过"
        return 0
    fi

    log "--- virglrenderer ($NATIVE_ARCH) ---"
    local src="$ROOT/thirdparty/virglrenderer"
    local build="$NATIVE_BUILD/virglrenderer"
    local cross
    local epoxy_pc="$NATIVE_BUILD/libepoxy/install/lib/pkgconfig"
    local python_with_yaml

    [ -d "$src" ] || err "thirdparty/virglrenderer is missing"
    [ -d "$epoxy_pc" ] || err "libepoxy pkg-config directory is missing, build libepoxy first: $epoxy_pc"
    python_with_yaml="$(find_python_with_yaml)"

    cross="$(gen_native_cross)"

    export PKG_CONFIG_PATH="$epoxy_pc${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
    # virglrenderer meson 需要 python3 找到 PyYAML
    export PATH="$(dirname "$python_with_yaml"):$PATH"

    rm -rf "$build"
    meson setup "$build" "$src" \
        --cross-file "$cross" \
        --prefix "$build/install" \
        --libdir lib \
        -Dplatforms=egl \
        -Dexternal-egl-without-gbm=true \
        -Dvtest=true \
        -Dvenus=false \
        -Dvideo=false \
        -Dtests=false \
        -Dfuzzer=false \
        -Dtracing=none

    ninja -C "$build"
    meson install -C "$build"

    # libvirglrenderer
    cp "$build/install/lib/libvirglrenderer.so.1"* "$NATIVE_LIBS/libvirglrenderer.so.1" 2>/dev/null || \
        find "$build/install/lib" -maxdepth 1 -name 'libvirglrenderer.so.1*' ! -name '*.so.1.*' -exec cp {} "$NATIVE_LIBS/libvirglrenderer.so.1" \;
    ln -sf libvirglrenderer.so.1 "$NATIVE_LIBS/libvirglrenderer.so"

    # virgl_test_server
    [ -f "$build/install/bin/virgl_test_server" ] || \
        find "$build" -name 'virgl_test_server' -type f -exec cp {} "$NATIVE_LIBS/virgl_test_server" \;
    cp "$build/install/bin/virgl_test_server" "$NATIVE_LIBS/" 2>/dev/null || true
    chmod +x "$NATIVE_LIBS/virgl_test_server" 2>/dev/null || true

    log "virglrenderer ($NATIVE_ARCH) → $NATIVE_LIBS"
}

# ── main ──
build_libffi
build_wayland
build_protocols
install_headers
build_libepoxy
build_virglrenderer

log "Native compositor 依赖就绪 ($NATIVE_ARCH)"
log "  libs:  $NATIVE_LIBS"
log "  inc:   $WINEHUA_INC"
log "  proto: $WINEHUA/entry/src/main/cpp/xdg-shell-protocol.c"
