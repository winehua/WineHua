#!/bin/bash
# Build DXVK Legacy (1.10.3) as ARM64X hybrid DLLs for the arm64 native runtime.
#
# 与 build_dxvk_modern_arm64x.sh 同管线: llvm-mingw clang -marm64x 单遍双图对象
# (AA 主图 + ARM64EC 子图) + ld.lld -m arm64xpe 链接。
# 1.10.3 的 arm64 兼容点 (照抄 dxvk 2.6 官方做法):
#   src/util/util_bit.h      DXVK_ARCH_X86/ARM64 宏 (arm64ec 同时定义 __x86_64__)
#   src/util/util_bit.h      tzcnt 用 __builtin_ctz 替代 bsf/cmovz x86 asm
#   src/util/util_bit.h      bcmpeq 的 __m128i 段仅 DXVK_ARCH_X86
#   src/util/sync/sync_spinlock.h  _mm_pause → arm64 "yield"
#   src/util/util_vector.h   replaceNaN 纯循环回退
#   src/util/config/config.cpp  + <algorithm> (std::transform)
#   src/d3d9/d3d9_device.cpp SetupFPU fnstcw asm 守卫排除 __arm64ec__
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/env.sh"

[ -f "$DXVK_SRC/meson.build" ] || err "DXVK source missing: $DXVK_SRC"
command -v glslangValidator >/dev/null 2>&1 || err "glslangValidator missing"

find_arm64x_mingw() {
    # env.sh 已确保 LLVM_MINGW 就绪 (缺失自动下载/显式报错 + 双图校验);
    # ARM64X_MINGW 仅作显式覆盖 (直接跑本脚本、绕过 env 链时用)。
    for d in "${ARM64X_MINGW:-}" "${LLVM_MINGW:-}"; do
        [ -n "${d:-}" ] && [ -x "$d/bin/clang" ] && \
            "$d/bin/llvm-ar" t "$d/aarch64-w64-mingw32/lib/libc++.a" 2>/dev/null | grep -q 'obj\.arm64ec/' && \
            { echo "$d"; return 0; }
    done
    err "ARM64X 构建需要 llvm-mingw ${LLVM_MINGW_VERSION} 双图工具链 (见 scripts/env.sh 的自动下载逻辑, 或 export LLVM_MINGW=...)。"
}
MGW="$(find_arm64x_mingw)"
CLANG="$MGW/bin/clang"
LLVM_READOBJ="$MGW/bin/llvm-readobj"

ARM64X_ROOT="$DXVK_BUILD_ROOT/arm64x"
BUILD="$ARM64X_ROOT/build"
GEN="$BUILD/gen"
OBJD="$BUILD/obj"
BIN="$ARM64X_ROOT/bin"
mkdir -p "$GEN" "$OBJD" "$BIN"

# ── 1) version.h (meson vcs_tag 产物) ──
sed 's/@VCS_TAG@/v1.10.3/' "$DXVK_SRC/version.h.in" > "$GEN/version.h"

# ── 2) glslang shader .h ──
log "Generating shader headers (glslang)..."
for f in $(find "$DXVK_SRC/src/dxvk/shaders" "$DXVK_SRC/src/dxvk/hud/shaders" \
               "$DXVK_SRC/src/d3d11/shaders" "$DXVK_SRC/src/d3d9/shaders" \
               \( -name '*.vert' -o -name '*.frag' -o -name '*.comp' -o -name '*.geom' \) 2>/dev/null); do
    b=$(basename "$f"); bn=${b%.*}
    glslangValidator --quiet -V --vn "$bn" "$f" -o "$GEN/$bn.h" >/dev/null 2>&1 || \
        err "glslang failed on $f"
done

# ── 3) -marm64x 单遍编译 ──
CFLAGS="-std=c++17 -DNOMINMAX -D_WIN32_WINNT=0xa00 \
  -I$DXVK_SRC/src -I$DXVK_SRC/include -I$DXVK_SRC/include/spirv \
  -I$DXVK_SRC/include/vulkan -I$GEN"
CFLAGS_C="-std=c11 -DNOMINMAX -D_WIN32_WINNT=0xa00 \
  -I$DXVK_SRC/src -I$DXVK_SRC/include -I$DXVK_SRC/include/spirv \
  -I$DXVK_SRC/include/vulkan -I$GEN"

# 注意: 1.10.3 无 src/wsi 目录 (wsi 实现在 dxvk/ 内), find 列缺失路径会返回
# 非零退出码 → set -e 误退, 故只要存在的模块目录
SRCS=$(find "$DXVK_SRC/src/util" "$DXVK_SRC/src/spirv" "$DXVK_SRC/src/vulkan" \
           "$DXVK_SRC/src/dxvk" "$DXVK_SRC/src/dxbc" "$DXVK_SRC/src/dxgi" "$DXVK_SRC/src/d3d10" \
           "$DXVK_SRC/src/d3d11" "$DXVK_SRC/src/d3d9" "$DXVK_SRC/src/dxso" \
           \( -name '*.cpp' -o -name '*.c' \) 2>/dev/null)
# 编译参数指纹: 增量 skip ([ -f "$obj" ] && continue) 不感知 CFLAGS 变化,
# 宏/选项变动必须重编 (2026-09-05 实测: -DDXVK_WSI_WIN32 加了却不生效)。
fp="$(printf '%s\n%s\n%s' "$CFLAGS" "$CFLAGS_C" "${CFLAGS_EXTRA:-}" | sha256sum | awk '{print $1}')"
fp_file="$OBJD/.cflags_fp"
[ -f "$fp_file" ] && [ "$(cat "$fp_file" 2>/dev/null)" = "$fp" ] || { rm -rf "$OBJD"; mkdir -p "$OBJD"; }
printf '%s\n' "$fp" > "$fp_file"
N=0
for s in $SRCS; do
    rel=${s#$DXVK_SRC/src/}
    obj="$OBJD/$(echo "$rel" | tr '/' '_').o"
    [ -f "$obj" ] && continue
    N=$((N+1))
    if [[ "$s" == *.c ]]; then CF="$CFLAGS_C"; else CF="$CFLAGS"; fi
    "$CLANG" --target=aarch64-w64-mingw32 -marm64x $CF -c "$s" -o "$obj" 2> "$obj.err" || {
        echo "==== $rel FAIL:"; head -6 "$obj.err"; exit 1;
    }
done
log "compiled $N sources (total $(ls "$OBJD"/*.o 2>/dev/null | wc -l) objects)"

# ── 4) 链接 (ld.lld arm64xpe; 源集合按 meson 权威清单) ──
#   dxgi.dll  = common + dxgi 全部
#   d3d11.dll = common + dxgi 组仅 format/monitor/swapchain + d3d10 组(去 main) + dxbc + d3d11
# d3d10/d3d11 的 main 各注一份 Logger::s_instance → d3d10 main 排除。
KEEP_COMMON='util_ spirv_ vulkan_ wsi_ dxvk_'

link_profile() {
    local dll="$1" keep="" extra_implib=""
    if [ "$dll" = "dxgi" ]; then
        keep="$KEEP_COMMON dxgi_"
    else
        keep="$KEEP_COMMON dxgi_dxgi_format dxgi_dxgi_monitor dxgi_dxgi_swapchain dxbc_ d3d10_ d3d11_"
        extra_implib="$ARM64X_ROOT/dxgi.lib"
    fi

    local objs=() base o p
    for o in "$OBJD"/*.o; do
        base=$(basename "$o")
        case "$base" in d3d10_d3d10_main*) continue;; esac
        for p in $keep; do
            case "$base" in "$p"*) objs+=("$o"); break;; esac
        done
    done

    local shim="$BUILD/$dll.exports.S"
    {
        echo '	.section .drectve,"r"'
        while read -r line; do
            local n o
            n=$(echo "$line" | awk '{print $1}'); o=$(echo "$line" | awk '{print $2}')
            echo "	.asciz \"/EXPORT:${n},${o}\""
        done < <(sed -n '3,$p' "$DXVK_SRC/src/$dll/$dll.def" | \
          grep -oE '^[[:space:]]+[A-Za-z_][A-Za-z0-9_]*(@[0-9]+)?[[:space:]]*$')
    } > "$shim"
    "$CLANG" --target=aarch64-w64-mingw32 -marm64x -c "$shim" -o "$BUILD/$dll.exports.o"

    log "Linking $dll.dll (${#objs[@]} objects)..."
    # libc++/libunwind 必须直给 .a 静态库并包 --start-group:
    # a) `-lc++` 解析到 libc++.dll.a (import 库), 但 dxvk 对象是
    #    非 imp 引用 (U _ZNSt3__1...), import 库无法满足 → undefined;
    # b) 双图 .a 成员 (root=ARM64, obj.arm64ec/=ARM64EC) 需循环扫描
    #    才能让 EC 侧成员被拉入 (单遍会漏选 EC 成员 → undefined native)。
    # 注意: 注释必须放在命令块之前, 不能插在 `\` 续行中间 (会注释掉
    # 整条链接命令, 2026-09-05 实测 3 次全被这个坑绊住)。
    "$CLANG" --target=aarch64-w64-mingw32 -marm64x -shared \
        -o "$BIN/$dll.dll" \
        -Wl,--out-implib="$ARM64X_ROOT/$dll.lib" \
        "${objs[@]}" \
        "$BUILD/$dll.exports.o" \
        $extra_implib \
        "$BUILD_DIR/wine-ohos-$WINE_ARCH/dlls/vulkan-1/aarch64-windows/libvulkan-1.a" \
        -L"$MGW/aarch64-w64-mingw32/lib" \
        -Wl,--start-group \
            "$MGW/aarch64-w64-mingw32/lib/libc++abi.a" \
            "$MGW/aarch64-w64-mingw32/lib/libunwind.a" \
            "$MGW/aarch64-w64-mingw32/lib/libc++.a" \
        -Wl,--end-group \
        -lole32 -loleaut32 -ldwmapi -lwinmm -limm32 -lcomdlg32 -luxtheme -lsetupapi \
        -lshlwapi -lws2_32 -liphlpapi -lgdi32 \
        2> "$BIN/$dll.link.log" || {
        err "DXVK Legacy $dll.dll link failed (see $BIN/$dll.link.log): $(grep -m1 -E 'undefined|replaced|error' "$BIN/$dll.link.log" || echo no-detail)"
    }
    local headers
    headers="$("$LLVM_READOBJ" --file-headers "$BIN/$dll.dll")"
    echo "$headers" | grep -q '0xA641' || \
        err "$dll.dll is not ARM64X (missing ARM64EC subimage)"
    # 产物级断言: C++ 运行时必须静态进 DLL。导入表出现 libc++/libunwind 意味着
    # -lc++ 被解析回 .dll.a 动态依赖 → guest 启动即 c0000135 (本会话实测)。
    # 注: 不能用 `grep -q X && err` —— 函数内 && 列表失败状态会泄漏给调用方,
    # set -e 静默杀掉脚本 (无 err 消息)。统一用计数式: 通过路径返回 0。
    imports="$("$LLVM_READOBJ" --coff-imports "$BIN/$dll.dll")"
    n=$(echo "$imports" | grep -ci 'libc++\|libunwind' || true)
    [ "$n" = 0 ] || err "$dll.dll dynamically links libc++/libunwind, should be static"
}

link_profile dxgi
link_profile d3d11

for dll in d3d11.dll dxgi.dll; do
    [ -f "$BIN/$dll" ] || err "DXVK Legacy ARM64X artifact missing: $BIN/$dll"
done
log "DXVK Legacy ARM64X profile ready: $BIN/ ($(ls -lh "$BIN"/*.dll | awk '{print $5, $9}'))"
