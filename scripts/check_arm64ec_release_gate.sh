#!/bin/bash
# ARM64EC / ARM64X production-flag gate.
# Inspects scripts (always) and existing build trees (if present).
# Does not compile. Exit 1 if any checked artifact would be O0/Debug.
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/env.sh"

fail=0
note() { echo -e "\033[32m[GATE]\033[0m $*" >&2; }
bad()  { echo -e "\033[31m[GATE]\033[0m $*" >&2; fail=1; }

# ── 1) DXVK ARM64X 手搓脚本必须引用 OPT 门禁 ──
for s in "$SCRIPT_DIR/build_dxvk_arm64x.sh" "$SCRIPT_DIR/build_dxvk_modern_arm64x.sh"; do
    grep -q 'ARM64X_OPT_FLAGS' "$s" || bad "$s: missing ARM64X_OPT_FLAGS"
    grep -q 'require_optimization_flags' "$s" || bad "$s: missing require_optimization_flags"
    grep -q 'require_ndebug' "$s" || bad "$s: missing require_ndebug"
done
note "DXVK ARM64X scripts: OPT/NDEBUG gate present"

# ── 2) Wine PE Makefile（方案③ 树存在时）──
wine_mk="$BUILD_DIR/wine-ohos-$WINE_ARCH/Makefile"
[ -f "$wine_mk" ] || wine_mk="$BUILD_DIR/wine-ohos/Makefile"
if [ "$WINE_ARCH" = "aarch64" ] && [ -f "$wine_mk" ]; then
    require_makefile_opt_var "$wine_mk" aarch64_CFLAGS
    require_makefile_opt_var "$wine_mk" arm64ec_CFLAGS
    require_makefile_opt_var "$wine_mk" i386_CFLAGS
    note "Wine PE CFLAGS: aarch64/arm64ec/i386 all have -O*"
elif [ "$WINE_ARCH" = "aarch64" ]; then
    note "Wine Makefile absent ($wine_mk) — skip PE CFLAGS"
fi

# ── 3) FEX CMake cache ──
for pair in "fex-ec:fex-ec" "fex-pe:fex-pe"; do
    dir="${pair%%:*}"; label="${pair##*:}"
    cache="$BUILD_DIR/$dir/CMakeCache.txt"
    if [ -f "$cache" ]; then
        require_cmake_not_debug "$cache" "$label"
        require_cmake_flag_var "$cache" CMAKE_CXX_FLAGS_RELWITHDEBINFO \
            "$label CMAKE_CXX_FLAGS_RELWITHDEBINFO"
        note "$label: RelWithDebInfo with -O*"
    else
        note "$label cache absent — skip"
    fi
done

# ── 4) wowbox64 ──
wow="$BUILD_DIR/box64-pe/wowbox64-prefix/src/wowbox64-build/CMakeCache.txt"
if [ -f "$wow" ]; then
    require_cmake_not_debug "$wow" "wowbox64.dll"
    require_cmake_flag_var "$wow" CMAKE_C_FLAGS_RELEASE "wowbox64.dll CMAKE_C_FLAGS_RELEASE"
    note "wowbox64.dll: Release with -O*"
else
    note "wowbox64 cache absent — skip"
fi

if [ "$fail" -ne 0 ]; then
    err "ARM64EC release gate failed"
fi
log "ARM64EC release gate passed"
