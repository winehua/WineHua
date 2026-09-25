#!/bin/bash
# assemble.sh — 组装 HAP 打包布局
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/env.sh"

# ============================================================
# 文件分流到 libs/ + rawfile/
# ============================================================
assemble_pad() {
    log "=== 组装布局 ($NATIVE_ARCH) ==="

    local wine_data="$STAGING_DIR/wine-data"
    local guest_arch="${GUEST_ARCH:-$WINE_ARCH}"
    # Wine 构建目录按 WINE_ARCH 隔离 (build_wine.sh 的 wine-ohos-$WINE_ARCH / wine_server-$WINE_ARCH)
    local wine_build_dir="$BUILD_DIR/wine-ohos-$WINE_ARCH"
    local wine_server_dir="$BUILD_DIR/wine_server-$WINE_ARCH"
    # 方案② box64+wine: arm64 设备 + x86_64 wine 全转译 (wine/guest .so 由 box64 加载)
    local is_box64_scheme=0
    [ "$WINE_ARCH" = "x86_64" ] && [ "$NATIVE_ARCH" = "arm64-v8a" ] && is_box64_scheme=1
    # Wine PE 目录 (arm64 原生: aarch64-windows 合并 arm64ec; x86_64: x86_64-windows)
    local wine_pe_dir="x86_64-windows"
    local pe_src_dirs="$wine_pe_dir"
    local smoke_src_dir="x86_64-windows"
    if [ "$WINE_ARCH" = "aarch64" ]; then
        wine_pe_dir="aarch64-windows"
        pe_src_dirs="aarch64-windows arm64ec-windows"
        # system DLL/exe 为 ARM64EC+ARM64 混合, 统一在 aarch64-windows;
        # arm64ec-windows 仅产 .o (ABI 编译验证) → smoke 也取 aarch64-windows
        smoke_src_dir="aarch64-windows"
    fi
    # ARM64X DXVK/VKD3D 由 llvm-mingw clang++ 链接, 导入 libc++.dll + libunwind.dll。
    # 原生 ARM64 smoke 加载 ARM64X d3d11 时若缺这两颗会 STATUS_DLL_NOT_FOUND (c0000135)。
    # 优先 20260826 工具链: 其 aarch64 libc++ 本身也是 ARM64X, 与 DXVK 双图匹配。
    copy_arm64x_cxx_runtime() {
        local dest="$1"
        local cxx_root=""
        local cand
        for cand in \
            "$ROOT/.temp/llvm-mingw-20260826-ucrt-ubuntu-22.04-x86_64/aarch64-w64-mingw32/bin" \
            "/data/share/winebox/.temp/llvm-mingw-20260826-ucrt-ubuntu-22.04-x86_64/aarch64-w64-mingw32/bin" \
            "$LLVM_MINGW/aarch64-w64-mingw32/bin"
        do
            if [ -f "$cand/libc++.dll" ] && [ -f "$cand/libunwind.dll" ]; then
                cxx_root="$cand"
                break
            fi
        done
        [ -n "$cxx_root" ] || err "ARM64X C++ runtime missing (libc++.dll / libunwind.dll)"
        cp "$cxx_root/libc++.dll" "$dest/libc++.dll"
        cp "$cxx_root/libunwind.dll" "$dest/libunwind.dll"
        log "    ARM64X C++ runtime ← $cxx_root → $dest"
    }
    # GStreamer discovers plugins through GST_PLUGIN_PATH.  Keep modules in the
    # architecture-specific rawfile directory even when the Wine unixlibs live
    # in the HAP native-library directory.
    package_gstreamer_plugins() {
        local gst_plugin_dir="$SYSROOT_EXT_LIB/gstreamer-1.0"
        local gst_dest="$wine_data/bin/$WINE_ARCH-unix/gstreamer-1.0"
        local plugin_count=0
        local pso required

        [ -d "$gst_plugin_dir" ] || err "GStreamer plugin directory missing: $gst_plugin_dir"
        mkdir -p "$gst_dest"
        for pso in "$gst_plugin_dir"/*.so; do
            [ -f "$pso" ] || continue
            cp -L "$pso" "$gst_dest/"
            plugin_count=$((plugin_count + 1))
        done
        [ "$plugin_count" -gt 0 ] || err "No GStreamer plugins found in: $gst_plugin_dir"

        # These modules cover the Wine media baseline: core discovery, common
        # playback, MP4/Matroska/ASF containers, H.264 parsing and libav decode.
        for required in \
            libgstcoreelements.so libgsttypefindfunctions.so libgstplayback.so \
            libgstisomp4.so libgstmatroska.so libgstasf.so \
            libgstvideoparsersbad.so libgstlibav.so; do
            [ -s "$gst_dest/$required" ] || err "Required GStreamer plugin missing: $gst_dest/$required"
        done
        log "    GStreamer plugins ($plugin_count) → rawfile bin/$WINE_ARCH-unix/gstreamer-1.0/"
    }

    verify_runtime_components() {
        local runtime_lib_dir="$NATIVE_LIBS"
        local gst_plugin_dir="$wine_data/bin/$WINE_ARCH-unix/gstreamer-1.0"
        local component_manifest="$wine_data/runtime-components.json"
        local plugin_count font_count mono_status="disabled"
        local gstreamer_version="1.24.4"
        local mono_version="11.1.0"
        local mono_sha256=""
        local llvm_readelf="$OHOS_SDK/native/llvm/bin/llvm-readelf"
        local plugin
        local required

        if [ "$is_box64_scheme" = "1" ]; then
            runtime_lib_dir="$wine_data/bin/$WINE_ARCH-unix"
        fi

        for required in \
            libglib-2.0.so.0 libgstreamer-1.0.so.0 libgstbase-1.0.so.0 \
            libgstvideo-1.0.so.0 libgstaudio-1.0.so.0 libgsttag-1.0.so.0 \
            libgstcodecparsers-1.0.so.0 libgstmpegts-1.0.so.0 \
            libavcodec.so.60 libavformat.so.60 libavutil.so.58 libintl.so; do
            [ -s "$runtime_lib_dir/$required" ] || \
                err "Runtime component missing: $runtime_lib_dir/$required"
        done
        grep -Fxq "Version: $gstreamer_version" "$SYSROOT_EXT_PC/gstreamer-1.0.pc" || \
            err "Unexpected GStreamer version in: $SYSROOT_EXT_PC/gstreamer-1.0.pc"
        for required in \
            libgstcoreelements.so libgsttypefindfunctions.so libgstplayback.so \
            libgstisomp4.so libgstmatroska.so libgstasf.so \
            libgstvideoparsersbad.so libgstlibav.so; do
            [ -s "$gst_plugin_dir/$required" ] || \
                err "Packaged GStreamer plugin missing: $gst_plugin_dir/$required"
        done
        [ -x "$llvm_readelf" ] || err "llvm-readelf missing: $llvm_readelf"
        "$llvm_readelf" --dynamic-table "$runtime_lib_dir/libintl.so" 2>/dev/null | \
            grep -Fq 'Library soname: [libintl.so]' || \
            err "libintl.so is missing DT_SONAME"
        for plugin in "$gst_plugin_dir"/*.so; do
            "$llvm_readelf" --dynamic-table "$plugin" 2>/dev/null | \
                grep -q 'Shared library: \[[^]]*/' && \
                err "GStreamer plugin has an absolute DT_NEEDED path: $plugin"
        done

        if [ "${BUILD_WINE_MONO:-0}" = "1" ]; then
            local packaged_mono="$wine_data/share/wine/mono/wine-mono-${mono_version}-x86.msi"
            [ -s "$packaged_mono" ] || err "Packaged Wine Mono MSI missing: $packaged_mono"
            mono_sha256="$(sha256sum "$packaged_mono" | awk '{print $1}')"
            [ "$mono_sha256" = "deb0341431f8260b209fff6bc79ddcc5414b97f8e9236ab9fbdca4ce59e0a9b9" ] || \
                err "Unexpected Wine Mono checksum: $packaged_mono"
            [ -s "$wine_data/bin/$wine_pe_dir/appwiz.cpl" ] || \
                err "Wine Mono requires appwiz.cpl: $wine_data/bin/$wine_pe_dir/appwiz.cpl"
            mono_status="bundled"
        fi

        font_count="$(find "$wine_data/share/wine/fonts" -maxdepth 1 -type f -name '*.ttf' | wc -l)"
        [ "$font_count" -gt 0 ] || err "Wine font payload is empty"
        plugin_count="$(find "$gst_plugin_dir" -maxdepth 1 -type f -name '*.so' | wc -l)"

        cat > "$component_manifest" <<EOF
{
  "schemaVersion": 1,
  "wineArch": "$WINE_ARCH",
  "gstreamer": {
    "version": "$gstreamer_version",
    "pluginDirectory": "bin/$WINE_ARCH-unix/gstreamer-1.0",
    "pluginCount": $plugin_count,
    "requiredPlugins": ["libgstcoreelements.so", "libgsttypefindfunctions.so", "libgstplayback.so", "libgstisomp4.so", "libgstmatroska.so", "libgstasf.so", "libgstvideoparsersbad.so", "libgstlibav.so"]
  },
  "mono": {
    "status": "$mono_status",
    "version": "$mono_version",
    "msi": "share/wine/mono/wine-mono-${mono_version}-x86.msi",
    "sha256": "$mono_sha256"
  },
  "fonts": {
    "ttfCount": $font_count
  },
  "gecko": {
    "status": "not-bundled",
    "reason": "not part of the direct-game runtime profile"
  }
}
EOF
        log "  runtime component verification: GStreamer=$plugin_count plugins, Mono=$mono_status, fonts=$font_count"
    }
    rm -rf "$STAGING_DIR"
    rm -rf "$wine_data"
    # 方案② 清理 entry/libs 残留: 该目录由 build_native (wayland/xkbcommon/ffi) +
    # 方案③ assemble (aarch64 wine/mesa/venus) 共同填充, 方案② 只往里面放少量
    # aarch64 桥接库 + box64.so — 不清洗则方案③ 残留 (80+ 库) 混入 HAP 膨胀 100M+。
    # 清洗方式 = 快照移走 + 白名单补回 (不从 sysroot-ext/usr/lib/aarch64-linux-ohos
    # 取: 该目录只有方案③ 的 deps 构建才会填充, 纯方案②流程为空 → 干净构建断裂;
    # build_native.sh 的产物 (freetype/xkbcommon/wayland/ffi/epoxy/virgl/vtest)
    # 是本流程内 arm64 原生库的唯一可靠来源, 与基线 b6c65a0 语义一致)。
    # 方案①/③ 各自完整填充 (自洽), 不清洗 (清理会破坏 build_native 已放入的
    # wayland/xkbcommon 库, 而 ninja 链接 libentry.so 直接依赖这些文件)。
    local libs_snapshot=""
    if [ "$is_box64_scheme" = "1" ]; then
        libs_snapshot="$BUILD_DIR/.libs_snapshot_$NATIVE_ARCH"
        rm -rf "$libs_snapshot"
        if [ -d "$NATIVE_LIBS" ]; then
            mv "$NATIVE_LIBS" "$libs_snapshot"
        else
            mkdir -p "$libs_snapshot"
        fi
        mkdir -p "$NATIVE_LIBS"
    fi
    mkdir -p "$wine_data/bin/$wine_pe_dir"
    mkdir -p "$wine_data/bin/$WINE_ARCH-unix"
    mkdir -p "$wine_data/share/wine/nls"
    mkdir -p "$wine_data/share/wine/fonts"
    mkdir -p "$wine_data/share/wine/winmd"
    mkdir -p "$wine_data/share/wine/mono"
    mkdir -p "$wine_data/share/X11"

    # SoundFont (MIDI 音色库)
    local soundfont="$WINEHUA/entry/src/main/resources/rawfile/winehua-gm.sf2"
    if [ -f "$soundfont" ]; then
        mkdir -p "$wine_data/audio"
        cp "$soundfont" "$wine_data/audio/winehua-gm.sf2"
        log "    winehua-gm.sf2 → rawfile audio/"
    else
        warn "winehua-gm.sf2 not found; MIDI output will be unavailable"
    fi

    # -- 1. 原生 .so → libs/$NATIVE_ARCH/ (由各 build 脚本完成) --
    mkdir -p "$NATIVE_LIBS"

    if [ "$NATIVE_ARCH" = "x86_64" ]; then
        # x86_64 Pad: Wine .so 是原生架构, 直接放 libs/
        log "  → Wine .so → libs/x86_64/"

        # 所有 Wine Unix .so → libs/x86_64/ (系统 linker 通过文件名搜索)
        for so in "$wine_build_dir/dlls/"*/*.so; do
            cp "$so" "$NATIVE_LIBS/"
        done
        log "    Wine .so: $(ls "$wine_build_dir/dlls/"*/*.so 2>/dev/null | wc -l) files"

        # 交叉编译依赖 → libs/x86_64/
        # (系统 linker 自动搜索此路径, 无需 x86_64-unix 子目录)
        _pick_lib_pad() {
            local name="$1" soname="$2" linker="${3:-}"
            local dest="$NATIVE_LIBS"
            if [ -f "$SYSROOT_EXT_LIB/$soname" ]; then
                cp -L "$SYSROOT_EXT_LIB/$soname" "$dest/$soname"
            elif [ -f "$SYSROOT/usr/lib/$TARGET/$name" ]; then
                cp -L "$SYSROOT/usr/lib/$TARGET/$name" "$dest/$soname"
            else
                warn "$soname 未找到"
                return 0
            fi
            if [ -n "$linker" ] && [ ! -f "$dest/$linker" ]; then
                cp "$dest/$soname" "$dest/$linker"
            fi
        }
        _pick_lib_pad "libfreetype.so.6.20.2"       "libfreetype.so.6"   "libfreetype.so"
        _pick_lib_pad "libz.so"                      "libz.so"
        _pick_lib_pad "libwayland-client.so.0.22.0"  "libwayland-client.so.0"
        _pick_lib_pad "libwayland-egl.so.1.22.0"     "libwayland-egl.so.1"
        _pick_lib_pad "libxkbcommon.so.0.0.0"        "libxkbcommon.so.0"
        _pick_lib_pad "libxkbregistry.so.0.0.0"      "libxkbregistry.so.0"
        _pick_lib_pad "libxml2.so.2.12.0"            "libxml2.so.2"
        _pick_lib_pad "libffi.so.8.1.4"              "libffi.so.8"
        # GnuTLS 链 (schannel TLS 后端)
        _pick_lib_pad "libgnutls.so.30.37.1"         "libgnutls.so.30"   "libgnutls.so"
        _pick_lib_pad "libnettle.so.8.11"            "libnettle.so.8"
        _pick_lib_pad "libhogweed.so.6.11"           "libhogweed.so.6"
        _pick_lib_pad "libgmp.so.10.4.1"             "libgmp.so.10"
        _pick_lib_pad "libtasn1.so.6.6.4"            "libtasn1.so.6"
        _pick_lib_pad "libunistring.so.5.2.0"        "libunistring.so.5"
        _pick_lib_pad "libm.so"                      "libm.so"
        # GStreamer 链 (winegstreamer 后端)
        for so in libglib-2.0.so.0 libgobject-2.0.so.0 libgmodule-2.0.so.0 libgio-2.0.so.0 \
                  libgthread-2.0.so.0 libpcre2-8.so.0 libintl.so.8 libintl.so libm.so \
                  libgstreamer-1.0.so.0 libgstbase-1.0.so.0 libgstcontroller-1.0.so.0 \
                  libgstnet-1.0.so.0 libgstvideo-1.0.so.0 libgstaudio-1.0.so.0 \
                  libgsttag-1.0.so.0 libgstpbutils-1.0.so.0 libgstallocators-1.0.so.0 \
                  libgstapp-1.0.so.0 libgstfft-1.0.so.0 libgstriff-1.0.so.0 \
                   libgstrtp-1.0.so.0 libgstrtsp-1.0.so.0 libgstsdp-1.0.so.0 \
                   libgstcodecparsers-1.0.so.0 libgstmpegts-1.0.so.0; do
            _pick_lib_pad "$so" "$so"
        done
        # gst-libav is a plugin; its FFmpeg shared objects must be visible to
        # the native linker when it is loaded from the rawfile plugin path.
        for so in libavcodec.so.60 libavformat.so.60 libavutil.so.58 \
                  libswscale.so.7 libswresample.so.4 libavfilter.so.9; do
            _pick_lib_pad "$so" "$so"
        done
        package_gstreamer_plugins
        log "    交叉编译依赖 → libs/x86_64/"

        # libc.so → libs/x86_64/
        cp "$SYSROOT/usr/lib/$TARGET/libc.so" "$NATIVE_LIBS/"

        # libfreetype 已由 _pick_lib_pad 放入 libs/x86_64/，系统 linker 可直接找到

        # libwineserver.so (Pad fork+dlopen 入口)
        if [ -f "$wine_server_dir/libwineserver.so" ]; then
            cp "$wine_server_dir/libwineserver.so" "$NATIVE_LIBS/"
            log "    libwineserver.so → libs/x86_64/"
        else
            warn "libwineserver.so 未找到！请先执行: bash scripts/build_wine.sh"
        fi
    elif [ "$is_box64_scheme" = "1" ]; then
        # 方案② box64+wine: arm64 设备 + x86_64 wine 全转译。
        # Wine .so 是 x86_64, 不放 libs/ (系统 linker 加载失败), 放 rawfile bin/x86_64-unix
        # 由 box64 的 BOX64_LD_LIBRARY_PATH 加载; box64.so 本身是 arm64 原生 → libs/。
        log "  → Wine x86_64 .so → rawfile zip (box64 转译)"

        # ARM64 原生库 → libs/arm64-v8a/ (Box64 dlopen bridge libraries)
        # Box64 模拟 x86_64 时需要加载 ARM64 原生的 freetype/xkbcommon 等,
        # 系统 linker 搜索 libs/arm64-v8a/。来源 = 开头快照的 build_native 产物
        # (build_native.sh 在 assemble 前已装入 NATIVE_LIBS; make/build.sh 流程保证)。
        _pick_arm64_native() {
            local soname="$1" linker="${2:-}"
            if [ -f "$libs_snapshot/$soname" ]; then
                cp -L "$libs_snapshot/$soname" "$NATIVE_LIBS/$soname"
            else
                warn "ARM64 原生库 $soname 未找到 (build_native 未产出?), 跳过"
                return 0
            fi
            if [ -n "$linker" ] && [ ! -f "$NATIVE_LIBS/$linker" ]; then
                cp -L "$libs_snapshot/$soname" "$NATIVE_LIBS/$linker"  # HAP 不支持 symlink, 实体复制
            fi
        }
        _pick_arm64_native "libfreetype.so.6"   "libfreetype.so"
        _pick_arm64_native "libxkbcommon.so.0"   "libxkbcommon.so"
        _pick_arm64_native "libxkbregistry.so.0" "libxkbregistry.so"
        _pick_arm64_native "libxml2.so.2"        "libxml2.so"
        _pick_arm64_native "libwayland-client.so.0" "libwayland-client.so"
        _pick_arm64_native "libwayland-server.so.0" "libwayland-server.so"
        _pick_arm64_native "libffi.so.8"         "libffi.so"

        # box64.so → libs/arm64-v8a/ (ARM64 原生翻译器, wine_child.cpp dlopen)
        if [ -f "$BUILD_DIR/box64_build/box64.so" ]; then
            cp "$BUILD_DIR/box64_build/box64.so" "$NATIVE_LIBS/"
            log "    box64.so → libs/arm64-v8a/"
        else
            warn "box64.so 未找到！请先执行: bash scripts/build_box64.sh"
        fi

        # virgl host 栈 → libs/arm64-v8a/: 同来自快照的 build_native 产物
        # (wayland-egl/epoxy/virglrenderer/vtest_server)。缺失则 virgl_child 子进程
        # dlopen libvirglrenderer.so.1 失败, vtest 无人监听 → guest GL 初始化失败。
        # 注意: libwayland-server.so.0 已在上方白名单补回 — 它是 CMakeLists 链接
        # libentry.so 的绝对路径依赖, hap 链接期就必须存在。
        _pick_arm64_native "libwayland-egl.so.1"    "libwayland-egl.so"
        _pick_arm64_native "libepoxy.so.0"          "libepoxy.so"
        _pick_arm64_native "libvirglrenderer.so.1"  "libvirglrenderer.so"
        _pick_arm64_native "libwinehua_vtest_server.so"
        log "    virgl host 栈 (wayland-egl/epoxy/virglrenderer/vtest_server) → libs/arm64-v8a/"

        # ntdll.so → rawfile bin/ (box64 加载 wine 时按名 dlopen)
        cp "$wine_build_dir/dlls/ntdll/ntdll.so" "$wine_data/bin/"

        # x86_64-unix/ .so → rawfile (BOX64_LD_LIBRARY_PATH 搜索)
        for so in "$wine_build_dir/dlls/"*/*.so; do
            [ "$(basename "$so")" = "ntdll.so" ] && continue
            cp "$so" "$wine_data/bin/x86_64-unix/"
        done

        # 交叉编译依赖 → rawfile x86_64-unix/
        _pick_lib_pad_rf() {
            local name="$1" soname="$2" linker="${3:-}"
            local dest="$wine_data/bin/x86_64-unix"
            if [ -f "$SYSROOT_EXT_LIB/$soname" ]; then
                cp -L "$SYSROOT_EXT_LIB/$soname" "$dest/$soname"
            elif [ -f "$SYSROOT/usr/lib/$TARGET/$name" ]; then
                cp -L "$SYSROOT/usr/lib/$TARGET/$name" "$dest/$soname"
            else
                warn "$soname 未找到"
                return 0
            fi
            if [ -n "$linker" ] && [ ! -f "$dest/$linker" ]; then
                cp "$dest/$soname" "$dest/$linker"
            fi
        }
        _pick_lib_pad_rf "libfreetype.so.6.20.2"       "libfreetype.so.6"   "libfreetype.so"
        _pick_lib_pad_rf "libz.so"                      "libz.so"
        _pick_lib_pad_rf "libwayland-client.so.0.22.0"  "libwayland-client.so.0"
        _pick_lib_pad_rf "libwayland-egl.so.1.22.0"     "libwayland-egl.so.1"    "libwayland-egl.so"
        _pick_lib_pad_rf "libxkbcommon.so.0.0.0"        "libxkbcommon.so.0"
        _pick_lib_pad_rf "libxkbregistry.so.0.0.0"      "libxkbregistry.so.0"
        _pick_lib_pad_rf "libxml2.so.2.12.0"            "libxml2.so.2"
        _pick_lib_pad_rf "libffi.so.8.1.4"              "libffi.so.8"
        # GnuTLS 链 (schannel TLS 后端, x86_64 guest) → rawfile
        _pick_lib_pad_rf "libgnutls.so.30.37.1"         "libgnutls.so.30"   "libgnutls.so"
        _pick_lib_pad_rf "libnettle.so.8.11"            "libnettle.so.8"
        _pick_lib_pad_rf "libhogweed.so.6.11"           "libhogweed.so.6"
        _pick_lib_pad_rf "libgmp.so.10.4.1"             "libgmp.so.10"
        _pick_lib_pad_rf "libtasn1.so.6.6.4"            "libtasn1.so.6"
        _pick_lib_pad_rf "libunistring.so.5.2.0"        "libunistring.so.5"
        # libm.so: 补 OHOS 缺失的 frexpl/ldexpl (glib long double 数学)
        _pick_lib_pad_rf "libm.so"                      "libm.so"
        # GStreamer 链 (winegstreamer 后端: glib + gstreamer core + gst-libs)
        for so in libglib-2.0.so.0 libgobject-2.0.so.0 libgmodule-2.0.so.0 libgio-2.0.so.0 \
                  libgthread-2.0.so.0 libpcre2-8.so.0 libintl.so.8 libintl.so libm.so \
                  libgstreamer-1.0.so.0 libgstbase-1.0.so.0 libgstcontroller-1.0.so.0 \
                  libgstnet-1.0.so.0 libgstvideo-1.0.so.0 libgstaudio-1.0.so.0 \
                  libgsttag-1.0.so.0 libgstpbutils-1.0.so.0 libgstallocators-1.0.so.0 \
                  libgstapp-1.0.so.0 libgstfft-1.0.so.0 libgstriff-1.0.so.0 \
                  libgstrtp-1.0.so.0 libgstrtsp-1.0.so.0 libgstsdp-1.0.so.0 \
                  libgstcodecparsers-1.0.so.0 libgstmpegts-1.0.so.0; do
            # box64 按 SONAME 解析依赖时可能查找无版本名 (libgstvideo-1.0.so),
            # 与 gnutls 链一致补上无版本软链, 否则 winegstreamer dlopen 报
            # "Error loading shared library libgstvideo-1.0.so: No such file"
            local unversioned="${so%.so.0}"
            if [ "$unversioned" != "$so" ] && [[ "$so" == *.so.0 ]]; then
                _pick_lib_pad_rf "$so" "$so" "$unversioned.so"
            else
                _pick_lib_pad_rf "$so" "$so"
            fi
        done
        # FFmpeg 解码库 (gst-libav 依赖) → rawfile
        for so in libavcodec.so.60 libavformat.so.60 libavutil.so.58 \
                  libswscale.so.7 libswresample.so.4 libavfilter.so.9; do
            _pick_lib_pad_rf "$so" "$so"
        done
        package_gstreamer_plugins

        # libfreetype → bin/ (box64 按名 dlopen 搜索路径: .)
        cp "$wine_data/bin/x86_64-unix/libfreetype.so.6" "$wine_data/bin/"
        cp "$wine_data/bin/x86_64-unix/libfreetype.so" "$wine_data/bin/"

        # libc.so → bin/ (当前目录) + x86_64-unix/ (BOX64_LD_LIBRARY_PATH)
        cp "$SYSROOT/usr/lib/$TARGET/libc.so" "$wine_data/bin/"
        cp "$SYSROOT/usr/lib/$TARGET/libc.so" "$wine_data/bin/x86_64-unix/"

        # wine + wineserver (x86_64 ELF, 由 box64 加载)
        cp "$wine_build_dir/loader/wine" "$wine_data/bin/"
        if [ -f "$wine_server_dir/wineserver" ]; then
            cp "$wine_server_dir/wineserver" "$wine_data/bin/"
        elif [ -f "$wine_build_dir/server/wineserver" ]; then
            cp "$wine_build_dir/server/wineserver" "$wine_data/bin/"
        fi
        log "    wine + wineserver → rawfile bin/ (box64 转译)"

        # 白名单补回完成, 清掉快照 (含方案③ 残留的 wine/mesa/venus aarch64 库)
        rm -rf "$libs_snapshot"
    else
        # arm64 原生 wine: Wine aarch64 .so 直接放 libs/ (与 x86_64 同构, 非 box64 整体模拟)
        log "  → Wine aarch64 .so → libs/$NATIVE_ARCH/"

        # 定向清除方案②残留: box64.so 是方案②唯一落入 libs/ 的专属产物 (~几十MB),
        # 方案③ 不清洗整目录 (build_native 产物 + ninja 链接依赖), 只移除它
        rm -f "$NATIVE_LIBS/box64.so"

        # 所有 Wine Unix .so → libs/ (系统 linker 通过文件名搜索)
        for so in "$wine_build_dir/dlls/"*/*.so; do
            cp "$so" "$NATIVE_LIBS/"
        done
        log "    Wine .so: $(ls "$wine_build_dir/dlls/"*/*.so 2>/dev/null | wc -l) files"

        # 交叉编译依赖 → libs/
        _pick_lib_pad() {
            local name="$1" soname="$2" linker="${3:-}"
            local dest="$NATIVE_LIBS"
            if [ -f "$SYSROOT_EXT_LIB/$soname" ]; then
                cp -L "$SYSROOT_EXT_LIB/$soname" "$dest/$soname"
            elif [ -f "$SYSROOT/usr/lib/$TARGET/$name" ]; then
                cp -L "$SYSROOT/usr/lib/$TARGET/$name" "$dest/$soname"
            else
                warn "$soname 未找到"
                return 0
            fi
            if [ -n "$linker" ] && [ ! -f "$dest/$linker" ]; then
                cp "$dest/$soname" "$dest/$linker"
            fi
        }
        _pick_lib_pad "libfreetype.so.6.20.2"       "libfreetype.so.6"   "libfreetype.so"
        _pick_lib_pad "libz.so"                      "libz.so"
        _pick_lib_pad "libwayland-client.so.0.22.0"  "libwayland-client.so.0"
        _pick_lib_pad "libwayland-egl.so.1.22.0"     "libwayland-egl.so.1"
        _pick_lib_pad "libxkbcommon.so.0.0.0"        "libxkbcommon.so.0"
        _pick_lib_pad "libxkbregistry.so.0.0.0"      "libxkbregistry.so.0"
        _pick_lib_pad "libxml2.so.2.12.0"            "libxml2.so.2"
        _pick_lib_pad "libffi.so.8.1.4"              "libffi.so.8"
        # GnuTLS 链 (schannel TLS 后端)
        _pick_lib_pad "libgnutls.so.30.37.1"         "libgnutls.so.30"   "libgnutls.so"
        _pick_lib_pad "libnettle.so.8.11"            "libnettle.so.8"
        _pick_lib_pad "libhogweed.so.6.11"           "libhogweed.so.6"
        _pick_lib_pad "libgmp.so.10.4.1"             "libgmp.so.10"
        _pick_lib_pad "libtasn1.so.6.6.4"            "libtasn1.so.6"
        _pick_lib_pad "libunistring.so.5.2.0"        "libunistring.so.5"
        _pick_lib_pad "libm.so"                      "libm.so"
        # GStreamer 链 (winegstreamer 后端)
        for so in libglib-2.0.so.0 libgobject-2.0.so.0 libgmodule-2.0.so.0 libgio-2.0.so.0 \
                  libgthread-2.0.so.0 libpcre2-8.so.0 libintl.so.8 libintl.so libm.so \
                  libgstreamer-1.0.so.0 libgstbase-1.0.so.0 libgstcontroller-1.0.so.0 \
                  libgstnet-1.0.so.0 libgstvideo-1.0.so.0 libgstaudio-1.0.so.0 \
                  libgsttag-1.0.so.0 libgstpbutils-1.0.so.0 libgstallocators-1.0.so.0 \
                  libgstapp-1.0.so.0 libgstfft-1.0.so.0 libgstriff-1.0.so.0 \
                  libgstrtp-1.0.so.0 libgstrtsp-1.0.so.0 libgstsdp-1.0.so.0 \
                  libgstcodecparsers-1.0.so.0 libgstmpegts-1.0.so.0; do
            _pick_lib_pad "$so" "$so"
        done
        # Native Wine loads gst-libav from rawfile, so its FFmpeg dependency
        # chain belongs beside the native GStreamer core libraries.
        for so in libavcodec.so.60 libavformat.so.60 libavutil.so.58 \
                  libswscale.so.7 libswresample.so.4 libavfilter.so.9; do
            _pick_lib_pad "$so" "$so"
        done
        package_gstreamer_plugins
        log "    交叉编译依赖 → libs/$NATIVE_ARCH/"

        # libc.so → libs/
        cp "$SYSROOT/usr/lib/$TARGET/libc.so" "$NATIVE_LIBS/"

        # libwineserver.so (dlopen 入口)
        if [ -f "$wine_server_dir/libwineserver.so" ]; then
            cp "$wine_server_dir/libwineserver.so" "$NATIVE_LIBS/"
            log "    libwineserver.so → libs/$NATIVE_ARCH/"
        else
            warn "libwineserver.so 未找到！请先执行: bash scripts/build_wine.sh"
        fi

        # 注: 不需要 wine/wine-preloader ELF。winehua 部署以 wine 构建出的
        # ntdll.so 作为 wine 启动 (wine_child.cpp dlopen ntdll.so → __wine_main);
        # OHOS 下 spawn_process 走 ohos_broker_spawn_child (StartNativeChildProcess),
        # 不走 exec_wineloader (ntdll/unix/process.c #ifdef __OHOS__ 分支)。

        # FEX libarm64ecfex.dll → PE 目录 (arm64 原生转译 x64 应用)
        if [ -f "$BUILD_DIR/fex-ec/Bin/libarm64ecfex.dll" ]; then
            mkdir -p "$wine_data/bin/$wine_pe_dir"
            cp "$BUILD_DIR/fex-ec/Bin/libarm64ecfex.dll" "$wine_data/bin/$wine_pe_dir/"
            log "    libarm64ecfex.dll → rawfile $wine_pe_dir/"
        else
            warn "libarm64ecfex.dll 未找到！请先执行: bash scripts/build_fex.sh"
        fi
        # FEX libwow64fex.dll → PE 目录 (arm64 原生转译 32 位 x86 应用)
        if [ -f "$BUILD_DIR/fex-pe/Bin/libwow64fex.dll" ]; then
            cp "$BUILD_DIR/fex-pe/Bin/libwow64fex.dll" "$wine_data/bin/$wine_pe_dir/"
            log "    libwow64fex.dll → rawfile $wine_pe_dir/"
        else
            warn "libwow64fex.dll 未找到！请先执行: bash scripts/build_fex.sh"
        fi
        # Box64 wowbox64.dll → PE 目录 (arm64 原生转译 32 位 x86 应用, HODLL 可选引擎)
        local wowbox64_dll="$BUILD_DIR/box64-pe/wowbox64-prefix/src/wowbox64-build/wowbox64.dll"
        if [ -f "$wowbox64_dll" ]; then
            cp "$wowbox64_dll" "$wine_data/bin/$wine_pe_dir/"
            log "    wowbox64.dll → rawfile $wine_pe_dir/"
        else
            warn "wowbox64.dll 未找到！请先执行: bash scripts/build_box64_wow64.sh"
        fi
        # libc++/libunwind runtime (防御): arm64x DLL 现以静态 .a
        # --start-group 链接, 正常不再依赖运行时; 若未来换工具链/改造
        # 恢复动态依赖, system32 (bin/aarch64-windows/) 缺失即 c0000135
        # (实测 2026-09-05 "Library libc++.dll ... not found" 即此坑)。
        for rt in libc++.dll libunwind.dll; do
            rt_src="$LLVM_MINGW/aarch64-w64-mingw32/bin/$rt"
            if [ -f "$rt_src" ]; then
                cp "$rt_src" "$wine_data/bin/$wine_pe_dir/"
                log "    $rt → rawfile $wine_pe_dir/ (arm64x runtime)"
            else
                warn "arm64x runtime $rt 未找到 ($rt_src)"
            fi
        done
    fi

    # -- 2. PE DLL + 数据文件 → rawfile (两种架构共用) --
    # x86_64-windows/ — 复制所有运行时 PE 文件
    # .cpl (含 appwiz.cpl) 是否打包由 BUILD_WINE_MONO 决定:
    #   =1 (显式实验): 打包 cpl + build_deps 下载 mono msi, 保留 .NET/控制面板.
    #   =0 (默认直接游戏档): 不打包 cpl. 若 mono 缺失, wineboot 初始化时 mscoree.dll
    #   会 CreateProcess "control.exe appwiz.cpl install_mono" 弹 DialogBoxW 模态框,
    #   OHOS 无头环境无人响应 → wineboot 永久阻塞 (mscoree WaitForSingleObject 无限
    #   等待). 去掉 appwiz.cpl 后 control.exe 加载 cpl 失败立即退出, mscoree 走
    #   "无 .NET 运行时"路径不卡死.
    # msstyles: 主题文件, 名义上是 DLL 但扩展名是 .msstyles (data-only PE,
    # 无代码只有 INI 文本 + 位图资源)。缺它时 uxtheme 加载
    # %10%\resources\themes\aero\aero.msstyles 落空, 所有 Win32 控件退化成
    # 经典 (无主题) 绘制 —— 窗口标题栏/边框/按钮变成 Win95 观感。
    # x86_64 与 i386 两份都要: WoW64 下 32 位进程加载 32 位主题。
    local pe_exts="dll drv exe sys acm ax ocx tlb msstyles"
    if [ "${BUILD_WINE_MONO:-0}" = "1" ]; then
        pe_exts="$pe_exts cpl"
    fi
    for ext in $pe_exts; do
        for pe_src in $pe_src_dirs; do
            for f in "$wine_build_dir/dlls/"*/$pe_src/*.$ext; do
                [ -f "$f" ] && cp "$f" "$wine_data/bin/$wine_pe_dir/"
            done
        done
    done
    if [ "$WINE_ARCH" = "aarch64" ]; then
        # Native ARM64 / ARM64X PE (DXVK, VKD3D, clang++ smokes) import these.
        copy_arm64x_cxx_runtime "$wine_data/bin/$wine_pe_dir"
    fi
    log "  $wine_pe_dir → $(ls "$wine_data/bin/$wine_pe_dir" | wc -l) files"

    # strip PE 调试符号 (DWARF .debug_*, 缩减 ~50%)
    log "  stripping debug symbols..."
    local pe_strip="x86_64-w64-mingw32-strip"
    [ "$WINE_ARCH" = "aarch64" ] && pe_strip="$LLVM_MINGW/bin/aarch64-w64-mingw32-strip"
    if command -v "$pe_strip" &>/dev/null || [ -x "$pe_strip" ]; then
        for f in "$wine_data/bin/$wine_pe_dir/"*.dll "$wine_data/bin/$wine_pe_dir/"*.drv "$wine_data/bin/$wine_pe_dir/"*.exe "$wine_data/bin/$wine_pe_dir/"*.sys; do
            [ -f "$f" ] && "$pe_strip" "$f" 2>/dev/null
        done
        log "  $wine_pe_dir PE stripped"
    else
        warn "  $pe_strip not found, skipping strip"
    fi
    # i386-windows/ (32-bit PE DLL for WoW64)
    # 主构建 --enable-archs=i386,x86_64 已产出全部 32-bit PE, 直接取自 wine-ohos,
    # 无需独立的 i686-mingw32 构建.
    # 注意: wineboot/rpcss/services/conhost 等服务程序只有 x86_64 版,
    # WoW64 下它们由 Wine 以 64 位进程拉起, 属上游 WoW64 的正常行为.
    mkdir -p "$wine_data/bin/i386-windows"
    # 与 x86_64 一致: cpl 仅当 BUILD_WINE_MONO=1 时打包 (见上方 pe_exts 注释)
    for ext in $pe_exts; do
        for f in "$wine_build_dir/dlls/"*/i386-windows/*.$ext; do
            [ -f "$f" ] && cp "$f" "$wine_data/bin/i386-windows/"
        done
    done
    log "  i386-windows → $(ls "$wine_data/bin/i386-windows" | wc -l) files (ALL)"

    # 32-bit exe stubs, 放在 bin/i386-windows/.
    # Wine 通过 WINEARCH 或 exe header 判断 32/64, 自动加载对应 DLL.
    # 排除测试程序: wine 的 bin/ 目录不放 smoke 程序, 载荷走 wine-data/smoke
    # 独立树 (见 assemble 尾部, 设备端经 SmokeHook.seed 播种到 C:\smoke);
    # winehua_keep.exe 是产品必需 (wine_launch.cpp 拷进 system32).
    for exe in "$wine_build_dir/programs/"*/i386-windows/*.exe; do
        case "$(basename "$exe")" in
            winehua_*_smoke.exe|winehua_dinput_probe.exe) continue ;;
        esac
        [ -f "$exe" ] && cp "$exe" "$wine_data/bin/i386-windows/"
    done
    log "  i386 exe stubs → $(ls "$wine_data/bin/i386-windows"/*.exe 2>/dev/null | wc -l) files"

    # 32-bit PE strip (必须在 copy 之后)
    if command -v i686-w64-mingw32-strip &>/dev/null; then
        for f in "$wine_data/bin/i386-windows/"*.dll "$wine_data/bin/i386-windows/"*.drv "$wine_data/bin/i386-windows/"*.exe "$wine_data/bin/i386-windows/"*.sys; do
            [ -f "$f" ] && i686-w64-mingw32-strip "$f" 2>/dev/null
        done
        log "  32-bit PE stripped"
    else
        warn "  i686-w64-mingw32-strip not found, skipping strip"
    fi

    # *.exe stubs → rawfile (bin/ 不放 smoke 程序, 载荷走 wine-data/smoke 独立树)
    # 注意: arm64 下 pe_src_dirs 含 arm64ec-windows, 该架构只产 .o 无 .exe
    # (system DLL 为 ARM64EC+ARM64 混合, 统一在 aarch64-windows) → 需 -f 保护
    for pe_src in $pe_src_dirs; do
        for exe in "$wine_build_dir/programs/"*/$pe_src/*.exe; do
            [ -f "$exe" ] || continue
            case "$(basename "$exe")" in
                winehua_*_smoke.exe|winehua_dinput_probe.exe) continue ;;
            esac
            cp -f "$exe" "$wine_data/bin/"
        done
    done

    # venus shader assets: 随 guest_vulkan bundle 打包 (aarch64/x86_64 源驱动; 无 bundle 则跳过)
    # 载荷本体由 automation/smoke.py 产出 (见 assemble 尾部), 这里只补 Vulkan 探针
    # 需要的 SPIR-V 资产 —— 它们来自 guest Vulkan bundle, 不在 smoke.py 的输入里。
    local guest_shader_root="$BUILD_DIR/guest_vulkan/$guest_arch/share/winehua"
    if [ -d "$guest_shader_root" ]; then
        local smoke_shader
        mkdir -p "$wine_data/smoke/assets"
        for smoke_shader in venus_storage_write venus_storage_read venus_image_fetch venus_combined_sample venus_separated_sample; do
            [ -f "$guest_shader_root/$smoke_shader.spv" ] || err "Wine Vulkan sampled-image shader missing: $guest_shader_root/$smoke_shader.spv"
            cp "$guest_shader_root/$smoke_shader.spv" "$wine_data/smoke/assets/$smoke_shader.spv"
        done
    fi
    local dxvk_root="$DXVK_BUILD_ROOT"
    [ -f "$dxvk_root/x86/bin/d3d11.dll" ] || err "DXVK Legacy x86 d3d11.dll missing: $dxvk_root/x86/bin/d3d11.dll"
    [ -f "$dxvk_root/x86/bin/dxgi.dll" ] || err "DXVK Legacy x86 dxgi.dll missing: $dxvk_root/x86/bin/dxgi.dll"
    mkdir -p "$wine_data/dxvk/legacy/x86"
    cp "$dxvk_root/x86/bin/d3d11.dll" "$wine_data/dxvk/legacy/x86/d3d11.dll"
    cp "$dxvk_root/x86/bin/dxgi.dll" "$wine_data/dxvk/legacy/x86/dxgi.dll"
    # 方案③ (aarch64 wine + FEX): x64 guest 的 d3d11/dxgi 由 ARM64X (arm64x/)
    # 提供 (wine_env.cpp 把 x64 overlay 指向 arm64x), x64/ 目录不被引用, 跳过
    # 打包; 方案①/② (x86_64 wine native / box64) 仍需要 x64/。
    if [ "$WINE_ARCH" != "aarch64" ]; then
        [ -f "$dxvk_root/x64/bin/d3d11.dll" ] || err "DXVK Legacy x64 d3d11.dll missing: $dxvk_root/x64/bin/d3d11.dll"
        [ -f "$dxvk_root/x64/bin/dxgi.dll" ] || err "DXVK Legacy x64 dxgi.dll missing: $dxvk_root/x64/bin/dxgi.dll"
        mkdir -p "$wine_data/dxvk/legacy/x64"
        cp "$dxvk_root/x64/bin/d3d11.dll" "$wine_data/dxvk/legacy/x64/d3d11.dll"
        cp "$dxvk_root/x64/bin/dxgi.dll" "$wine_data/dxvk/legacy/x64/dxgi.dll"
        # D3D10 链必须整套装齐 (实测 WarThunderLauncher 启动器):
        # wine builtin 的 d3d10core 是调 dxgi 的私有导出 DXGID3D10CreateDevice 建
        # 设备的, 而 DXVK 的 dxgi 没有该导出 —— 缺 native d3d10 时程序落到 builtin
        # 链, 又被 WINEDLLOVERRIDES=dxgi=n (禁止回退 builtin) 卡住, 直接 abort。
        # DXVK 1.10.3 自带 d3d10/d3d10_1/d3d10core (2.x 起移除, 故只在 legacy 装),
        # 三者与 dxgi 同一次构建产出, 整套走 native 才自洽。
        # (方案③ 的 x64 走 ARM64X 双图, 而 arm64x 只产 d3d11/dxgi 无 d3d10,
        # 故 x64 d3d10 只在非 aarch64 装。)
        local dxvk_d3d10
        for dxvk_d3d10 in d3d10.dll d3d10_1.dll d3d10core.dll; do
            [ -f "$dxvk_root/x64/bin/$dxvk_d3d10" ] || err "DXVK Legacy x64 $dxvk_d3d10 missing"
            cp "$dxvk_root/x64/bin/$dxvk_d3d10" "$wine_data/dxvk/legacy/x64/$dxvk_d3d10"
        done
    fi
    # D3D10 x86: 32 位走 FEX/wow64, 两方案都用 x86 PE
    local dxvk_d3d10_x86
    for dxvk_d3d10_x86 in d3d10.dll d3d10_1.dll d3d10core.dll; do
        [ -f "$dxvk_root/x86/bin/$dxvk_d3d10_x86" ] || err "DXVK Legacy x86 $dxvk_d3d10_x86 missing"
        cp "$dxvk_root/x86/bin/$dxvk_d3d10_x86" "$wine_data/dxvk/legacy/x86/$dxvk_d3d10_x86"
    done
    # 方案③ ARM64X 双图 DLL: wine_env.cpp 把 x64 overlay 指向 arm64x
    if [ "$WINE_ARCH" = "aarch64" ]; then
        local dxvk_arm64x="$DXVK_BUILD_ROOT/arm64x"
        [ -f "$dxvk_arm64x/bin/d3d11.dll" ] || \
            err "DXVK Legacy ARM64X d3d11.dll missing: $dxvk_arm64x/bin/d3d11.dll"
        [ -f "$dxvk_arm64x/bin/dxgi.dll" ] || \
            err "DXVK Legacy ARM64X dxgi.dll missing: $dxvk_arm64x/bin/dxgi.dll"
        mkdir -p "$wine_data/dxvk/legacy/arm64x"
        cp "$dxvk_arm64x/bin/d3d11.dll" "$wine_data/dxvk/legacy/arm64x/d3d11.dll"
        cp "$dxvk_arm64x/bin/dxgi.dll" "$wine_data/dxvk/legacy/arm64x/dxgi.dll"
        copy_arm64x_cxx_runtime "$wine_data/dxvk/legacy/arm64x"
        # ntdll overlay 现已搜 arm64x/; 仍把双图镜像进 x64/, 兼容旧 ntdll
        # 以及只认 x64 目录名的钩子。缺 x64 时 64 位 Unity LoadLibrary(d3d11)
        # 会落到 Wine builtin 再被 d3d11=n 拒绝 (0x80029C4A)。
        mkdir -p "$wine_data/dxvk/legacy/x64"
        cp "$dxvk_arm64x/bin/d3d11.dll" "$wine_data/dxvk/legacy/x64/d3d11.dll"
        cp "$dxvk_arm64x/bin/dxgi.dll" "$wine_data/dxvk/legacy/x64/dxgi.dll"
        copy_arm64x_cxx_runtime "$wine_data/dxvk/legacy/x64"
    fi
    local dxvk_modern_root="$DXVK_MODERN_BUILD_ROOT"
    [ -f "$dxvk_modern_root/x86/bin/d3d11.dll" ] || err "DXVK Modern x86 d3d11.dll missing: $dxvk_modern_root/x86/bin/d3d11.dll"
    [ -f "$dxvk_modern_root/x86/bin/dxgi.dll" ] || err "DXVK Modern x86 dxgi.dll missing: $dxvk_modern_root/x86/bin/dxgi.dll"
    mkdir -p "$wine_data/dxvk/modern-2.6/x86"
    cp "$dxvk_modern_root/x86/bin/d3d11.dll" "$wine_data/dxvk/modern-2.6/x86/d3d11.dll"
    cp "$dxvk_modern_root/x86/bin/dxgi.dll" "$wine_data/dxvk/modern-2.6/x86/dxgi.dll"
    # 方案③ (aarch64 wine + FEX): 同上, x64/ 由 arm64x/ 取代, 跳过打包;
    # 方案①/② 需要 x64/。
    if [ "$WINE_ARCH" != "aarch64" ]; then
        [ -f "$dxvk_modern_root/x64/bin/d3d11.dll" ] || err "DXVK Modern x64 d3d11.dll missing: $dxvk_modern_root/x64/bin/d3d11.dll"
        [ -f "$dxvk_modern_root/x64/bin/dxgi.dll" ] || err "DXVK Modern x64 dxgi.dll missing: $dxvk_modern_root/x64/bin/dxgi.dll"
        mkdir -p "$wine_data/dxvk/modern-2.6/x64"
        cp "$dxvk_modern_root/x64/bin/d3d11.dll" "$wine_data/dxvk/modern-2.6/x64/d3d11.dll"
        cp "$dxvk_modern_root/x64/bin/dxgi.dll" "$wine_data/dxvk/modern-2.6/x64/dxgi.dll"
    fi
    # 方案③ (aarch64 wine + FEX): ARM64X 双图 DLL, wine_env.cpp 把 x64 overlay
    # 指向 arm64x, 使 FEX 以 native view 执行而非逐条 x64 转译。
    if [ "$WINE_ARCH" = "aarch64" ]; then
        local dxvk_arm64x="$DXVK_MODERN_BUILD_ROOT/arm64x"
        [ -f "$dxvk_arm64x/bin/d3d11.dll" ] || \
            err "DXVK Modern ARM64X d3d11.dll missing: $dxvk_arm64x/bin/d3d11.dll"
        [ -f "$dxvk_arm64x/bin/dxgi.dll" ] || \
            err "DXVK Modern ARM64X dxgi.dll missing: $dxvk_arm64x/bin/dxgi.dll"
        mkdir -p "$wine_data/dxvk/modern-2.6/arm64x"
        cp "$dxvk_arm64x/bin/d3d11.dll" "$wine_data/dxvk/modern-2.6/arm64x/d3d11.dll"
        cp "$dxvk_arm64x/bin/dxgi.dll" "$wine_data/dxvk/modern-2.6/arm64x/dxgi.dll"
        copy_arm64x_cxx_runtime "$wine_data/dxvk/modern-2.6/arm64x"
        # 同 legacy: 镜像进 x64/, 兼容旧 ntdll overlay
        mkdir -p "$wine_data/dxvk/modern-2.6/x64"
        cp "$dxvk_arm64x/bin/d3d11.dll" "$wine_data/dxvk/modern-2.6/x64/d3d11.dll"
        cp "$dxvk_arm64x/bin/dxgi.dll" "$wine_data/dxvk/modern-2.6/x64/dxgi.dll"
        copy_arm64x_cxx_runtime "$wine_data/dxvk/modern-2.6/x64"
    fi
    local vkd3d_root="$VKD3D_PROTON_BUILD_ROOT/limited-500k"
    # 方案③: d3d12 固定用 x86_64 单图 (FEX 转译执行)。
    # vkd3d arm64x 弃用结论 (2026-09-05 最终决策, 不再重试):
    #   * 手搓链路用 --target=aarch64-w64-mingw32 -marm64x: 该 clang 对
    #     -marm64x 静默忽略, 产物 = ARM64X 壳但仅 AA64 单子图 (假双图),
    #     x64 guest 加载后执行形态错误 → buffer 回读 (vkCmdCopyBuffer 链)
    #     全 0, fence 却正常信号;
    #   * 真 ARM64EC (-target=arm64ec-w64-mingw32, meson cross 已验证可产出
    #     0xA641) 在 libarm64ecfex 桥上 D3D12CreateDevice 阶段崩溃。
    #   * 对照: dxvk 的 arm64x 为真双图 (0xA64E+0xA641), 与以上无关, 保留。
    # x64 单图 = 09-01 验证过的 "d12 能跑" 基线同款机制, 新特性 HAP 上实测 PASS。
    # (方案①/②/③ 统一: d3d12 均为 x64 单图)
    [ -f "$vkd3d_root/x64/d3d12.dll" ] || err "VKD3D-Proton x64 d3d12.dll missing: $vkd3d_root/x64/d3d12.dll"
    [ -f "$vkd3d_root/manifest.json" ] || err "VKD3D-Proton manifest missing: $vkd3d_root/manifest.json"
    mkdir -p "$wine_data/vkd3d/limited-500k/x64"
    cp "$vkd3d_root/x64/d3d12.dll" "$wine_data/vkd3d/limited-500k/x64/d3d12.dll"
    cp "$vkd3d_root/manifest.json" "$wine_data/vkd3d/manifest.json"
    # DXVK/VKD3D 二进制是 runtime overlay: 由 SpawnWineProgram 按选定后端经
    # WINEDLLPATH 暴露, 不作为 C:\smoke 载荷的一部分。
    local vkd3d64_d3d12_sha
    vkd3d64_d3d12_sha="$(sha256sum "$wine_data/vkd3d/limited-500k/x64/d3d12.dll" | awk '{print $1}')"
    local dxvk_commit dxvk_modern_commit mesa_commit virglrenderer_commit
    local guest_venus_icd_sha host_virglrenderer_sha venus_runtime_id
    dxvk_commit="$(git -c safe.directory="$DXVK_SRC" -C "$DXVK_SRC" rev-parse HEAD 2>/dev/null || echo unknown)"
    dxvk_modern_commit="$(git -c safe.directory="$DXVK_MODERN_SRC" -C "$DXVK_MODERN_SRC" rev-parse HEAD 2>/dev/null || echo unknown)"
    mesa_commit="$(git -c safe.directory="$ROOT/thirdparty/mesa" -C "$ROOT/thirdparty/mesa" rev-parse HEAD 2>/dev/null || echo unknown)"
    virglrenderer_commit="$(git -c safe.directory="$ROOT/thirdparty/virglrenderer" -C "$ROOT/thirdparty/virglrenderer" rev-parse HEAD 2>/dev/null || echo unknown)"
    guest_venus_icd_sha="$(sha256sum "$BUILD_DIR/guest_vulkan/$guest_arch/lib/libvulkan_virtio.so" | awk '{print $1}')"
    host_virglrenderer_sha="$(sha256sum "$ROOT/entry/libs/$NATIVE_ARCH/libvirglrenderer.so.1" | awk '{print $1}')"
    venus_runtime_id="venus-${guest_venus_icd_sha:0:12}-${host_virglrenderer_sha:0:12}"
    local dxvk64_d3d11_sha dxvk64_dxgi_sha dxvk32_d3d11_sha dxvk32_dxgi_sha
    local dxvkmodern64_d3d11_sha dxvkmodern64_dxgi_sha dxvkmodern32_d3d11_sha dxvkmodern32_dxgi_sha
    # 方案③ (aarch64) 不打包 x64/ → sha 记空 (同 vkd3d64 的守卫风格)
    if [ -f "$wine_data/dxvk/legacy/x64/d3d11.dll" ]; then
        dxvk64_d3d11_sha="$(sha256sum "$wine_data/dxvk/legacy/x64/d3d11.dll" | awk '{print $1}')"
        dxvk64_dxgi_sha="$(sha256sum "$wine_data/dxvk/legacy/x64/dxgi.dll" | awk '{print $1}')"
    else
        dxvk64_d3d11_sha="" dxvk64_dxgi_sha=""
    fi
    dxvk32_d3d11_sha="$(sha256sum "$wine_data/dxvk/legacy/x86/d3d11.dll" | awk '{print $1}')"
    dxvk32_dxgi_sha="$(sha256sum "$wine_data/dxvk/legacy/x86/dxgi.dll" | awk '{print $1}')"
    if [ -f "$wine_data/dxvk/modern-2.6/x64/d3d11.dll" ]; then
        dxvkmodern64_d3d11_sha="$(sha256sum "$wine_data/dxvk/modern-2.6/x64/d3d11.dll" | awk '{print $1}')"
        dxvkmodern64_dxgi_sha="$(sha256sum "$wine_data/dxvk/modern-2.6/x64/dxgi.dll" | awk '{print $1}')"
    else
        dxvkmodern64_d3d11_sha="" dxvkmodern64_dxgi_sha=""
    fi
    dxvkmodern32_d3d11_sha="$(sha256sum "$wine_data/dxvk/modern-2.6/x86/d3d11.dll" | awk '{print $1}')"
    dxvkmodern32_dxgi_sha="$(sha256sum "$wine_data/dxvk/modern-2.6/x86/dxgi.dll" | awk '{print $1}')"
    cat > "$wine_data/dxvk/manifest.json" <<EOF
{
  "schemaVersion": 2,
  "backend": "dxvk",
  "defaultProfile": "legacy",
  "runtimeRoot": "dxvk",
  "venusRuntime": {
    "id": "$venus_runtime_id",
    "guestMesaCommit": "$mesa_commit",
    "guestIcdSha256": "$guest_venus_icd_sha",
    "hostVirglrendererCommit": "$virglrenderer_commit",
    "hostVirglrendererSha256": "$host_virglrenderer_sha",
    "transportCapabilities": {
      "remoteMemoryShadow": true,
      "multiRing": false,
      "fenceFeedback": false,
      "queryFeedback": false,
      "semaphoreFeedback": true,
      "modernRequiresSynchronousTimelineQueries": true
    }
  },
  "runtimes": {
    "legacy": {
      "version": "1.10.3",
      "commit": "$dxvk_commit",
      "state": "stable",
      "requiredCapabilities": {"vulkanApi": "1.1", "bcFormats": false, "descriptorIndexing": false},
      "x64": {"d3d11.dll": "$dxvk64_d3d11_sha", "dxgi.dll": "$dxvk64_dxgi_sha"},
      "x86": {"d3d11.dll": "$dxvk32_d3d11_sha", "dxgi.dll": "$dxvk32_dxgi_sha"}
    },
    "modern-2.6": {
      "version": "2.6.2",
      "commit": "$dxvk_modern_commit",
      "state": "adapted-game-validated-capability-gated",
      "requiredCapabilities": {"vulkanApi": "1.3", "robustness2": true, "dynamicRendering": true, "maintenance4": true},
      "x64": {"d3d11.dll": "$dxvkmodern64_d3d11_sha", "dxgi.dll": "$dxvkmodern64_dxgi_sha"},
      "x86": {"d3d11.dll": "$dxvkmodern32_d3d11_sha", "dxgi.dll": "$dxvkmodern32_dxgi_sha"}
    }
  }
}
EOF
    # Suite 编排定义由 automation/smoke.py 产出 (smoke/tests + smoke/suites),
    # 见 assemble 尾部的载荷拷贝。
    log "  VKD3D-Proton 2.6 limited-500K (default mixed D3D12 profile) → vkd3d/limited-500k/x64 (sha256=$vkd3d64_d3d12_sha)"

    # fonts
    cp "$WINE_SRC/fonts/"*.ttf "$wine_data/share/wine/fonts/"
    # NLS
    cp "$wine_build_dir/nls/"*.nls "$wine_data/share/wine/nls/"
    # winmd
    cp "$wine_build_dir/include/"*.winmd "$wine_data/share/wine/winmd/"
    # Wine Mono (.NET runtime) is retained for explicit experiments only.
    # The direct-game default omits it and appwiz.cpl because Wine's first-run
    # installer is interactive and blocks wineboot without a device-side flow.
    local wine_mono_msi="$BUILD_DIR/wine-mono/wine-mono-11.1.0-x86.msi"
    if [ "${BUILD_WINE_MONO:-0}" = "1" ]; then
        [ -s "$wine_mono_msi" ] || err "Wine Mono MSI missing: $wine_mono_msi"
        cp "$wine_mono_msi" "$wine_data/share/wine/mono/"
        log "    wine-mono.msi → rawfile share/wine/mono/"
    else
        log "    Wine Mono: SKIP (BUILD_WINE_MONO=0)"
    fi
    # wine.inf (含 OHOS font substitutes)
    cp "$wine_build_dir/loader/wine.inf" "$wine_data/share/wine/"
    sed_i '/^\[MCI\]$/i\
;; OHOS font substitutes\
HKLM,%FontSubStr%,"System",,"HarmonyOS Sans SC"\
HKLM,%FontSubStr%,"MS Sans Serif",,"HarmonyOS Sans SC"\
HKLM,%FontSubStr%,"MS Shell Dlg",,"HarmonyOS Sans SC"\
HKLM,%FontSubStr%,"MS Shell Dlg 2",,"HarmonyOS Sans SC"\
HKLM,%FontSubStr%,"Arial",,"HarmonyOS Sans SC"\
HKLM,%FontSubStr%,"Arial Black",,"HarmonyOS Sans SC"\
HKLM,%FontSubStr%,"Calibri",,"HarmonyOS Sans SC"\
HKLM,%FontSubStr%,"Cambria",,"Noto Serif"\
HKLM,%FontSubStr%,"Candara",,"HarmonyOS Sans SC"\
HKLM,%FontSubStr%,"Comic Sans MS",,"HarmonyOS Sans SC"\
HKLM,%FontSubStr%,"Constantia",,"Noto Serif"\
HKLM,%FontSubStr%,"Corbel",,"HarmonyOS Sans SC"\
HKLM,%FontSubStr%,"Impact",,"HarmonyOS Sans SC"\
HKLM,%FontSubStr%,"Palatino Linotype",,"Noto Serif"\
HKLM,%FontSubStr%,"Segoe UI",,"HarmonyOS Sans SC"\
HKLM,%FontSubStr%,"Tahoma",,"HarmonyOS Sans SC"\
HKLM,%FontSubStr%,"Trebuchet MS",,"HarmonyOS Sans SC"\
HKLM,%FontSubStr%,"Verdana",,"HarmonyOS Sans SC"\
;; Latin: 衬线 (serif)\
HKLM,%FontSubStr%,"Georgia",,"Noto Serif"\
HKLM,%FontSubStr%,"Times New Roman",,"Noto Serif"\
;; CJK: 简体中文\
HKLM,%FontSubStr%,"Microsoft JhengHei",,"HarmonyOS Sans TC"\
HKLM,%FontSubStr%,"Microsoft JhengHei UI",,"HarmonyOS Sans TC"\
HKLM,%FontSubStr%,"Microsoft YaHei",,"HarmonyOS Sans SC"\
HKLM,%FontSubStr%,"Microsoft YaHei UI",,"HarmonyOS Sans SC"\
;; CJK: 宋体/楷体 (serif)\
HKLM,%FontSubStr%,"SimSun",,"Noto Serif CJK SC"\
HKLM,%FontSubStr%,"NSimSun",,"Noto Serif CJK SC"\
HKLM,%FontSubStr%,"SimHei",,"HarmonyOS Sans SC"\
HKLM,%FontSubStr%,"FangSong",,"Noto Serif CJK SC"\
HKLM,%FontSubStr%,"KaiTi",,"Noto Serif CJK SC"\
HKLM,%FontSubStr%,"YouYuan",,"HarmonyOS Sans SC"\
HKLM,%FontSubStr%,"LiSu",,"HarmonyOS Sans SC"\
HKLM,%FontSubStr%,"DengXian",,"HarmonyOS Sans SC"\
HKLM,%FontSubStr%,"STSong",,"Noto Serif CJK SC"\
HKLM,%FontSubStr%,"STKaiti",,"Noto Serif CJK SC"\
HKLM,%FontSubStr%,"STFangsong",,"Noto Serif CJK SC"\
HKLM,%FontSubStr%,"STHeiti",,"HarmonyOS Sans SC"\
HKLM,%FontSubStr%,"STXihei",,"HarmonyOS Sans SC"\
HKLM,%FontSubStr%,"STLiti",,"HarmonyOS Sans SC"\
HKLM,%FontSubStr%,"STXingkai",,"HarmonyOS Sans SC"\
HKLM,%FontSubStr%,"STXinwei",,"HarmonyOS Sans SC"\
HKLM,%FontSubStr%,"STHupo",,"HarmonyOS Sans SC"\
HKLM,%FontSubStr%,"STCaiyun",,"HarmonyOS Sans SC"\
HKLM,%FontSubStr%,"STZhongSong",,"Noto Serif CJK SC"\
HKLM,%FontSubStr%,"STBaoli",,"Noto Serif CJK SC"\
HKLM,%FontSubStr%,"FZShuTi",,"HarmonyOS Sans SC"\
HKLM,%FontSubStr%,"FZYaoti",,"HarmonyOS Sans SC"\
;; CJK: 繁体中文\
HKLM,%FontSubStr%,"MingLiU",,"HarmonyOS Sans TC"\
HKLM,%FontSubStr%,"PMingLiU",,"HarmonyOS Sans TC"\
HKLM,%FontSubStr%,"DFKai-SB",,"Noto Serif CJK TC"\
HKLM,%FontSubStr%,"Consolas",,"Noto Sans Mono"\
HKLM,%FontSubStr%,"Courier",,"Noto Sans Mono"\
HKLM,%FontSubStr%,"Courier New",,"Noto Sans Mono"\
HKLM,%FontSubStr%,"Fixedsys",,"Noto Sans Mono"\
HKLM,%FontSubStr%,"Lucida Console",,"Noto Sans Mono"' "$wine_data/share/wine/wine.inf"
    # XKB
    if [ -d "$SYSROOT_EXT_SHARE/X11/xkb" ]; then
        cp -r "$SYSROOT_EXT_SHARE/X11/xkb" "$wine_data/share/X11/"
    fi

    verify_runtime_components

    # guest GPU 库 (Mesa/VirGL, 供 GraphicsBroker 注入到 Wine LD_LIBRARY_PATH)
    if [ -d "$BUILD_DIR/guest_gfx/$guest_arch/lib" ]; then
        mkdir -p "$wine_data/bin/guest_gfx"
        cp -a "$BUILD_DIR/guest_gfx/$guest_arch/"* "$wine_data/bin/guest_gfx/"
        log "  guest_gfx ($guest_arch): $(ls "$wine_data/bin/guest_gfx/lib"/*.so* 2>/dev/null | wc -l) .so files"
    else
        if [ "${BUILD_GUEST_GFX:-0}" = "1" ]; then
            err "BUILD_GUEST_GFX=1 but build/guest_gfx/$guest_arch/lib is missing"
        fi
        log "  guest_gfx: SKIP (build/guest_gfx/$guest_arch/lib not found)"
    fi

    # guest Mesa 库必须可被系统 dlopen (el1 bundle libs); el2 数据区 dlopen 被拒 (ENOENT).
    # libwayland-*/libffi/libz 复用 el1 已有 host 版本; libc++_shared 设备 el1/系统都没有,
    # 但 guest libgallium (C++) 动态依赖它, 必须随 guest 一起复制到 el1, 否则 guest libEGL
    # dlopen 失败 → Wine 内 OpenGL 初始化失败 (ChoosePixelFormat 失败).
    # 方案① (x86_64 模拟器) 与方案③ (arm64 原生) 都执行: 方案① 是 master 语义 (031e930:
    # 模拟器系统 linker 拒绝 el2 dlopen, guest Mesa 必须放 el1 libs, GALLIUM_DRIVER=softpipe).
    # 注意 guest_gfx pattern 不含 libvulkan.so, 不污染 CMake 链接 libentry.so 的 -lvulkan;
    # 缺 vkCreateSurfaceOHOS 只与 guest_vulkan 的 libvulkan.so 相关 (下方守卫已限定 arm64).
    # 方案② (box64+wine): guest 是 x86_64, 由 box64 从 rawfile 加载, 不做 el1 dlopen 复制
    if [ "$is_box64_scheme" = "0" ] && [ -d "$BUILD_DIR/guest_gfx/$guest_arch/lib" ]; then
        log "  guest_gfx -> entry/libs/$NATIVE_ARCH (el1 dlopen)"
        mkdir -p "$ROOT/entry/libs/$NATIVE_ARCH"
        for pattern in libEGL.so libGLESv2.so libGLESv1_CM.so libgallium-*.so libdrm.so libc++_shared.so; do
            for f in "$BUILD_DIR/guest_gfx/$guest_arch/lib"/$pattern*; do
                [ -f "$f" ] && cp -a "$f" "$ROOT/entry/libs/$NATIVE_ARCH/"
            done
        done
        for f in "$BUILD_DIR/guest_gfx/$guest_arch/lib"/dri/*.so; do
            [ -f "$f" ] && cp -a "$f" "$ROOT/entry/libs/$NATIVE_ARCH/"
        done
        log "  guest_gfx el1: $(ls "$ROOT/entry/libs/$NATIVE_ARCH"/libEGL.so* "$ROOT/entry/libs/$NATIVE_ARCH"/libgallium-*.so 2>/dev/null | wc -l) libs + $(ls "$ROOT/entry/libs/$NATIVE_ARCH"/*_dri.so 2>/dev/null | wc -l) dri drivers"
    fi

    # Guest Linux Vulkan runtime is intentionally outside C:\smoke: Loader + Venus
    # ICD + offscreen smoke, 按 GUEST_ARCH (aarch64/x86_64) 打包.
    if [ -f "$BUILD_DIR/guest_vulkan/$guest_arch/manifest.json" ]; then
        mkdir -p "$wine_data/bin/guest_vulkan"
        cp -a "$BUILD_DIR/guest_vulkan/$guest_arch/"* "$wine_data/bin/guest_vulkan/"
        log "  guest_vulkan ($guest_arch): Loader + Venus ICD"
    elif [ "${BUILD_GUEST_VULKAN:-0}" = "1" ]; then
        err "BUILD_GUEST_VULKAN=1 but build/guest_vulkan/$guest_arch/manifest.json is missing"
    else
        log "  guest_vulkan: SKIP"
    fi

    # guest_vulkan 关键 dlopen 库 → el1 bundle (loader + venus ICD + smoke 程序).
    # 仅方案③ (arm64 原生) 需要: el2 data 区 dlopen 被拒 (ENOENT)。方案① (x86_64) 的
    # guest_vulkan 在 el2 可 dlopen (master 语义), 且 el1 拷贝的 guest libvulkan.so 会
    # 污染 CMake 链接 (-L 优先命中, 缺 vkCreateSurfaceOHOS) → x86_64 不复制。
    # 方案② (box64+wine): guest vulkan 是 x86_64, 由 box64 从 rawfile 加载, 不做 el1 dlopen 复制
    if [ "$NATIVE_ARCH" = "arm64-v8a" ] && [ "$is_box64_scheme" = "0" ] && [ -d "$BUILD_DIR/guest_vulkan/$guest_arch/lib" ]; then
        log "  guest_vulkan -> entry/libs/$NATIVE_ARCH (el1 dlopen)"
        mkdir -p "$ROOT/entry/libs/$NATIVE_ARCH"
        # guest Vulkan Loader (libvulkan.so.1) 放 el1 顶层: guest smoke 进程的
        # DT_NEEDED libvulkan.so.1 靠 LD_LIBRARY_PATH(el1 顶层) 名字搜索命中.
        # host vkr 不再担心遮蔽 — vkr_library.c 已改为绝对路径
        # dlopen("/system/lib64/libvulkan.so") 加载宿主系统 Vulkan (Maleoon 935),
        # 不走名字搜索, 顶层 guest loader 不会遮蔽 host 驱动. libvirglrenderer.so.1
        # 的 NEEDED 只有 libepoxy/libc (纯 dlopen vulkan), 无 NEEDED 绑定问题.
        # 注意只清理旧布局残留: 不能碰 libvulkan_virtio.so (venus ICD, 放顶层).
        rm -f "$ROOT/entry/libs/$NATIVE_ARCH"/libvulkan.so \
              "$ROOT/entry/libs/$NATIVE_ARCH"/libvulkan.so.1
        rm -rf "$ROOT/entry/libs/$NATIVE_ARCH/guest-vulkan-loader"
        for f in "$BUILD_DIR/guest_vulkan/$guest_arch"/lib/libvulkan.so.1 \
                 "$BUILD_DIR/guest_vulkan/$guest_arch"/lib/libvulkan.so; do
            [ -f "$f" ] && cp -a "$f" "$ROOT/entry/libs/$NATIVE_ARCH/"
        done
        for f in "$BUILD_DIR/guest_vulkan/$guest_arch"/lib/libvulkan_virtio.so \
                 "$BUILD_DIR/guest_vulkan/$guest_arch"/bin/libwinehua_guest_vulkan_smoke.so \
                 "$BUILD_DIR/guest_vulkan/$guest_arch"/bin/libvenus_sampled_image_probe.so \
                 "$BUILD_DIR/guest_vulkan/$guest_arch"/bin/libvenus_spirv_replay.so \
                 "$BUILD_DIR/guest_vulkan/$guest_arch"/bin/libvenus_heaven_material_replay.so; do
            [ -f "$f" ] && cp -a "$f" "$ROOT/entry/libs/$NATIVE_ARCH/"
        done
        log "  guest_vulkan el1: $(ls "$ROOT/entry/libs/$NATIVE_ARCH"/libvulkan.so* "$ROOT/entry/libs/$NATIVE_ARCH"/libvulkan_virtio.so 2>/dev/null | wc -l) vulkan libs (loader 顶层)"
    fi

    # Native offscreen replay runs in the App/NCP security domain and links the
    # system Host Vulkan loader. Captured resources remain in guest_vulkan so
    # there is one authoritative exact-replay input set for the Host/Venus A/B.
    local host_vulkan_root="$BUILD_DIR/host_vulkan/$NATIVE_ARCH"
    [ -f "$host_vulkan_root/manifest.json" ] || \
        err "Host Vulkan replay manifest missing: $host_vulkan_root/manifest.json"
    [ -f "$host_vulkan_root/bin/heaven_exact_host_replay" ] || \
        err "Host Vulkan replay marker missing: $host_vulkan_root/bin/heaven_exact_host_replay"
    [ -f "$host_vulkan_root/lib/libwinehua_host_heaven_replay.so" ] || \
        err "Host Vulkan replay module missing: $host_vulkan_root/lib/libwinehua_host_heaven_replay.so"
    mkdir -p "$wine_data/bin/host_vulkan"
    cp -a "$host_vulkan_root/"* "$wine_data/bin/host_vulkan/"
    log "  host_vulkan ($NATIVE_ARCH): native exact replay"

    # Smoke 载荷 (v2, automation/smoke.py build 产出) → wine-data/smoke/。
    # 设备端 SmokeHook.seed 的离线源就是 files/wine/smoke (解压自本 zip),
    # 发布环境无 host 也能播种 C:\smoke; 开发环境 host 推送源
    # files/smoke-payload 优先级更高, 改测试不用重装 HAP。
    local smoke_payload="$BUILD_DIR/smoke-payload"
    [ -f "$smoke_payload/manifest.json" ] || \
        err "smoke payload missing: run 'python3 automation/smoke.py build' first"
    mkdir -p "$wine_data/smoke"
    cp -a "$smoke_payload/." "$wine_data/smoke/"
    log "  smoke payload → wine-data/smoke ($(find "$wine_data/smoke" -name '*.exe' | wc -l) exe)"

    # -- 3. 打包 zip → rawfile (不带 wine-data/ 前缀) --
    local rawfile_dir="$WINEHUA/entry/src/main/resources/rawfile"
    mkdir -p "$rawfile_dir"
    local zip_name="wine-data.zip"
    cd "$wine_data"
    rm -f "$STAGING_DIR/$zip_name"
    zip -r "$STAGING_DIR/$zip_name" . -x '*.git*'
    python3 "$SCRIPT_DIR/check_runtime_components.py" --payload "$STAGING_DIR/$zip_name"
    cp "$STAGING_DIR/$zip_name" "$rawfile_dir/"
    local payload_sha
    payload_sha="$(sha256sum "$rawfile_dir/$zip_name" | awk '{print $1}')"
    # smokeSuiteVersion 落盘校验器要核对（载荷由 automation/smoke.py build 产出，
    # 版本号即载荷内容哈希）—— HAP 里的载荷与本次构建不一致时在 package 阶段拦下
    local smoke_suite_version
    smoke_suite_version="$(python3 -c \
        'import json,sys; print(json.load(open(sys.argv[1]))["suiteVersion"])' \
        "$smoke_payload/manifest.json")"
    cat > "$rawfile_dir/wine-runtime-manifest.json" <<EOF
{
  "schemaVersion": 1,
  "payload": "wine-data.zip",
  "payloadSha256": "$payload_sha",
  "smokeSuiteVersion": "$smoke_suite_version"
}
EOF
    log "  $zip_name → rawfile/ ($(du -h "$rawfile_dir/$zip_name" | cut -f1))"

    # 记录本次 assemble 的架构组合, package.sh hap 校验一致性 (方案切换后未重跑
    # assemble 直接 hap → rawfile 与 .wine_arch 宏错配 → 畸形 HAP, 实测发生过)
    printf '%s:%s' "$NATIVE_ARCH" "$WINE_ARCH" > "$rawfile_dir/.wine-data-arch"

    log "Pad 布局组装完成 ($NATIVE_ARCH)"
    echo ""
    echo "  libs/$NATIVE_ARCH/"
    ls -la "$NATIVE_LIBS/" 2>/dev/null || echo "    (empty)"
    echo "  rawfile/$zip_name"
}

log "=== 组装布局 ($NATIVE_ARCH) ==="

# 统一使用 rawfile zip 布局
assemble_pad
