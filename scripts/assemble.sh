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
    local guest_arch="${GUEST_ARCH:-x86_64}"
    rm -rf "$STAGING_DIR"
    rm -rf "$wine_data"
    mkdir -p "$wine_data/bin/x86_64-windows"
    mkdir -p "$wine_data/bin/x86_64-unix"
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
        for so in "$BUILD_DIR/wine-ohos/dlls/"*/*.so; do
            cp "$so" "$NATIVE_LIBS/"
        done
        log "    Wine .so: $(ls "$BUILD_DIR/wine-ohos/dlls/"*/*.so 2>/dev/null | wc -l) files"

        # 交叉编译依赖 → libs/x86_64/
        # (系统 linker 自动搜索此路径, 无需 x86_64-unix 子目录)
        _pick_lib_pad() {
            local name="$1" soname="$2" linker="${3:-}"
            local dest="$NATIVE_LIBS"
            if [ -f "$SYSROOT_EXT_LIB/$soname" ]; then
                cp "$SYSROOT_EXT_LIB/$soname" "$dest/$soname"
            elif [ -f "$SYSROOT/usr/lib/x86_64-linux-ohos/$name" ]; then
                cp "$SYSROOT/usr/lib/x86_64-linux-ohos/$name" "$dest/$soname"
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
        log "    交叉编译依赖 → libs/x86_64/"

        # libc.so → libs/x86_64/
        cp "$SYSROOT/usr/lib/x86_64-linux-ohos/libc.so" "$NATIVE_LIBS/"

        # libfreetype 已由 _pick_lib_pad 放入 libs/x86_64/，系统 linker 可直接找到

        # libwineserver.so (Pad fork+dlopen 入口)
        if [ -f "$BUILD_DIR/wine_server/libwineserver.so" ]; then
            cp "$BUILD_DIR/wine_server/libwineserver.so" "$NATIVE_LIBS/"
            log "    libwineserver.so → libs/x86_64/"
        else
            warn "libwineserver.so 未找到！请先执行: bash scripts/build_wine.sh"
        fi
    elif [ "$NATIVE_ARCH" = "arm64-v8a" ]; then
        # arm64 Pad: Wine .so 是 x86_64, 不放 libs/, 放 rawfile zip
        # box64.so 由 build_box64.sh 放入 NATIVE_LIBS
        log "  → Wine x86_64 .so → rawfile zip"

        # ARM64 原生库 → libs/arm64-v8a/ (Box64 dlopen bridge libraries)
        # Box64 模拟 x86_64 时需要加载 ARM64 原生的 freetype/xkbcommon 等,
        # 系统 linker 搜索 libs/arm64-v8a/
        local aarch64_lib="$SYSROOT_EXT/usr/lib/$NATIVE_TARGET"
        _pick_arm64_native() {
            local soname="$1" linker="${2:-}"
            if [ -f "$aarch64_lib/$soname" ]; then
                cp "$aarch64_lib/$soname" "$NATIVE_LIBS/$soname"
            else
                warn "ARM64 原生库 $soname 未找到, 跳过"
                return 0
            fi
            if [ -n "$linker" ] && [ ! -f "$NATIVE_LIBS/$linker" ]; then
                cp "$aarch64_lib/$soname" "$NATIVE_LIBS/$linker"  # HAP 不支持 symlink, 实体复制
            fi
        }
        # Box64 native bridge libs: soname 文件 + linker 名拷贝
        _pick_arm64_native "libfreetype.so.6"   "libfreetype.so"
        _pick_arm64_native "libxkbcommon.so.0"   "libxkbcommon.so"
        _pick_arm64_native "libxkbregistry.so.0" "libxkbregistry.so"
        _pick_arm64_native "libxml2.so.2"        "libxml2.so"
        _pick_arm64_native "libwayland-client.so.0" "libwayland-client.so"
        _pick_arm64_native "libwayland-server.so.0" "libwayland-server.so"
        _pick_arm64_native "libffi.so.8"         "libffi.so"

        # box64.so → libs/arm64-v8a/ (ARM64 原生翻译器)
        if [ -f "$BUILD_DIR/box64_build/box64.so" ]; then
            cp "$BUILD_DIR/box64_build/box64.so" "$NATIVE_LIBS/"
            log "    box64.so → libs/arm64-v8a/"
        else
            warn "box64.so 未找到！请先执行: bash scripts/build_box64.sh"
        fi

        # ntdll.so → rawfile
        cp "$BUILD_DIR/wine-ohos/dlls/ntdll/ntdll.so" "$wine_data/bin/"

        # x86_64-unix/ .so → rawfile
        for so in "$BUILD_DIR/wine-ohos/dlls/"*/*.so; do
            [ "$(basename "$so")" = "ntdll.so" ] && continue
            cp "$so" "$wine_data/bin/x86_64-unix/"
        done

        # 交叉编译依赖 → rawfile
        _pick_lib_pad_rf() {
            local name="$1" soname="$2" linker="${3:-}"
            local dest="$wine_data/bin/x86_64-unix"
            if [ -f "$SYSROOT_EXT_LIB/$soname" ]; then
                cp "$SYSROOT_EXT_LIB/$soname" "$dest/$soname"
            elif [ -f "$SYSROOT/usr/lib/x86_64-linux-ohos/$name" ]; then
                cp "$SYSROOT/usr/lib/x86_64-linux-ohos/$name" "$dest/$soname"
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
        # 系统 libm.so 是空壳, 必须用我们的版本 (含 math 符号需 libc 兜底)
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
        # GStreamer 插件 (gst-plugins-base/good + gst-libav) → rawfile
        local gst_plugin_dir="$SYSROOT_EXT_LIB/gstreamer-1.0"
        if [ -d "$gst_plugin_dir" ]; then
            mkdir -p "$wine_data/bin/x86_64-unix/gstreamer-1.0"
            for pso in "$gst_plugin_dir"/*.so; do
                [ -f "$pso" ] || continue
                cp "$pso" "$wine_data/bin/x86_64-unix/gstreamer-1.0/"
            done
            log "    GStreamer 插件 ($(ls "$gst_plugin_dir"/*.so 2>/dev/null | wc -l) 个) → rawfile gstreamer-1.0/"
        else
            warn "gstreamer-1.0 插件目录缺失: $gst_plugin_dir"
        fi

        # libgnutls → bin/ (box64 按名 dlopen 搜索路径: .)
        cp "$wine_data/bin/x86_64-unix/libfreetype.so.6" "$wine_data/bin/"
        cp "$wine_data/bin/x86_64-unix/libfreetype.so" "$wine_data/bin/"

        # libc.so → bin/ (当前目录) + x86_64-unix/ (BOX64_LD_LIBRARY_PATH)
        cp "$SYSROOT/usr/lib/x86_64-linux-ohos/libc.so" "$wine_data/bin/"
        cp "$SYSROOT/usr/lib/x86_64-linux-ohos/libc.so" "$wine_data/bin/x86_64-unix/"

        # wine + wineserver (x86_64 ELF, 由 box64 加载)
        cp "$BUILD_DIR/wine-ohos/loader/wine" "$wine_data/bin/"
        if [ -f "$BUILD_DIR/wine_server/wineserver" ]; then
            cp "$BUILD_DIR/wine_server/wineserver" "$wine_data/bin/"
        elif [ -f "$BUILD_DIR/wine-ohos/server/wineserver" ]; then
            cp "$BUILD_DIR/wine-ohos/server/wineserver" "$wine_data/bin/"
        fi
    fi

    # -- 2. PE DLL + 数据文件 → rawfile (两种架构共用) --
    # x86_64-windows/ — 复制所有运行时 PE 文件
    # .cpl (含 appwiz.cpl) 是否打包由 BUILD_WINE_MONO 决定:
    #   =1 (本地默认): 打包 cpl + build_deps 下载 mono msi, 保留 .NET/控制面板.
    #   =0 (CI 无 curl): 不打包 cpl. 若 mono 缺失, wineboot 初始化时 mscoree.dll
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
    if [ "${BUILD_WINE_MONO:-1}" = "1" ]; then
        pe_exts="$pe_exts cpl"
    fi
    for ext in $pe_exts; do
        for f in "$BUILD_DIR/wine-ohos/dlls/"*/x86_64-windows/*.$ext; do
            [ -f "$f" ] && cp "$f" "$wine_data/bin/x86_64-windows/"
        done
    done
    log "  x86_64-windows → $(ls "$wine_data/bin/x86_64-windows" | wc -l) files"

    # strip PE 调试符号 (DWARF .debug_*, 缩减 ~50%)
    log "  stripping debug symbols..."
    if command -v x86_64-w64-mingw32-strip &>/dev/null; then
        for f in "$wine_data/bin/x86_64-windows/"*.dll "$wine_data/bin/x86_64-windows/"*.drv "$wine_data/bin/x86_64-windows/"*.exe "$wine_data/bin/x86_64-windows/"*.sys; do
            [ -f "$f" ] && x86_64-w64-mingw32-strip "$f" 2>/dev/null
        done
        log "  64-bit PE stripped"
    else
        warn "  x86_64-w64-mingw32-strip not found, skipping strip"
    fi
    # i386-windows/ (32-bit PE DLL for WoW64)
    # 主构建 --enable-archs=i386,x86_64 已产出全部 32-bit PE, 直接取自 wine-ohos,
    # 无需独立的 i686-mingw32 构建.
    # 注意: wineboot/rpcss/services/conhost 等服务程序只有 x86_64 版,
    # WoW64 下它们由 Wine 以 64 位进程拉起, 属上游 WoW64 的正常行为.
    mkdir -p "$wine_data/bin/i386-windows"
    # 与 x86_64 一致: cpl 仅当 BUILD_WINE_MONO=1 时打包 (见上方 pe_exts 注释)
    for ext in $pe_exts; do
        for f in "$BUILD_DIR/wine-ohos/dlls/"*/i386-windows/*.$ext; do
            [ -f "$f" ] && cp "$f" "$wine_data/bin/i386-windows/"
        done
    done
    log "  i386-windows → $(ls "$wine_data/bin/i386-windows" | wc -l) files (ALL)"

    # 32-bit exe stubs, 放在 bin/i386-windows/.
    # Wine 通过 WINEARCH 或 exe header 判断 32/64, 自动加载对应 DLL.
    # 排除测试程序: wine 的 bin/ 目录不放 smoke 程序, 载荷走 wine-data/smoke
    # 独立树 (见 assemble 尾部, 设备端经 SmokeHook.seed 播种到 C:\smoke);
    # winehua_keep.exe 是产品必需 (wine_launch.cpp 拷进 system32).
    for exe in "$BUILD_DIR/wine-ohos/programs/"*/i386-windows/*.exe; do
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

    # *.exe stubs → rawfile (bin/ 不放 smoke 程序, 同上)
    for exe in "$BUILD_DIR/wine-ohos/programs/"*/x86_64-windows/*.exe; do
        case "$(basename "$exe")" in
            winehua_*_smoke.exe|winehua_dinput_probe.exe) continue ;;
        esac
        cp "$exe" "$wine_data/bin/"
    done
    local dxvk_root="$DXVK_BUILD_ROOT"
    [ -f "$dxvk_root/x64/bin/d3d11.dll" ] || err "DXVK Legacy x64 d3d11.dll missing: $dxvk_root/x64/bin/d3d11.dll"
    [ -f "$dxvk_root/x64/bin/dxgi.dll" ] || err "DXVK Legacy x64 dxgi.dll missing: $dxvk_root/x64/bin/dxgi.dll"
    [ -f "$dxvk_root/x86/bin/d3d11.dll" ] || err "DXVK Legacy x86 d3d11.dll missing: $dxvk_root/x86/bin/d3d11.dll"
    [ -f "$dxvk_root/x86/bin/dxgi.dll" ] || err "DXVK Legacy x86 dxgi.dll missing: $dxvk_root/x86/bin/dxgi.dll"
    mkdir -p "$wine_data/dxvk/legacy/x64" "$wine_data/dxvk/legacy/x86"
    cp "$dxvk_root/x64/bin/d3d11.dll" "$wine_data/dxvk/legacy/x64/d3d11.dll"
    cp "$dxvk_root/x64/bin/dxgi.dll" "$wine_data/dxvk/legacy/x64/dxgi.dll"
    cp "$dxvk_root/x86/bin/d3d11.dll" "$wine_data/dxvk/legacy/x86/d3d11.dll"
    cp "$dxvk_root/x86/bin/dxgi.dll" "$wine_data/dxvk/legacy/x86/dxgi.dll"
    # D3D10 链必须整套装齐 (实测 WarThunderLauncher 启动器):
    # wine builtin 的 d3d10core 是调 dxgi 的私有导出 DXGID3D10CreateDevice 建
    # 设备的, 而 DXVK 的 dxgi 没有该导出 —— 缺 native d3d10 时程序落到 builtin
    # 链, 又被 WINEDLLOVERRIDES=dxgi=n (禁止回退 builtin) 卡住, 直接 abort。
    # DXVK 1.10.3 自带 d3d10/d3d10_1/d3d10core (2.x 起移除, 故只在 legacy 装),
    # 三者与 dxgi 同一次构建产出, 整套走 native 才自洽。
    local dxvk_d3d10
    for dxvk_d3d10 in d3d10.dll d3d10_1.dll d3d10core.dll; do
        [ -f "$dxvk_root/x64/bin/$dxvk_d3d10" ] || err "DXVK Legacy x64 $dxvk_d3d10 missing"
        [ -f "$dxvk_root/x86/bin/$dxvk_d3d10" ] || err "DXVK Legacy x86 $dxvk_d3d10 missing"
        cp "$dxvk_root/x64/bin/$dxvk_d3d10" "$wine_data/dxvk/legacy/x64/$dxvk_d3d10"
        cp "$dxvk_root/x86/bin/$dxvk_d3d10" "$wine_data/dxvk/legacy/x86/$dxvk_d3d10"
    done
    local dxvk_modern_root="$DXVK_MODERN_BUILD_ROOT"
    [ -f "$dxvk_modern_root/x64/bin/d3d11.dll" ] || err "DXVK Modern x64 d3d11.dll missing: $dxvk_modern_root/x64/bin/d3d11.dll"
    [ -f "$dxvk_modern_root/x64/bin/dxgi.dll" ] || err "DXVK Modern x64 dxgi.dll missing: $dxvk_modern_root/x64/bin/dxgi.dll"
    [ -f "$dxvk_modern_root/x86/bin/d3d11.dll" ] || err "DXVK Modern x86 d3d11.dll missing: $dxvk_modern_root/x86/bin/d3d11.dll"
    [ -f "$dxvk_modern_root/x86/bin/dxgi.dll" ] || err "DXVK Modern x86 dxgi.dll missing: $dxvk_modern_root/x86/bin/dxgi.dll"
    mkdir -p "$wine_data/dxvk/modern-2.6/x64" "$wine_data/dxvk/modern-2.6/x86"
    cp "$dxvk_modern_root/x64/bin/d3d11.dll" "$wine_data/dxvk/modern-2.6/x64/d3d11.dll"
    cp "$dxvk_modern_root/x64/bin/dxgi.dll" "$wine_data/dxvk/modern-2.6/x64/dxgi.dll"
    cp "$dxvk_modern_root/x86/bin/d3d11.dll" "$wine_data/dxvk/modern-2.6/x86/d3d11.dll"
    cp "$dxvk_modern_root/x86/bin/dxgi.dll" "$wine_data/dxvk/modern-2.6/x86/dxgi.dll"
    local vkd3d_root="$VKD3D_PROTON_BUILD_ROOT/limited-500k"
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
    dxvk64_d3d11_sha="$(sha256sum "$wine_data/dxvk/legacy/x64/d3d11.dll" | awk '{print $1}')"
    dxvk64_dxgi_sha="$(sha256sum "$wine_data/dxvk/legacy/x64/dxgi.dll" | awk '{print $1}')"
    dxvk32_d3d11_sha="$(sha256sum "$wine_data/dxvk/legacy/x86/d3d11.dll" | awk '{print $1}')"
    dxvk32_dxgi_sha="$(sha256sum "$wine_data/dxvk/legacy/x86/dxgi.dll" | awk '{print $1}')"
    dxvkmodern64_d3d11_sha="$(sha256sum "$wine_data/dxvk/modern-2.6/x64/d3d11.dll" | awk '{print $1}')"
    dxvkmodern64_dxgi_sha="$(sha256sum "$wine_data/dxvk/modern-2.6/x64/dxgi.dll" | awk '{print $1}')"
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
    log "  VKD3D-Proton 2.6 limited-500K (default mixed D3D12 profile) → vkd3d/limited-500k/x64 (sha256=$vkd3d64_d3d12_sha)"

    # fonts
    cp "$WINE_SRC/fonts/"*.ttf "$wine_data/share/wine/fonts/"
    # NLS
    cp "$BUILD_DIR/wine-ohos/nls/"*.nls "$wine_data/share/wine/nls/"
    # winmd
    cp "$BUILD_DIR/wine-ohos/include/"*.winmd "$wine_data/share/wine/winmd/"
    # Wine Mono (.NET 运行时). Default builds require the exact MSI expected
    # by mscoree/appwiz; an empty directory would otherwise leave wineboot in
    # an interactive installer forever on first launch.
    local wine_mono_msi="$BUILD_DIR/wine-ohos/share/wine/mono/wine-mono-11.1.0-x86.msi"
    if [ "${BUILD_WINE_MONO:-1}" = "1" ]; then
        [ -s "$wine_mono_msi" ] || err "Wine Mono MSI missing: $wine_mono_msi"
        cp "$wine_mono_msi" "$wine_data/share/wine/mono/"
        log "    wine-mono.msi → rawfile share/wine/mono/"
    else
        log "    Wine Mono: SKIP (BUILD_WINE_MONO=0)"
    fi
    # wine.inf (含 OHOS font substitutes)
    cp "$BUILD_DIR/wine-ohos/loader/wine.inf" "$wine_data/share/wine/"
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

    # x86_64: guest Mesa 库必须可被系统 dlopen (el1 bundle libs); el2 数据区 dlopen 被拒 (ENOENT).
    # 仅复制 host libs 中不存在的 guest 专用库, 共享依赖 (libwayland-*/libffi/libz/libc++_shared)
    # 直接复用 el1 中已有的 host 版本.
    if [ "$NATIVE_ARCH" = "x86_64" ] && [ -d "$BUILD_DIR/guest_gfx/$guest_arch/lib" ]; then
        log "  guest_gfx -> entry/libs/x86_64 (el1 dlopen for x86_64)"
        mkdir -p "$ROOT/entry/libs/x86_64"
        for pattern in libEGL.so libGLESv2.so libGLESv1_CM.so libgallium-*.so libdrm.so; do
            for f in "$BUILD_DIR/guest_gfx/$guest_arch/lib"/$pattern*; do
                [ -f "$f" ] && cp -a "$f" "$ROOT/entry/libs/x86_64/"
            done
        done
        for f in "$BUILD_DIR/guest_gfx/$guest_arch/lib"/dri/*.so; do
            [ -f "$f" ] && cp -a "$f" "$ROOT/entry/libs/x86_64/"
        done
        log "  guest_gfx el1: $(ls "$ROOT/entry/libs/x86_64"/libEGL.so* "$ROOT/entry/libs/x86_64"/libgallium-*.so 2>/dev/null | wc -l) libs + $(ls "$ROOT/entry/libs/x86_64"/*_dri.so 2>/dev/null | wc -l) dri drivers"
    fi

    # Guest Linux Vulkan runtime: x86_64 OHOS ELF/Loader/ICD 栈, 经 Box64 启动.
    if [ -f "$BUILD_DIR/guest_vulkan/$guest_arch/manifest.json" ]; then
        mkdir -p "$wine_data/bin/guest_vulkan"
        cp -a "$BUILD_DIR/guest_vulkan/$guest_arch/"* "$wine_data/bin/guest_vulkan/"
        log "  guest_vulkan ($guest_arch): Loader + Venus ICD"
    elif [ "${BUILD_GUEST_VULKAN:-0}" = "1" ]; then
        err "BUILD_GUEST_VULKAN=1 but build/guest_vulkan/$guest_arch/manifest.json is missing"
    else
        log "  guest_vulkan: SKIP"
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
    cp "$STAGING_DIR/$zip_name" "$rawfile_dir/"
    local payload_sha
    payload_sha="$(sha256sum "$rawfile_dir/$zip_name" | awk '{print $1}')"
    cat > "$rawfile_dir/wine-runtime-manifest.json" <<EOF
{
  "schemaVersion": 1,
  "payload": "wine-data.zip",
  "payloadSha256": "$payload_sha"
}
EOF
    log "  $zip_name → rawfile/ ($(du -h "$rawfile_dir/$zip_name" | cut -f1))"

    log "Pad 布局组装完成 ($NATIVE_ARCH)"
    echo ""
    echo "  libs/$NATIVE_ARCH/"
    ls -la "$NATIVE_LIBS/" 2>/dev/null || echo "    (empty)"
    echo "  rawfile/$zip_name"
}

log "=== 组装布局 ($NATIVE_ARCH) ==="

# 统一使用 rawfile zip 布局
assemble_pad
