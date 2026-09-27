#!/bin/bash
# build_native.sh — Native compositor (Wayland compositor) 依赖
# 产物: entry/libs/$NATIVE_ARCH/ (.so) + entry/src/main/cpp/protocols/ (协议头文件)
# 注意: 协议文件 (xdg-shell-protocol.c 等) 架构无关, 只生成一次
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/env.sh"

NATIVE_TARGET="${NATIVE_TARGET:-aarch64-linux-ohos}"
WINEHUA_INC="$WINEHUA/entry/src/main/cpp/protocols"
NATIVE_BUILD="$BUILD_DIR/native_${NATIVE_ARCH}"
if [ "$HOST_OS" = "Darwin" ] || [ "$HOST_OS" = "HarmonyOS" ]; then
    export PKG_CONFIG_PATH="$BUILD_DIR/host-tools/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
    export PKG_CONFIG_PATH_FOR_BUILD="$BUILD_DIR/host-tools/lib/pkgconfig${PKG_CONFIG_PATH_FOR_BUILD:+:$PKG_CONFIG_PATH_FOR_BUILD}"
fi

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
pkg-config = '$PKG_CONFIG_BIN'

[built-in options]
c_args = ['--target=$NATIVE_TARGET', '--sysroot=$SYSROOT', '-I$ffi_prefix/include']
c_link_args = ['--target=$NATIVE_TARGET', '--sysroot=$SYSROOT', '-fuse-ld=lld', '-L$ffi_prefix/lib']
# meson 构建机依赖查找 (wayland-scanner 等) 由此选项决定, 环境变量 PKG_CONFIG_PATH 会被它覆盖
build.pkg_config_path = ['$BUILD_DIR/host-tools/lib/pkgconfig']

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
    if [ "$HOST_OS" = "Darwin" ]; then
        CC="$OHOS_SDK/native/llvm/bin/clang" CCAS="$OHOS_SDK/native/llvm/bin/clang" \
        AR="$OHOS_SDK/native/llvm/bin/llvm-ar" \
        RANLIB="$OHOS_SDK/native/llvm/bin/llvm-ranlib" \
        NM="$OHOS_SDK/native/llvm/bin/llvm-nm" LD="$OHOS_SDK/native/llvm/bin/ld.lld" \
        CFLAGS="--target=$NATIVE_TARGET --sysroot=$SYSROOT -O2 -fPIC -D__MUSL__" \
        LDFLAGS="-fuse-ld=lld --sysroot=$SYSROOT --target=$NATIVE_TARGET" \
        "$src/configure" --host=${NATIVE_CPU}-linux-gnu --prefix="$build/install" --disable-docs
    else
        CC="$OHOS_SDK/native/llvm/bin/clang --target=$NATIVE_TARGET --sysroot=$SYSROOT" \
        CFLAGS="-O2 -fPIC -D__MUSL__" LDFLAGS="-fuse-ld=lld" \
        "$src/configure" --host=${NATIVE_CPU}-linux-gnu --prefix="$build/install" --disable-docs
    fi

    make -j$JOBS && make install

    # 只保留 SONAME
    cp "$build/install/lib/libffi.so.8.1.4" "$NATIVE_LIBS/libffi.so.8"
    ln -sf libffi.so.8 "$NATIVE_LIBS/libffi.so"
    log "libffi ($NATIVE_ARCH) → $NATIVE_LIBS"
}

copy_soname_with_linker_alias() {
    local src="$1"
    local soname="$2"
    local linker="${3:-}"

    [ -f "$src" ] || err "native lib source missing: $src"
    cp "$src" "$NATIVE_LIBS/$soname"
    if [ -n "$linker" ]; then
        cp "$src" "$NATIVE_LIBS/$linker"
    fi
}

# ── 2. ARM64 bridge libs used by Box64 wrapped native libraries ──
remove_native_egl_linker_stubs() {
    # The Native SDK files are link-time stubs. OHOS host processes must load
    # the public runtime implementations from /system/lib64 instead.
    rm -f "$NATIVE_LIBS/libEGL.so" "$NATIVE_LIBS/libEGL.so.1"
}

build_native_freetype() {
    if [ -f "$NATIVE_LIBS/libfreetype.so.6" ] && [ -f "$NATIVE_LIBS/libfreetype.so" ]; then
        log "freetype ($NATIVE_ARCH) 已就绪，跳过"
        return 0
    fi

    log "--- freetype ($NATIVE_ARCH) ---"
    local src="$ROOT/thirdparty/freetype"
    local build="$NATIVE_BUILD/freetype"
    rm -rf "$build"
    cmake "$src" -B "$build" -GNinja \
        -DCMAKE_SYSTEM_NAME=Linux \
        -DCMAKE_SYSTEM_PROCESSOR="$NATIVE_CPU" \
        -DCMAKE_C_COMPILER="$OHOS_SDK/native/llvm/bin/clang" \
        -DCMAKE_C_COMPILER_TARGET="$NATIVE_TARGET" \
        -DCMAKE_SYSROOT="$SYSROOT" \
        -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY \
        -DCMAKE_C_FLAGS="--target=$NATIVE_TARGET --sysroot=$SYSROOT -fPIC -D__MUSL__" \
        -DCMAKE_SHARED_LINKER_FLAGS="--target=$NATIVE_TARGET --sysroot=$SYSROOT -fuse-ld=lld" \
        -DCMAKE_EXE_LINKER_FLAGS="--target=$NATIVE_TARGET --sysroot=$SYSROOT -fuse-ld=lld" \
        -DCMAKE_BUILD_TYPE=Release \
        -DFT_DISABLE_ZLIB=ON \
        -DFT_DISABLE_BROTLI=ON \
        -DFT_DISABLE_HARFBUZZ=ON \
        -DFT_DISABLE_PNG=ON \
        -DFT_DISABLE_BZIP2=ON \
        -DBUILD_SHARED_LIBS=ON
    ninja -C "$build"

    copy_soname_with_linker_alias "$build/libfreetype.so.6.20.2" "libfreetype.so.6" "libfreetype.so"
    log "freetype ($NATIVE_ARCH) → $NATIVE_LIBS"
}

build_native_libxml2() {
    if [ -f "$NATIVE_LIBS/libxml2.so.2" ] \
       && [ -f "$NATIVE_LIBS/libxml2.so" ] \
       && [ -d "$NATIVE_BUILD/libxml2/install/include/libxml2/libxml" ] \
       && [ -f "$NATIVE_BUILD/libxml2/install/lib/pkgconfig/libxml-2.0.pc" ]; then
        log "libxml2 ($NATIVE_ARCH) 已就绪，跳过"
        return 0
    fi

    log "--- libxml2 ($NATIVE_ARCH) ---"
    local src="$ROOT/thirdparty/libxml2"
    local build="$NATIVE_BUILD/libxml2"
    rm -rf "$build"
    cmake -S "$src" -B "$build" -GNinja \
        -DCMAKE_TOOLCHAIN_FILE="$OHOS_SDK/native/build/cmake/ohos.toolchain.cmake" \
        -DOHOS_ARCH="$NATIVE_ARCH" -DOHOS_PLATFORM=OHOS \
        -DCMAKE_BUILD_TYPE=Release \
        -DLIBXML2_WITH_PYTHON=OFF -DLIBXML2_WITH_TESTS=OFF \
        -DLIBXML2_WITH_PROGRAMS=OFF -DLIBXML2_WITH_HTTP=OFF \
        -DLIBXML2_WITH_FTP=OFF -DLIBXML2_WITH_MODULES=OFF \
        -DLIBXML2_WITH_LZMA=OFF -DLIBXML2_WITH_ZLIB=OFF -DLIBXML2_WITH_ICONV=OFF \
        -DCMAKE_INSTALL_PREFIX="$build/install"
    cmake --build "$build"
    cmake --install "$build"

    copy_soname_with_linker_alias "$(compgen -G "$build/libxml2.so.2.*" | head -1)" "libxml2.so.2" "libxml2.so"
    log "libxml2 ($NATIVE_ARCH) → $NATIVE_LIBS"
}

build_native_xkbcommon() {
    # 版本取自源码; 装好后记 marker, 守卫校验一致 (防升级后被"文件存在"误跳过)
    local src="$ROOT/thirdparty/libxkbcommon"
    local ver
    ver=$(sed -n "s/^[[:space:]]*version[[:space:]]*:[[:space:]]*'\([^']*\)'.*/\1/p" "$src/meson.build" | head -1)
    [ -n "$ver" ] || { err "无法从 $src/meson.build 解析 version"; return 1; }

    if [ -f "$NATIVE_LIBS/libxkbcommon.so.0" ] \
       && [ -f "$NATIVE_LIBS/libxkbcommon.so" ] \
       && [ -f "$NATIVE_LIBS/libxkbregistry.so.0" ] \
       && [ -f "$NATIVE_LIBS/libxkbregistry.so" ] \
       && [ "$(cat "$NATIVE_LIBS/.xkbcommon_version" 2>/dev/null)" = "$ver" ]; then
        log "xkbcommon ($NATIVE_ARCH, $ver) 已就绪，跳过"
        return 0
    fi

    log "--- xkbcommon ($NATIVE_ARCH, $ver) ---"
    # 构建目录按版本隔离 (升级时旧 meson 缓存不可复用)
    local build="$NATIVE_BUILD/xkbcommon_$ver"
    local cross
    cross="$(gen_native_cross)"

    rm -rf "$build"
    export PKG_CONFIG_PATH="$NATIVE_BUILD/libxml2/install/lib/pkgconfig:$NATIVE_BUILD/libffi/install/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
    meson setup "$build" "$src" \
        --cross-file "$cross" \
        -Denable-x11=false \
        -Denable-wayland=false \
        -Denable-xkbregistry=true \
        -Denable-tools=false \
        -Denable-docs=false \
        -Denable-bash-completion=false
    ninja -C "$build"

    # 产物尾缀随版本变 (实测: 1.7.0=so.0.0.0, 1.13.2 不同), glob 取实际产物
    copy_soname_with_linker_alias "$(compgen -G "$build/libxkbcommon.so.0.*" | head -1)" "libxkbcommon.so.0" "libxkbcommon.so"
    copy_soname_with_linker_alias "$(compgen -G "$build/libxkbregistry.so.0.*" | head -1)" "libxkbregistry.so.0" "libxkbregistry.so"
    echo "$ver" > "$NATIVE_LIBS/.xkbcommon_version"
    log "xkbcommon ($NATIVE_ARCH, $ver) → $NATIVE_LIBS"
}

# ── 3. wayland (server + client) ──
build_wayland() {
    # 版本取自源码; 装好后记版本到 marker, 守卫校验 marker 与源码一致
    # (防升级 submodule 后被"文件存在"误跳过 — 实测: 1.22→1.26 时旧 .so 原样留存)
    local src="$ROOT/thirdparty/wayland"
    local ver
    ver=$(sed -n "s/^[[:space:]]*version[[:space:]]*:[[:space:]]*'\([^']*\)'.*/\1/p" "$src/meson.build" | head -1)
    [ -n "$ver" ] || { err "无法从 $src/meson.build 解析 version"; return 1; }

    if [ -f "$NATIVE_LIBS/libwayland-server.so.0" ] \
       && [ -f "$NATIVE_LIBS/libwayland-client.so.0" ] \
       && [ -f "$NATIVE_LIBS/libwayland-egl.so.1" ] \
       && [ "$(cat "$NATIVE_LIBS/.wayland_version" 2>/dev/null)" = "$ver" ]; then
        log "wayland ($NATIVE_ARCH, $ver) 已就绪，跳过"
        return 0
    fi

    log "--- wayland ($NATIVE_ARCH, $ver) ---"
    # 构建目录按版本隔离: 旧版本 meson 缓存不可复用 (同 build_wayland.sh 教训)
    local build="$NATIVE_BUILD/wayland_$ver"
    local cross
    cross="$(gen_native_cross)"

    # libffi 头文件/库已在 cross file 的 c_args/c_link_args 中
    export PKG_CONFIG_PATH="$NATIVE_BUILD/libffi/install/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
    meson setup "$build" "$src" \
        --cross-file "$cross" \
        -Ddocumentation=false -Dtests=false -Dscanner=false

    ninja -C "$build"

    # 安装 .so 到 Native libs (版本尾缀随 tag 走, glob 取实际产物, 取不到即失败)
    local f
    for f in server client; do
        local g
        g=$(ls "$build/src/libwayland-$f.so.0."* 2>/dev/null | head -1)
        [ -n "$g" ] || { err "未找到产物 libwayland-$f"; return 1; }
        cp "$g" "$NATIVE_LIBS/libwayland-$f.so.0"
    done
    g=$(ls "$build/egl/libwayland-egl.so.1."* 2>/dev/null | head -1)
    [ -n "$g" ] || { err "未找到产物 libwayland-egl"; return 1; }
    cp "$g" "$NATIVE_LIBS/libwayland-egl.so.1"
    ln -sf libwayland-server.so.0 "$NATIVE_LIBS/libwayland-server.so"
    ln -sf libwayland-client.so.0 "$NATIVE_LIBS/libwayland-client.so"
    ln -sf libwayland-egl.so.1    "$NATIVE_LIBS/libwayland-egl.so"
    echo "$ver" > "$NATIVE_LIBS/.wayland_version"

    log "wayland ($NATIVE_ARCH, $ver) → $NATIVE_LIBS"
}

# host 侧 wayland .pc: NATIVE_LIBS 里的 aarch64 .so 是 host 进程 (Xwayland 子进程、
# display_compositor) 的链接目标, 但此前只有 .so 没有 pc——host 消费者 (wlroots/
# meson_host_build) 会静默回退解析到 sysroot-ext 的 GUEST 侧 (x86_64) pc, 侧别错乱。
# 本函数无条件执行 (自身带版本守卫), 让 pc 与 .so 同源同版本。
emit_wayland_host_pc() {
    local src="$ROOT/thirdparty/wayland"
    local ver
    ver=$(sed -n "s/^[[:space:]]*version[[:space:]]*:[[:space:]]*'\([^']*\)'.*/\1/p" "$src/meson.build" | head -1)
    [ -n "$ver" ] || { err "emit_wayland_host_pc: 无法解析 version"; return 1; }

    [ -f "$NATIVE_LIBS/libwayland-server.so.0" ] \
        && [ -f "$NATIVE_LIBS/libwayland-client.so.0" ] \
        || { err "emit_wayland_host_pc: $NATIVE_LIBS 缺 wayland .so, 先跑 build_wayland"; return 1; }

    # 版本守卫: pc 已存在且 Version 行一致才跳过 (直接读文件, 不走 pkg-config——
    # 顶层 PKG_CONFIG_LIBDIR 里 guest 侧同名 pc 同版本, 会混淆判读)
    if [ -f "$HOST_EXT_PC/wayland-server.pc" ] && [ -f "$HOST_EXT_PC/wayland-client.pc" ] \
       && [ "$(sed -n 's/^Version: //p' "$HOST_EXT_PC/wayland-server.pc")" = "$ver" ]; then
        return 0
    fi

    cat > "$HOST_EXT_PC/wayland-client.pc" << EOF
Name: Wayland Client
Description: Wayland client side library (host 侧, $NATIVE_ARCH, 链接目标是 entry/libs)
Version: $ver
Requires.private: libffi
Libs: -L$NATIVE_LIBS -lwayland-client
Cflags: -I$WINEHUA_INC
EOF
    cat > "$HOST_EXT_PC/wayland-server.pc" << EOF
Name: Wayland Server
Description: Wayland server side library (host 侧, $NATIVE_ARCH, 链接目标是 entry/libs)
Version: $ver
Requires.private: libffi
Libs: -L$NATIVE_LIBS -lwayland-server
Cflags: -I$WINEHUA_INC
EOF
    log "wayland host pc ($NATIVE_ARCH, $ver) → $HOST_EXT_PC"
}

# ── 3. xdg-shell + wayland 协议文件 (架构无关, 只生成一次) ──
build_protocols() {
    if [ -f "$WINEHUA_INC/xdg-shell-protocol.c" ] \
       && [ -f "$WINEHUA_INC/winehua-toplevel-protocol.c" ]; then
        log "协议文件已就绪，跳过"
        return 0
    fi

    log "--- 生成 Wayland 协议文件 ---"
    local scanner="$WAYLAND_SCANNER"

    # wayland core protocol
    local wl_xml="$ROOT/thirdparty/wayland/protocol/wayland.xml"
    "$scanner" server-header "$wl_xml" "$WINEHUA_INC/wayland-server-protocol.h"
    "$scanner" client-header "$wl_xml" "$WINEHUA_INC/wayland-client-protocol.h"
    "$scanner" code "$wl_xml" /dev/null

    # xdg-shell protocol
    local xdg_xml="$ROOT/thirdparty/wayland-protocols/stable/xdg-shell/xdg-shell.xml"
    "$scanner" server-header "$xdg_xml" "$WINEHUA_INC/xdg-shell-server-protocol.h"
    "$scanner" client-header "$xdg_xml" "$WINEHUA_INC/xdg-shell-client-protocol.h"
    "$scanner" private-code "$xdg_xml" "$WINEHUA_INC/xdg-shell-protocol.c"

    # WineHua 私有协议 (权威源在 winewayland.drv/Makefile.in 同文件, 双端
    # 同一 XML; server 侧只需 server-header + private-code)
    local wh_xml="$ROOT/thirdparty/wine/dlls/winewayland.drv/winehua-toplevel.xml"
    "$scanner" server-header "$wh_xml" "$WINEHUA_INC/winehua-toplevel-server-protocol.h"
    "$scanner" private-code "$wh_xml" "$WINEHUA_INC/winehua-toplevel-protocol.c"

    log "协议文件 → $WINEHUA_INC"
}

# ── 4. wayland 头文件 (架构无关, 按版本守卫安装) ──
install_headers() {
    # 守卫比对已装 wayland-version.h 的 WAYLAND_VERSION 与源码版本, 不一致即重装。
    # 踩坑: 旧版用"文件存在"守卫, 1.22 时代的遗留头(实为更早的手工截短 stub,
    # 连 wl_proxy_get_queue 都没有)一直挡住 1.26 重装, 直到 wlroots 编译报
    # undeclared 才暴露——文件存在守卫在升级场景失明, 本项目第三次踩同款坑。
    local src="$ROOT/thirdparty/wayland"
    local ver
    ver=$(sed -n "s/^[[:space:]]*version[[:space:]]*:[[:space:]]*'\([^']*\)'.*/\1/p" "$src/meson.build" | head -1)
    [ -n "$ver" ] || { err "install_headers: 无法从 $src/meson.build 解析 version"; return 1; }

    local inst_ver
    inst_ver=$(sed -n 's/^#define WAYLAND_VERSION "\(.*\)"/\1/p' "$WINEHUA_INC/wayland-version.h" 2>/dev/null | head -1)
    if [ -n "$inst_ver" ] && [ "$inst_ver" = "$ver" ]; then
        log "wayland 头文件 ($ver) 已就绪，跳过"
        return 0
    fi

    log "--- 安装 wayland 头文件 (已装=${inst_ver:-无} → $ver) ---"
    # 生成头取自版本化构建目录 (T1 起构建目录带版本后缀, 未版本化旧路径已废)
    local build="$NATIVE_BUILD/wayland_$ver"
    [ -f "$build/src/wayland-version.h" ] \
        || { err "install_headers: $build 缺生成头, 先跑本脚本完成 build_wayland"; return 1; }

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
    log "wayland 头文件 ($ver) → $WINEHUA_INC"
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
    local src="$ROOT/thirdparty/virglrenderer"
    local build="$NATIVE_BUILD/virglrenderer"
    local cross
    local epoxy_pc="$NATIVE_BUILD/libepoxy/install/lib/pkgconfig"
    local python_with_yaml
    local config_stamp="$build/.winehua-config"
    local expected_config="venus=1;render-server-mode=thread;render-server-worker=thread;vulkan-dload=1"

    rm -f "$NATIVE_LIBS/libvirgl_test_server.so"

    if [ -f "$NATIVE_LIBS/libvirglrenderer.so.1" ] && \
       [ -f "$NATIVE_LIBS/libwinehua_vtest_server.so" ] && \
       [ -x "$NATIVE_LIBS/virgl_test_server" ] && \
       [ "$(cat "$config_stamp" 2>/dev/null || true)" = "$expected_config" ] && \
       [ -z "$(find "$src" -newer "$NATIVE_LIBS/libwinehua_vtest_server.so" -type f \
           \( -name '*.c' -o -name '*.h' -o -name 'meson.build' \) -print -quit 2>/dev/null)" ]; then
        log "virglrenderer ($NATIVE_ARCH) 已就绪，跳过"
        return 0
    fi

    log "--- virglrenderer ($NATIVE_ARCH) ---"

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
        -Dvenus=true \
        -Drender-server-mode=thread \
        -Drender-server-worker=thread \
        -Dvulkan-dload=true \
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
    cp "$build/install/lib/libwinehua_vtest_server.so" "$NATIVE_LIBS/" 2>/dev/null || \
        find "$build" -name 'libwinehua_vtest_server.so' -type f -exec cp {} "$NATIVE_LIBS/libwinehua_vtest_server.so" \;
    if [ -x "$build/install/libexec/virgl_render_server" ]; then
        cp "$build/install/libexec/virgl_render_server" "$NATIVE_LIBS/"
    fi
    printf '%s\n' "$expected_config" > "$config_stamp"
    rm -f "$NATIVE_LIBS/libvirgl_test_server.so"

    log "virglrenderer ($NATIVE_ARCH) → $NATIVE_LIBS"
}

# ── main ──
build_libffi
build_native_freetype
build_native_libxml2
build_native_xkbcommon
build_wayland
emit_wayland_host_pc
build_protocols
install_headers
build_libepoxy
build_virglrenderer
remove_native_egl_linker_stubs

log "Native compositor 依赖就绪 ($NATIVE_ARCH)"
log "  libs:  $NATIVE_LIBS"
log "  inc:   $WINEHUA_INC"
log "  proto: $WINEHUA/entry/src/main/cpp/xdg-shell-protocol.c"
