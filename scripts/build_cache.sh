#!/bin/bash
# Shared content-addressed verification for expensive graphics builds.
#
# This is deliberately a verification cache, not a binary download cache:
# callers keep their normal build/output directories, while the manifest proves
# that both the inputs and every packaged artifact still match before reuse.

if [ -n "${WINEHUA_BUILD_CACHE_SH_LOADED:-}" ]; then
    return 0 2>/dev/null || exit 0
fi
WINEHUA_BUILD_CACHE_SH_LOADED=1

winehua_cache_sha256_file() {
    sha256sum -- "$1" | awk '{print $1}'
}

winehua_cache_files_digest() {
    local input=""
    {
        for input in "$@"; do
            if [ -f "$input" ]; then
                printf 'file=%s\n' "$(basename "$input")"
                winehua_cache_sha256_file "$input"
            elif [ -d "$input" ]; then
                printf 'tree=%s\n' "$(basename "$input")"
                while IFS= read -r -d '' tree_file; do
                    printf 'path=%s\n' "${tree_file#"$input"/}"
                    winehua_cache_sha256_file "$tree_file"
                done < <(find "$input" -type f -not -path '*/.git/*' -print0 | sort -z)
            else
                printf 'missing=%s\n' "$(basename "$input")"
            fi
        done
    } | sha256sum | awk '{print $1}'
}

winehua_cache_git_digest() {
    local repo="$1"
    local repo_top=""
    [ -d "$repo" ] || {
        printf 'missing\n'
        return 0
    }
    repo_top="$(git -c safe.directory="$repo" -C "$repo" rev-parse --show-toplevel 2>/dev/null || true)"
    if [ -z "$repo_top" ] || [ "$(realpath "$repo_top")" != "$(realpath "$repo")" ]; then
        # Git normally walks to parent directories. A missing submodule would
        # otherwise fingerprint the WineHua superproject (including unrelated
        # untracked logs) and might incorrectly validate an old binary.
        printf 'invalid-repository\n'
        return 0
    fi

    {
        printf 'head='
        git -c safe.directory="$repo" -C "$repo" rev-parse HEAD
        printf 'tracked-diff='
        git -c safe.directory="$repo" -C "$repo" diff --no-ext-diff --binary HEAD | \
            sha256sum | awk '{print $1}'
        while IFS= read -r -d '' untracked_file; do
            printf 'untracked=%s:' "$untracked_file"
            winehua_cache_sha256_file "$repo/$untracked_file"
        done < <(git -c safe.directory="$repo" -C "$repo" \
            ls-files --others --exclude-standard -z)
        git -c safe.directory="$repo" -C "$repo" submodule status --recursive 2>/dev/null || true
        git -c safe.directory="$repo" -C "$repo" submodule foreach --quiet --recursive '
            printf "submodule=%s\nhead=" "$displaypath"
            git rev-parse HEAD
            printf "tracked-diff="
            git diff --no-ext-diff --binary HEAD | sha256sum | awk "{print \$1}"
            git ls-files --others --exclude-standard | while IFS= read -r nested_untracked; do
                printf "untracked=%s:" "$nested_untracked"
                sha256sum -- "$nested_untracked" | awk "{print \$1}"
            done
        ' 2>/dev/null || true
    } | sha256sum | awk '{print $1}'
}

winehua_cache_toolchain_digest() {
    local tool=""
    {
        for tool in \
            meson ninja python3 glslangValidator patch \
            x86_64-w64-mingw32-g++ i686-w64-mingw32-g++ \
            x86_64-w64-mingw32-widl i686-w64-mingw32-widl; do
            printf 'tool=%s:' "$tool"
            if command -v "$tool" >/dev/null 2>&1; then
                "$tool" --version 2>&1 | head -n 1
            else
                printf 'missing\n'
            fi
        done
    } | sha256sum | awk '{print $1}'
}

winehua_cache_input_key() {
    local component="$1"
    local source_repo="$2"
    local input_files_digest="$3"
    shift 3

    {
        printf 'schema=winehua-build-input-v1\n'
        printf 'component=%s\n' "$component"
        printf 'source=%s\n' "$(winehua_cache_git_digest "$source_repo")"
        printf 'files=%s\n' "$input_files_digest"
        printf 'toolchain=%s\n' "$(winehua_cache_toolchain_digest)"
        printf 'option=%s\n' "$@"
    } | sha256sum | awk '{print $1}'
}

winehua_cache_verify() {
    local manifest="$1"
    local component="$2"
    local input_key="$3"
    shift 3
    local artifact=""
    local artifact_count="$#"
    local index=0
    local expected=""
    local actual=""

    WINEHUA_CACHE_MISS_REASON="manifest-missing"
    [ -f "$manifest" ] || return 1
    grep -qx 'schema=winehua-build-cache-v1' "$manifest" || {
        WINEHUA_CACHE_MISS_REASON="schema-changed"
        return 1
    }
    grep -qx "component=$component" "$manifest" || {
        WINEHUA_CACHE_MISS_REASON="component-changed"
        return 1
    }
    grep -qx "input_sha256=$input_key" "$manifest" || {
        WINEHUA_CACHE_MISS_REASON="inputs-changed"
        return 1
    }
    grep -qx "artifact_count=$artifact_count" "$manifest" || {
        WINEHUA_CACHE_MISS_REASON="artifact-set-changed"
        return 1
    }

    for artifact in "$@"; do
        [ -s "$artifact" ] || {
            WINEHUA_CACHE_MISS_REASON="artifact-missing:$index"
            return 1
        }
        expected="$(sed -n "s/^artifact\.$index\.size=//p" "$manifest")"
        actual="$(wc -c < "$artifact" | tr -d '[:space:]')"
        [ -n "$expected" ] && [ "$actual" = "$expected" ] || {
            WINEHUA_CACHE_MISS_REASON="artifact-size:$index"
            return 1
        }
        expected="$(sed -n "s/^artifact\.$index\.sha256=//p" "$manifest")"
        actual="$(winehua_cache_sha256_file "$artifact")"
        [ -n "$expected" ] && [ "$actual" = "$expected" ] || {
            WINEHUA_CACHE_MISS_REASON="artifact-hash:$index"
            return 1
        }
        index=$((index + 1))
    done

    WINEHUA_CACHE_MISS_REASON=""
    return 0
}

winehua_cache_write() {
    local manifest="$1"
    local component="$2"
    local input_key="$3"
    shift 3
    local artifact=""
    local index=0
    local tmp_manifest=""

    mkdir -p "$(dirname "$manifest")"
    tmp_manifest="$(mktemp "${manifest}.tmp.XXXXXX")"
    {
        printf 'schema=winehua-build-cache-v1\n'
        printf 'component=%s\n' "$component"
        printf 'input_sha256=%s\n' "$input_key"
        printf 'artifact_count=%s\n' "$#"
        for artifact in "$@"; do
            [ -s "$artifact" ] || {
                rm -f "$tmp_manifest"
                return 1
            }
            printf 'artifact.%s.size=%s\n' "$index" \
                "$(wc -c < "$artifact" | tr -d '[:space:]')"
            printf 'artifact.%s.sha256=%s\n' "$index" \
                "$(winehua_cache_sha256_file "$artifact")"
            index=$((index + 1))
        done
    } > "$tmp_manifest"
    mv -f "$tmp_manifest" "$manifest"
}
