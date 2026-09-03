#!/bin/bash
# Build VKD3D-Proton 2.6 d3d12.dll as ARM64X hybrid DLL for the arm64 native runtime.
#
# 与 dxvk 同管线: llvm-mingw clang -marm64x 单遍双图对象 + ld.lld -m arm64xpe。
# 依赖: 已有的 vkd3d-proton meson 构建产物 (limited-500k-build) 提供
#   widl 生成的 13 个接口头 (vkd3d_d3d12.h / vkd3d_dxgi*.h ...) 与
#   vkd3d_build.h / vkd3d_version.h (configure/vcs_tag 产物)。
#   → Makefile 让 build_vkd3d_proton.sh (meson) 先跑。
# dxgi COM helper (IDXGIFactory4_EnumAdapters 等) 在 -O3 下由 static inline
# 内联为 vtable 调用, 无外部符号引用 (与 meson x64 构建同行为)。
# dxil-spirv 用 -DHAVE_LLVMBC (内嵌 LLVM bitcode 解析器, 无需 DXC LLVM fork)。
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/env.sh"

[ -f "$VKD3D_PROTON_SRC/meson.build" ] || err "VKD3D-Proton source missing: $VKD3D_PROTON_SRC"
command -v glslangValidator >/dev/null 2>&1 || err "glslangValidator missing"

find_arm64x_mingw() {
    for d in "${ARM64X_MINGW:-}" \
             "$ROOT/.temp/llvm-mingw-20260826-ucrt-ubuntu-22.04-x86_64" \
             "/data/share/winebox/.temp/llvm-mingw-20260826-ucrt-ubuntu-22.04-x86_64"; do
        [ -n "${d:-}" ] && [ -x "$d/bin/clang" ] && { echo "$d"; return 0; }
    done
    err "ARM64X 构建需要 llvm-mingw 20260826+ (20260616 无 ARM64EC C++ 库)。
  可用 CI 同款下载命令:
    curl -fL -o \$ROOT/.temp/llvm-mingw-20260826-ucrt-ubuntu-22.04-x86_64.tar.xz \
      https://github.com/mstorsjo/llvm-mingw/releases/download/20260826/llvm-mingw-20260826-ucrt-ubuntu-22.04-x86_64.tar.xz
    tar -xJf \$ROOT/.temp/llvm-mingw-20260826-ucrt-ubuntu-22.04-x86_64.tar.xz -C \$ROOT/.temp"
}
MGW="$(find_arm64x_mingw)"
CLANG="$MGW/bin/clang"
LLVM_READOBJ="$MGW/bin/llvm-readobj"

SRC="$VKD3D_PROTON_SRC"
MESON_ROOT="$VKD3D_PROTON_BUILD_ROOT/limited-500k-build"
IDL="$MESON_ROOT/libs/vkd3d-common/libvkd3d_common.a.p"   # widl 生成的接口头
DXIL_SRC="$SRC/subprojects/dxil-spirv"

ARM64X_ROOT="$VKD3D_PROTON_BUILD_ROOT/limited-500k/arm64x"
BUILD="$ARM64X_ROOT/build"
GEN="$BUILD/gen"
OBJD="$BUILD/obj"
BIN="$ARM64X_ROOT/bin"
mkdir -p "$GEN" "$OBJD" "$BIN"

[ -f "$IDL/vkd3d_d3d12.h" ] || err "VKD3D meson widl 头缺失: $IDL (先执行 meson 构建 build_vkd3d_proton.sh)"

# ── 1) glslang shader .h (_vn 同 meson) ──
for f in $(find "$SRC/libs/vkd3d/shaders" \( -name '*.comp' -o -name '*.frag' \
               -o -name '*.geom' -o -name '*.vert' \) 2>/dev/null); do
    b=$(basename "$f"); bn=${b%.*}
    glslangValidator --quiet -V --vn "$bn" "$f" -o "$GEN/$bn.h" >/dev/null 2>&1 || \
        err "glslang failed on $f"
done

# ── 2) vkd3d -marm64x 编译 (flags 对齐 meson ARGS: -O3 使 dxgi COM static
#        inline 内联为 vtable 调用, 与 x64 构建同样无外部引用) ──
INC="-I$SRC/include -I$SRC/include/private -I$SRC/include/d3d12 -I$SRC/libs/vkd3d \
  -I$SRC/libs/vkd3d-common -I$SRC/subprojects/Vulkan-Headers/include \
  -I$SRC/subprojects/SPIRV-Headers/include -I$SRC/subprojects/dxil-spirv \
  -I$IDL -I$MESON_ROOT -I$GEN"
FLAGS='-O3 -std=c11 -D_GNU_SOURCE -D_WIN32_WINNT=0x600 -DUNICODE -DVKD3D_BIN=0x3228be382b2f5620 -DVKD3D_EXPORTS -DPACKAGE_VERSION="2.6" -DVKD3D_ENABLE_DESCRIPTOR_QA -DVKD3D_BUILD_STANDALONE_D3D12 -DVKD3D_EXPERIMENTAL_LIMITED_RESOURCE_VIEW_HEAPS -DVKD3D_NO_TRACE_MESSAGES -fvisibility=hidden -Wno-incompatible-pointer-types'   # 对齐 gcc 的 warning 级别

VKD3D_SRCS=$(ls "$SRC/libs/vkd3d"/*.c "$SRC/libs/vkd3d-common"/*.c \
                "$SRC/libs/vkd3d-shader"/*.c "$SRC/libs/d3d12"/*.c 2>/dev/null)
N=0
for s in $VKD3D_SRCS; do
    rel=${s#$SRC/}; obj="$OBJD/$(echo "$rel" | tr '/' '_').o"
    [ -f "$obj" ] && continue
    N=$((N+1))
    "$CLANG" --target=aarch64-w64-mingw32 -marm64x $FLAGS $INC -c "$s" -o "$obj" 2> "$obj.err" || {
        echo "==== $rel FAIL:"; head -6 "$obj.err"; exit 1;
    }
done

# ── 3) dxil-spirv -marm64x 编译 (-DHAVE_LLVMBC: 内嵌 LLVM bitcode 解析器) ──
DXIL_INC="-I$DXIL_SRC -I$DXIL_SRC/bc -I$DXIL_SRC/debug -I$DXIL_SRC/util \
  -I$DXIL_SRC/third_party/bc-decoder -I$DXIL_SRC/third_party/glslang-spirv \
  -I$DXIL_SRC/third_party/cli_parser -I$DXIL_SRC/third_party/SPIRV-Tools/include \
  -I$DXIL_SRC/third_party/spirv-headers/include/spirv/unified1 \
  -I$SRC/subprojects/SPIRV-Headers/include"
DXIL_SRCS=$(find "$DXIL_SRC" -maxdepth 2 \( -name '*.cpp' -o -name '*.c' \) 2>/dev/null | \
  grep -v -e /third_party/bc/ -e "$DXIL_SRC/dxil_spirv.cpp" \
          -e "$DXIL_SRC/dxil_extract.cpp" -e "$DXIL_SRC/misc/")
DXIL_SRCS=$(echo "$DXIL_SRCS"; ls "$DXIL_SRC"/bc/*.cpp "$DXIL_SRC"/third_party/glslang-spirv/*.cpp \
  "$DXIL_SRC"/third_party/bc-decoder/*.cpp "$DXIL_SRC"/opcodes/*.cpp \
  "$DXIL_SRC"/opcodes/dxil/*.cpp "$DXIL_SRC"/util/*.cpp "$DXIL_SRC"/debug/*.cpp 2>/dev/null)
DXIL_SRCS=$(echo "$DXIL_SRCS" | tr ' ' '\n' | sort -u)
for s in $DXIL_SRCS; do
    rel=${s#$DXIL_SRC/}; obj="$OBJD/dxilspirv_$(echo "$rel" | tr '/' '_').o"
    [ -f "$obj" ] && continue
    N=$((N+1))
    "$CLANG" --target=aarch64-w64-mingw32 -marm64x -O2 -std=c++14 \
      -DHAVE_LLVMBC -DNOMINMAX -DAMD_EXTENSIONS -ffast-math -fno-exceptions \
      -include exception $DXIL_INC -c "$s" -o "$obj" 2> "$obj.err" || {
        echo "==== $rel FAIL:"; head -6 "$obj.err"; exit 1;
    }
done
log "compiled $N sources (total $(ls "$OBJD"/*.o | wc -l) objects)"

# ── 4) 链接: 21 源提示: def 文件转 .drectve shim; dxgi 的 COM helper 由
#        -O3 inline 化, 无需外部库 ──
SHIM="$BUILD/d3d12.exports.S"
{
    echo '	.section .drectve,"r"'
    while read -r line; do
        n=$(echo "$line" | awk '{print $1}'); o=$(echo "$line" | awk '{print $2}')
        echo "	.asciz \"/EXPORT:${n},${o}\""
    done < <(sed -n '3,$p' "$SRC/libs/d3d12/d3d12.def" | \
      grep -oE '^[[:space:]]+[A-Za-z_][A-Za-z0-9_]*(@[0-9]+)?[[:space:]]*$')
} > "$SHIM"
"$CLANG" --target=aarch64-w64-mingw32 -marm64x -c "$SHIM" -o "$BUILD/d3d12.exports.o"

log "Linking d3d12.dll ($(ls "$OBJD"/*.o | wc -l) objects)..."
"$CLANG" --target=aarch64-w64-mingw32 -marm64x -shared \
    -o "$BIN/d3d12.dll" \
    -Wl,--out-implib="$ARM64X_ROOT/d3d12.lib" \
    "$OBJD"/*.o "$BUILD/d3d12.exports.o" \
    "$BUILD_DIR/wine-ohos-$WINE_ARCH/dlls/dxgi/aarch64-windows/libdxgi.a" \
    -L"$MGW/aarch64-w64-mingw32/lib" \
    -lc++abi -lunwind -lc++ \
    -lkernel32 -luser32 -lgdi32 -lwinspool -lshell32 -lole32 -loleaut32 -luuid \
    -lcomdlg32 -ladvapi32 -lmingwex -lmingw32 -lmsvcrt \
    2> "$BIN/d3d12.link.log" || {
    err "d3d12.dll link failed (see $BIN/d3d12.link.log): $(grep -m1 -E 'undefined|error' "$BIN/d3d12.link.log" || echo no-detail)"
}
headers="$("$LLVM_READOBJ" --file-headers "$BIN/d3d12.dll")"
echo "$headers" | grep -q '0xA641' || err "d3d12.dll is not ARM64X (missing ARM64EC subimage)"

[ -f "$BIN/d3d12.dll" ] || err "VKD3D-Proton ARM64X artifact missing: $BIN/d3d12.dll"
log "VKD3D-Proton ARM64X profile ready: $BIN/d3d12.dll ($(ls -lh "$BIN/d3d12.dll" | awk '{print $5}'))"
