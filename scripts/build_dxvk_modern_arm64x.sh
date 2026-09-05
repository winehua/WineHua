#!/bin/bash
# Build the DXVK 2.6 (dxvk-modern) ARM64X hybrid profile for the arm64 native runtime.
#
# 方案③ (aarch64 wine + FEX): x64 guest 应用加载 d3d11/dxgi.dll 时走 FEX 逐条转译。
# 本脚本用 llvm-mingw clang 的 -marm64x 把 dxvk-modern 编译为 ARM64X DLL
# (Machine=0xAA64 主图 + ARM64EC 0xA641 子图), 使 FEX 以 native view 执行,
# 而非对 x64 DLL 逐条指令转译。
#
# 前提: llvm-mingw ≥ 20260826 (其 aarch64-w64-mingw32 的 libc++/libc++abi/libunwind
# 静态库本身已是 AA+EC 双图布局; 20260616 无 ARM64EC C++ 库, 不可用于本产线)。
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/env.sh"

[ -f "$DXVK_MODERN_SRC/meson.build" ] || err "DXVK Modern source missing: $DXVK_MODERN_SRC"
[ -f "$DXVK_MODERN_SRC/include/vulkan/include/vulkan/vulkan.h" ] || \
    err "DXVK Modern Vulkan-Headers submodule is missing"
[ -f "$DXVK_MODERN_SRC/include/spirv/include/spirv/unified1/spirv.hpp" ] || \
    err "DXVK Modern SPIRV-Headers submodule is missing"
command -v glslangValidator >/dev/null 2>&1 || \
    err "glslangValidator missing (负责 dxvk shader .h 生成)"

# ── 工具链: ARM64X 需要 20260826+ ──
find_arm64x_mingw() {
    # 同 build_dxvk_arm64x.sh: LLVM_MINGW 由 env.sh 保证 (自动下载+双图校验)
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

# ── 目录 ──
ARM64X_ROOT="$DXVK_MODERN_BUILD_ROOT/arm64x"
BUILD="$ARM64X_ROOT/build"
GEN="$BUILD/gen"
OBJD="$BUILD/obj"
BIN="$ARM64X_ROOT/bin"
mkdir -p "$GEN" "$OBJD" "$BIN"

# ── 1) meson 配置产物 (PoV: meson 只做 configure_file/vcs_tag/glslang, 手搓等价) ──
sed -e 's/@VCS_TAG@/v2.6/' "$DXVK_MODERN_SRC/version.h.in" > "$GEN/version.h"
cat > "$GEN/buildenv.h" <<'EOF'
#pragma once
#define DXVK_TARGET "aarch64"
#define DXVK_COMPILER "clang"
#define DXVK_COMPILER_VERSION "23"
EOF

# ── 2) glslang shader .h ──
log "Generating shader headers (glslang)..."
for f in $(find "$DXVK_MODERN_SRC/src/dxvk/shaders" "$DXVK_MODERN_SRC/src/dxvk/hud/shaders" \
               "$DXVK_MODERN_SRC/src/d3d11/shaders" \
               \( -name '*.vert' -o -name '*.frag' -o -name '*.comp' -o -name '*.geom' \) 2>/dev/null); do
    b=$(basename "$f"); bn=${b%.*}
    glslangValidator --quiet -V --vn "$bn" "$f" -o "$GEN/$bn.h" >/dev/null 2>&1 || \
        err "glslang failed on $f"
done

# ── 3) libdisplay-info pnp-id-table.c (其 meson 生成产物) ──
[ -f "$GEN/pnp-id-table.c" ] || \
    python3 "$DXVK_MODERN_SRC/subprojects/libdisplay-info/tool/gen-search-table.py" \
      "$DXVK_MODERN_SRC/subprojects/libdisplay-info/pnp.ids" \
      "$GEN/pnp-id-table.c" pnp_id_table 2>/dev/null || true

# ── 4) -marm64x 单遍编译 (每源一个 AA+EC 双图对象) ──
CFLAGS="-std=c++17 -DNOMINMAX -D_WIN32_WINNT=0xa00 -DDXVK_WSI_WIN32 \
  -I$DXVK_MODERN_SRC/src -I$DXVK_MODERN_SRC/include \
  -I$DXVK_MODERN_SRC/include/vulkan/include \
  -I$DXVK_MODERN_SRC/include/spirv/include -I$DXVK_MODERN_SRC/include/native \
  -I$DXVK_MODERN_SRC/subprojects/libdisplay-info/include -I$GEN"
# C 源 (libdisplay-info/sha1): 无 C++17 参数, 需 meson 同款 -Dstatic_array=static
CFLAGS_C="-std=c11 -DNOMINMAX -D_WIN32_WINNT=0xa00 -D_POSIX_C_SOURCE=200809L -Dstatic_array=static \
  -I$DXVK_MODERN_SRC/src -I$DXVK_MODERN_SRC/include \
  -I$DXVK_MODERN_SRC/include/vulkan/include \
  -I$DXVK_MODERN_SRC/include/spirv/include -I$DXVK_MODERN_SRC/include/native \
  -I$DXVK_MODERN_SRC/subprojects/libdisplay-info/include -I$GEN"

SRCS=$(find "$DXVK_MODERN_SRC/src/wsi" "$DXVK_MODERN_SRC/src/util" "$DXVK_MODERN_SRC/src/spirv" \
           "$DXVK_MODERN_SRC/src/vulkan" "$DXVK_MODERN_SRC/src/dxvk" "$DXVK_MODERN_SRC/src/dxbc" \
           "$DXVK_MODERN_SRC/src/dxgi" "$DXVK_MODERN_SRC/src/d3d10" "$DXVK_MODERN_SRC/src/d3d11" \
           \( -name '*.cpp' -o -name '*.c' \) 2>/dev/null)
SRCS="$SRCS $(find "$DXVK_MODERN_SRC/subprojects/libdisplay-info" -maxdepth 1 -name '*.c' 2>/dev/null)"
SRCS="$SRCS $GEN/pnp-id-table.c"

# 编译参数指纹 (增量 skip 不感知 CFLAGS 变化, 见 build_dxvk_arm64x.sh 注释)
fp="$(printf '%s\n%s\n%s' "$CFLAGS" "$CFLAGS_C" "${CFLAGS_EXTRA:-}" | sha256sum | awk '{print $1}')"
fp_file="$OBJD/.cflags_fp"
[ -f "$fp_file" ] && [ "$(cat "$fp_file" 2>/dev/null)" = "$fp" ] || { rm -rf "$OBJD"; mkdir -p "$OBJD"; }
printf '%s\n' "$fp" > "$fp_file"
N=0
for s in $SRCS; do
    if [[ "$s" == $DXVK_MODERN_SRC/src/* ]]; then
        rel=${s#$DXVK_MODERN_SRC/src/}
    else
        rel=$(echo "$s" | tr '/' '_')
    fi
    obj="$OBJD/$(echo "$rel" | tr '/' '_').o"
    [ -f "$obj" ] && continue
    N=$((N+1))
    if [[ "$s" == *.c ]]; then CF="$CFLAGS_C"; else CF="$CFLAGS"; fi
    "$CLANG" --target=aarch64-w64-mingw32 -marm64x $CF -c "$s" -o "$obj" 2> "$obj.err" || {
        echo "==== $rel FAIL:"; head -6 "$obj.err"; exit 1;
    }
done
log "compiled $N sources (total $(ls "$OBJD"/*.o 2>/dev/null | wc -l) objects)"

# ── 5) 链接: clang driver -marm64x → ld.lld -m arm64xpe ──
# 注意库序: libc++abi 必须在 libc++ 之前 (反序报 "EC symbol was replaced")。
# def 文件 gnu 前端不认 → 生成 .drectve shim 对象导出 (/EXPORT:name,@N)。
# 对象名: src/xxx 前缀源 + 绝对路径子串 libdisplay-info/pnp-id-table
KEEP_COMMON='util_ spirv_ vulkan_ wsi_ dxvk_'
KEEP_OLD='dxbc_ d3d10_ d3d11_'

link_profile() {
    local dll="$1" keep="" extra_implib=""
    if [ "$dll" = "dxgi" ]; then
        keep="$KEEP_COMMON dxgi_"
    else # d3d11: 全部 common + 仅 dxgi 组的 dxgi_format + dxbc/d3d10/d3d11
        keep="$KEEP_COMMON $KEEP_OLD" extra_implib="$ARM64X_ROOT/dxgi.lib"
    fi

    local objs=() base o p
    for o in "$OBJD"/*.o; do
        base=$(basename "$o")
        case "$base" in *libdisplay-info*|*pnp-id-table*) objs+=("$o"); continue;; esac
        for p in $keep; do
            case "$base" in
              "$p"*)
                # d3d11 只需 dxgi 组里的 dxgi_format (其余会致 Logger::s_instance 重复定义)
                [ "$dll" = "d3d11" ] && [ "$p" = "dxgi_" ] && \
                  case "$base" in dxgi_dxgi_format*) ;; *) continue 2;; esac
                objs+=("$o"); break;;
            esac
        done
    done

    # .drectve shim: /EXPORT:name,@N (符号本尊在 dxvk 对象中)
    # 进程替换避免 while 管道 EOF 返回 1 触发 set -e
    local shim="$BUILD/$dll.exports.S"
    {
        echo '	.section .drectve,"r"'
        while read -r line; do
            local n o
            n=$(echo "$line" | awk '{print $1}'); o=$(echo "$line" | awk '{print $2}')
            echo "	.asciz \"/EXPORT:${n},${o}\""
        done < <(sed -n '3,$p' "$DXVK_MODERN_SRC/src/$dll/$dll.def" | \
          grep -oE '^[[:space:]]+[A-Za-z_][A-Za-z0-9_]*(@[0-9]+)?[[:space:]]*$')
    } > "$shim"
    "$CLANG" --target=aarch64-w64-mingw32 -marm64x -c "$shim" -o "$BUILD/$dll.exports.o"

    log "Linking $dll.dll (${#objs[@]} objects)..."
    # 静态 .a + --start-group 三件套 (见 build_dxvk_arm64x.sh 同款注释;
    # --- 注释必须在命令块之前, 续行中插 # 会注释掉整条链接命令 ---)
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
        err "DXVK Modern $dll.dll link failed (see $BIN/$dll.link.log): $(grep -m1 -E 'undefined|replaced|error' "$BIN/$dll.link.log" || echo no-detail)"
    }
    # 验证真 ARM64X (0xA64E + 0xA641 双层); 先读完再 grep, 避免 -q 提前关管道吃 SIGPIPE
    local headers
    headers="$("$LLVM_READOBJ" --file-headers "$BIN/$dll.dll")"
    echo "$headers" | grep -q '0xA641' || \
        err "$dll.dll is not ARM64X (missing ARM64EC subimage)"
    # 产物级断言: WSI 必须编译进 Win32 路径。无 -DDXVK_WSI_WIN32 时运行时抛
    # "DXVK_WSI_DRIVER environment variable unset" → DXGI factory 失败 → d3d11/d3d12 全灭
    # (本会话实测: 增量 skip 吞掉 CFLAGS 变化的根因)。字符串仍在 → 宏没生效。
    # 注: 不能用 `grep -q X && err` —— 函数内 && 列表失败状态会泄漏给调用方,
    # set -e 静默杀掉脚本 (无 err 消息)。统一用计数式: 通过路径返回 0。
    n=$(grep -ac 'DXVK_WSI_DRIVER environment variable unset' "$BIN/$dll.dll" || true)
    [ "$n" = 0 ] || err "$dll.dll compiled without -DDXVK_WSI_WIN32"
    # 产物级断言: C++ 运行时必须静态进 DLL (与 build_dxvk_arm64x.sh 同款说明)
    imports="$("$LLVM_READOBJ" --coff-imports "$BIN/$dll.dll")"
    n=$(echo "$imports" | grep -ci 'libc++\|libunwind' || true)
    [ "$n" = 0 ] || err "$dll.dll dynamically links libc++/libunwind, should be static"
}

link_profile dxgi
link_profile d3d11

for dll in d3d11.dll dxgi.dll; do
    [ -f "$BIN/$dll" ] || err "DXVK Modern ARM64X artifact missing: $BIN/$dll"
done

log "DXVK Modern ARM64X profile ready: $BIN/ ($(ls -lh "$BIN"/*.dll | awk '{print $5, $9}'))"
