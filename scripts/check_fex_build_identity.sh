#!/bin/bash
# Stamp gate only: no SDK discovery, source mutation, or compiler invocation.
set -euo pipefail
script_dir="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$script_dir/.." && pwd)"
mode="${1:?check|write}" stamp="${2:?stamp path}"
build="${BUILD_DIR:?BUILD_DIR required}"
diagnostics=0
[ "${FEX_EXACTSTORE_DIAGNOSTICS:-0}" = 1 ] && diagnostics=1
strict_mul24="${FEX_STRICT_MUL24:-0}"
case "$strict_mul24" in 0|1) ;; *) exit 2 ;; esac

identity() {
    {
        printf 'schema=2\nsource=%s\ndiagnostics=%s\ntoolchain=%s\n' \
            "${FEX_SRC:-$root/build/runtime-sources/fex}" "$diagnostics" "${LLVM_MINGW:-auto}"
        printf 'strict_mul24=%s\n' "$strict_mul24"
        printf 'source_hash=%s\nsource_version=%s\n' "${FEX_SOURCE_HASH:-detect}" "${FEX_SOURCE_VERSION:-detect}"
        # Hash contents as well as timestamps: switching overlays or restoring
        # older mtimes must not reuse an artifact from a different experiment.
        for input in "$script_dir/build_locked_fex.sh" "$script_dir/env.sh" "$script_dir/fex_simd_runtime.py" \
                     "$script_dir/check_fex_build_identity.sh" "$script_dir/prepare_runtime_sources.py" \
                     "$root/docs/upstream-sync/fex-candidate.lock.json" \
                     "$root/docs/upstream-sync/fex-adapted-history.json" "$script_dir"/patches/fex-upstream/*.patch; do
            test -f "$input"
            printf '%s ' "${input#"$script_dir"/}"
            sha256sum "$input" | cut -d ' ' -f 1
        done
    } | sha256sum | cut -d ' ' -f 1
}

check_flags() {
    local pe ec
    test -f "$build/fex-pe/CMakeCache.txt" && test -f "$build/fex-ec/CMakeCache.txt" || return 1
    pe="$(sed -n 's/^CMAKE_CXX_FLAGS:STRING=//p' "$build/fex-pe/CMakeCache.txt")"
    ec="$(sed -n 's/^CMAKE_CXX_FLAGS:STRING=//p' "$build/fex-ec/CMakeCache.txt")"
    [[ " $pe " == *' -DFEX_WOW64_EXACT_STORE=1 '* ]] || return 1
    [[ "$ec" != *FEX_WOW64_EXACT_STORE* ]] || return 1
    [[ "$ec" != *FEX_WOW64_STRICT_MUL24* ]] || return 1
    if [ "$strict_mul24" = 1 ]; then
        [[ " $pe " == *' -DFEX_WOW64_STRICT_MUL24=1 '* ]] || return 1
    else
        [[ "$pe" != *FEX_WOW64_STRICT_MUL24* ]] || return 1
    fi
    if [ "$diagnostics" = 1 ]; then
        [[ " $pe " == *' -DFEX_WOW64_EXACT_STORE_DIAGNOSTICS=1 '* ]] || return 1
    else
        [[ "$pe" != *FEX_WOW64_EXACT_STORE_DIAGNOSTICS* ]] || return 1
    fi
}

case "$mode" in
    check)
        test -f "$stamp.identity" || exit 1
        test "$(cat "$stamp.identity")" = "$(identity)" || exit 1
        check_flags
        ;;
    write)
        check_flags || { echo 'FEX artifact mode does not match requested build identity' >&2; exit 1; }
        identity > "$stamp.identity.tmp.$$"
        mv "$stamp.identity.tmp.$$" "$stamp.identity"
        ;;
    *) exit 2 ;;
esac
