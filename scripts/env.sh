# 共享环境变量 — 被所有子脚本 source
# 不要直接执行此文件

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export HOST_OS="${HOST_OS:-$(uname -s)}"

# 跨平台 sed -i (GNU: sed -i, BSD: sed -i '')
if [ "$HOST_OS" = "Darwin" ]; then
    sed_i() { sed -i '' "$@"; }
else
    sed_i() { sed -i "$@"; }
fi

# OHOS SDK
# macOS 下直接从 PATH 查找命令行工具目录，并推导相关路径。
if [ "$HOST_OS" = "Darwin" ] && [ -z "${TOOL_HOME:-}" ]; then
    old_ifs="$IFS"
    IFS=:
    for bin_dir in $PATH; do
        [ -n "$bin_dir" ] || bin_dir=.
        candidate_home="$(cd -P "$bin_dir/.." 2>/dev/null && pwd || true)"
        if { [ -e "$bin_dir/ohpm" ] || [ -e "$bin_dir/hvigorw" ]; } \
           && [ -d "$candidate_home/sdk/default/openharmony" ]; then
            export TOOL_HOME="$candidate_home"
            break
        fi
    done
    IFS="$old_ifs"
fi
if [ "$HOST_OS" = "Darwin" ] && [ -z "${OHOS_SDK:-}" ] \
   && [ -d "${TOOL_HOME:-}/sdk/default/openharmony" ]; then
    export OHOS_SDK="$TOOL_HOME/sdk/default/openharmony"
fi
if [ "$HOST_OS" = "Darwin" ] && [ -z "${OHOS_SDK:-}" ]; then
    echo "ERROR: 未找到 HarmonyOS command-line-tools。请将其 bin 目录加入 PATH，或设置 OHOS_SDK。" >&2
    return 1 2>/dev/null || exit 1
fi
export OHOS_SDK="${OHOS_SDK:-/apps/harmony/sdk/default/openharmony}"
export TOOL_HOME="${TOOL_HOME:-/apps/harmony}"
export PATH="$TOOL_HOME/bin:$TOOL_HOME/tool/node/bin:$PATH"

if [ "$HOST_OS" = "Darwin" ] && command -v brew >/dev/null 2>&1; then
    HOMEBREW_BISON_BIN="$(brew --prefix bison 2>/dev/null || true)/bin"
    if [ -x "$HOMEBREW_BISON_BIN/bison" ]; then
        export PATH="$HOMEBREW_BISON_BIN:$PATH"
    fi
fi

CLANG="$OHOS_SDK/native/llvm/bin/clang"
SYSROOT="$OHOS_SDK/native/sysroot"

# llvm-mingw (arm64ec/aarch64 PE 编译 + FEX 构建, 需 LLVM ≥ 18)
# 版本常量只在此一处 (CI workflow / ci/Dockerfile.buildenv 的 version= 与其
# 同步维护)。20260616 的 libc++ 无 ARM64EC C++ 库, 编不出 arm64x 双图 DLL。
# 发现链 (位置完全参数化, 不要求任何固定目录名):
#   1. $LLVM_MINGW 环境变量 — 用户自选位置, 任意目录名;
#   2. $ROOT/.temp/llvm-mingw-<版本>-… — 脚本缺失时自动下载解压的落点
#      (标准布局 = CI 同款, 不是"约定用户手工放这里");
#   3. 无网络 → 显式报错 + 设置指引。
# 均需通过双图库校验 (libc++.a 含 obj.arm64ec/ 成员, 旧版会失败)。
LLVM_MINGW_VERSION="${LLVM_MINGW_VERSION:-20260826}"
LLVM_MINGW_DIRNAME="llvm-mingw-${LLVM_MINGW_VERSION}-ucrt-ubuntu-22.04-x86_64"
LLVM_MINGW="${LLVM_MINGW:-}"
# 注意: arm64ec-w64-mingw32-clang 是二进制文件, 校验用 -x; 用 -d 永远为假 → 一旦
# 某脚本 export LLVM_MINGW 后, 后续所有 source env.sh 的脚本全部误报 (2026-09-05 实测)。
if [ -n "${LLVM_MINGW}" ] && [ ! -x "$LLVM_MINGW/bin/arm64ec-w64-mingw32-clang" ]; then
    echo "ERROR: LLVM_MINGW 指向的工具链不完整: $LLVM_MINGW (缺 bin/arm64ec-w64-mingw32-clang)" >&2
    exit 1
fi
if [ -z "${LLVM_MINGW}" ] && [ -d "$ROOT/.temp/$LLVM_MINGW_DIRNAME" ]; then
    LLVM_MINGW="$ROOT/.temp/$LLVM_MINGW_DIRNAME"
fi
if [ -z "${LLVM_MINGW}" ]; then
    echo "llvm-mingw 缺失, 自动下载 $LLVM_MINGW_VERSION (CI 同款 release) 到 $ROOT/.temp/  ..." >&2
    mkdir -p "$ROOT/.temp"
    tarball="$ROOT/.temp/$LLVM_MINGW_DIRNAME.tar.xz"
    if ! curl -fL --retry 3 --connect-timeout 30 -o "$tarball" \
         "https://github.com/mstorsjo/llvm-mingw/releases/download/$LLVM_MINGW_VERSION/$LLVM_MINGW_DIRNAME.tar.xz"; then
        echo "ERROR: 自动下载失败。请用以下任一方式准备工具链再构建:" >&2
        echo "  方式 A (推荐): http_proxy/https_proxy 正常后重试本命令;" >&2
        echo "  方式 B: 自行下载解压, 然后 export LLVM_MINGW=/path/to/$LLVM_MINGW_DIRNAME" >&2
        echo "  方式 C: 离线拷入 $ROOT/.temp/$LLVM_MINGW_DIRNAME (与 CI 同布局)" >&2
        exit 1
    fi
    tar -xJf "$tarball" -C "$ROOT/.temp" || { echo "ERROR: 解压失败: $tarball" >&2; exit 1; }
    rm -f "$tarball"
    LLVM_MINGW="$ROOT/.temp/$LLVM_MINGW_DIRNAME"
fi
# 双图库校验: 版本合格才放行 (20260616 无 obj.arm64ec/ 成员)
if ! "$LLVM_MINGW/bin/llvm-ar" t "$LLVM_MINGW/aarch64-w64-mingw32/lib/libc++.a" 2>/dev/null \
     | grep -q 'obj\.arm64ec/'; then
    echo "ERROR: $LLVM_MINGW 的 libc++.a 缺 ARM64EC 双图成员 (需 >= $LLVM_MINGW_VERSION)" >&2
    exit 1
fi
export LLVM_MINGW

# ── Native 层架构 (鸿蒙设备 CPU, HAP .so 的目标) ──
# arm64-v8a: 真机 (AArch64)
# x86_64:    模拟器 / x86_64 设备
NATIVE_ARCH="${NATIVE_ARCH:-arm64-v8a}"

# ── Wine 模拟层架构 ──
# arm64 真机 → aarch64 原生 wine + FEX 转译应用 (libarm64ecfex.dll)
# x86_64 模拟器 → x86_64 同目标 wine
if [ -z "${WINE_ARCH:-}" ]; then
    [ "$NATIVE_ARCH" = "arm64-v8a" ] && WINE_ARCH=aarch64 || WINE_ARCH=x86_64
fi
export WINE_ARCH
TARGET="${WINE_ARCH}-linux-ohos"                            # aarch64-linux-ohos | x86_64-linux-ohos
HOST_TRIPLE="${HOST_TRIPLE:-${WINE_ARCH}-unknown-linux-ohos}"  # wine configure --host
GNU_HOST="${GNU_HOST:-${WINE_ARCH}-linux-gnu}"              # autoconf 系 deps --host
OHOS_ARCH="${OHOS_ARCH:-$( [ "$WINE_ARCH" = aarch64 ] && echo arm64-v8a || echo x86_64 )}"
export TARGET HOST_TRIPLE GNU_HOST OHOS_ARCH

# 根据 NATIVE_ARCH 推导 Native 层 LLVM target / meson cpu
case "$NATIVE_ARCH" in
    arm64-v8a)
        NATIVE_TARGET="aarch64-linux-ohos"
        NATIVE_CPU_FAMILY="aarch64"
        NATIVE_CPU="aarch64"
        ;;
    x86_64)
        NATIVE_TARGET="x86_64-linux-ohos"
        NATIVE_CPU_FAMILY="x86_64"
        NATIVE_CPU="x86_64"
        ;;
    all)
        echo "ERROR: NATIVE_ARCH=all 不再支持 (单一 WINE_ARCH 无法同时满足 arm64 原生与 box64+wine 的 assemble)。请分别构建 NATIVE_ARCH=x86_64 / arm64-v8a。" >&2
        exit 1
        ;;
    *)
        echo "ERROR: 不支持的 NATIVE_ARCH: $NATIVE_ARCH (可选: arm64-v8a, x86_64)"
        exit 1
        ;;
esac

# ── 设备上的 Wine 运行时根目录 (由 rawfile zip 解压) ──
WINE_DEVICE_ROOT="/data/storage/el2/base/files/wine"

# 源码路径
WINE_SRC="$ROOT/thirdparty/wine"
DXVK_SRC="$ROOT/thirdparty/dxvk"
# box64+wine 方案 (方案②, arm64 设备 + x86_64 wine) 的 in-process 转译器源码
BOX64_SRC="$ROOT/thirdparty/box64"
DXVK_MODERN_SRC="$ROOT/thirdparty/dxvk-modern"
VKD3D_PROTON_SRC="$ROOT/thirdparty/vkd3d-proton"

# 产物路径
BUILD_DIR="$ROOT/build"          # 源码构建中间产物
SYSROOT_EXT="$BUILD_DIR/sysroot-ext"  # 交叉编译扩展 (不污染 SDK)
STAGING_DIR="$BUILD_DIR/staging"   # 打包临时目录
DXVK_BUILD_ROOT="$BUILD_DIR/dxvk/legacy"
DXVK_MODERN_BUILD_ROOT="$BUILD_DIR/dxvk/modern-2.6"
VKD3D_PROTON_BUILD_ROOT="$BUILD_DIR/vkd3d-proton"

# sysroot-ext 目录结构
SYSROOT_EXT_INC="$SYSROOT_EXT/usr/include"
SYSROOT_EXT_LIB="$SYSROOT_EXT/usr/lib/$TARGET"
# pkgconfig 按架构隔离: 共享目录跨架构切换会残留旧架构 .pc (libdir 指向旧架构)
# → x86_64 构建解析到 aarch64 .pc → 链接旧架构 .so (gstreamer 链实测必现)
SYSROOT_EXT_PC="$SYSROOT_EXT/usr/lib/$TARGET/pkgconfig"
SYSROOT_EXT_SHARE="$SYSROOT_EXT/usr/share"

# Linux/WSL 保留原路径；macOS/HarmonyOS 使用项目内扫描器和当前工具链的 pkg-config。
if [ "$HOST_OS" = "Darwin" ] || [ "$HOST_OS" = "HarmonyOS" ]; then
    export PKG_CONFIG_BIN="${PKG_CONFIG_BIN:-$(command -v pkg-config || true)}"
    export WAYLAND_SCANNER="${WAYLAND_SCANNER:-$BUILD_DIR/host-tools/bin/wayland-scanner}"
    [ -n "${PKG_CONFIG_BIN:-}" ] || err "pkg-config not found in PATH; run: brew install pkg-config"
else
    export PKG_CONFIG_BIN="${PKG_CONFIG_BIN:-/usr/bin/pkg-config}"
    export WAYLAND_SCANNER="${WAYLAND_SCANNER:-/usr/local/bin/wayland-scanner}"
fi

# HAP 项目
WINEHUA="$ROOT"

# Native 层 libs 目录
NATIVE_LIBS="$WINEHUA/entry/libs/$NATIVE_ARCH"

# 编译并行
if [ -z "${JOBS:-}" ]; then
    if command -v nproc >/dev/null 2>&1; then
        JOBS="$(nproc)"
    elif command -v sysctl >/dev/null 2>&1; then
        JOBS="$(sysctl -n hw.ncpu 2>/dev/null || echo 4)"
    else
        JOBS=4
    fi
fi
export JOBS

# 生成 meson cross file (路径依赖 ROOT, 不能硬编码)
gen_cross_file() {
    local cross="$BUILD_DIR/ohos-${WINE_ARCH}-cross.txt"
    # pkg-config wrapper: --with-path 替换默认搜索路径 (宿主系统 /usr/lib/pkgconfig
    # 会混入 x11.pc 等 → 交叉构建误用宿主库探测)
    local pcwrap="$BUILD_DIR/pkg-config-cross.sh"
    cat > "$pcwrap" << PWEOF
#!/bin/sh
# PKG_CONFIG_LIBDIR 替换默认搜索路径 (--with-path 只是追加, 宿主 /usr/lib 仍混入)
# SYSROOT_EXT_PC 已按架构隔离 (usr/lib/$TARGET/pkgconfig), 只加系统 pc 目录
export PKG_CONFIG_LIBDIR="$SYSROOT_EXT_PC:$SYSROOT/usr/lib/pkgconfig"
exec "$PKG_CONFIG_BIN" "\$@"
PWEOF
    chmod +x "$pcwrap"
    cat > "$cross" << XEOF
[binaries]
c = '$OHOS_SDK/native/llvm/bin/clang'
cpp = '$OHOS_SDK/native/llvm/bin/clang++'
ar = '$OHOS_SDK/native/llvm/bin/llvm-ar'
strip = '$OHOS_SDK/native/llvm/bin/llvm-strip'
pkg-config = '$pcwrap'
wayland-scanner = '$WAYLAND_SCANNER'
# 交叉装的 python 工具 (与架构无关, 宿主可直接执行) — meson 默认不搜 cross 前缀 bin
glib-mkenums = '$SYSROOT_EXT/usr/bin/glib-mkenums'
gdbus-codegen = '$SYSROOT_EXT/usr/bin/gdbus-codegen'

[built-in options]
c_args = ['--target=$TARGET', '--sysroot=$SYSROOT', '-I$SYSROOT_EXT_INC']
c_link_args = ['--target=$TARGET', '--sysroot=$SYSROOT', '-fuse-ld=lld', '-L$SYSROOT_EXT_LIB']
# pkgconfig 只走架构隔离的 SYSROOT_EXT_PC (usr/lib/$TARGET/pkgconfig) + OHOS SDK;
# 共享 usr/lib/pkgconfig 已废弃 (跨架构残留 aarch64 .pc → x86_64 链接错架构)
pkg_config_path = ['$SYSROOT_EXT_PC', '$SYSROOT/usr/lib/pkgconfig']

[properties]
# 不设 sys_root: 编译器 --sysroot 已在 c_args/c_link_args 中，
# sysroot-ext 的 .pc 使用绝对路径，无需额外拼接。
# 强制宿主不能执行交叉产物 (build/host 同为 x86_64 linux 时 meson 会误判
# "能跑" → cc.run() 真的执行 OHOS ELF 失败; 与 HiSH deps/libglib 一致的做法),
# 使 meson.can_run_host_binaries() 返回 false, gnulib 检测自动走 else 分支。
needs_exe_wrapper = true

[host_machine]
system = 'linux'
cpu_family = '$WINE_ARCH'
cpu = '$WINE_ARCH'
endian = 'little'
XEOF
    echo "$cross"
}

# meson 构建: touch 源码避免 NFS clock skew
meson_build() {
    local build="$1" src="$2"
    shift 2
    local cross="$(gen_cross_file)"
    # 源码时间戳可能来自 NFS (比本地时钟快), touch 到本地时间
    find "$src" -type f -exec touch {} + 2>/dev/null || true
    mkdir -p "$build"
    meson setup "$build" "$src" --cross-file "$cross" "$@"
}

# 日志
log()  { echo -e "\033[32m[BUILD]\033[0m $*" >&2; }
warn() { echo -e "\033[33m[WARN]\033[0m $*" >&2; }
err()  { echo -e "\033[31m[ERROR]\033[0m $*" >&2; exit 1; }

# ── 共享工具函数 ──
find_first_existing_dir() {
    local candidate=""
    for candidate in "$@"; do
        [ -n "${candidate:-}" ] || continue
        if [ -d "$candidate" ]; then
            printf '%s\n' "$candidate"
            return 0
        fi
    done
    return 1
}

resolve_first_executable() {
    local candidate=""
    for candidate in "$@"; do
        [ -n "${candidate:-}" ] || continue
        if [ -x "$candidate" ]; then
            printf '%s\n' "$candidate"
            return 0
        fi
    done
    return 1
}
