#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/env.sh"

DEST_ROOT="${WINEHUA_OHOS_MESA_SRC_ROOT:-$ROOT/tmp/ohos-guest-gfx-src}"
MESA_DIR_NAME="${WINEHUA_OHOS_MESA_DIR_NAME:-third_party_mesa3d}"
LIBDRM_DIR_NAME="${WINEHUA_OHOS_LIBDRM_DIR_NAME:-third_party_libdrm}"
MESA_URL="${WINEHUA_OHOS_MESA_URL:-https://gitee.com/openharmony/third_party_mesa3d.git}"
LIBDRM_URL="${WINEHUA_OHOS_LIBDRM_URL:-https://gitee.com/openharmony/third_party_libdrm.git}"
MESA_BRANCH="${WINEHUA_OHOS_MESA_BRANCH:-OpenHarmony-6.0-Beta1}"
LIBDRM_BRANCH="${WINEHUA_OHOS_LIBDRM_BRANCH:-OpenHarmony-6.0-Beta1}"
MESA_REF="${WINEHUA_OHOS_MESA_REF:-}"
LIBDRM_REF="${WINEHUA_OHOS_LIBDRM_REF:-}"
UPDATE_EXISTING=0
MESA_ONLY=0
CLONE_RETRIES="${WINEHUA_OHOS_CLONE_RETRIES:-3}"

repo_is_usable() {
    local dir="$1"
    [ -d "$dir/.git" ] || return 1
    git -C "$dir" rev-parse --verify HEAD >/dev/null 2>&1
}

usage() {
    cat <<'EOF'
Usage:
  bash scripts/fetch_ohos_mesa.sh [--dest-root <dir>] [--mesa-branch <name>] [--libdrm-branch <name>]
                                  [--mesa-ref <commit-or-tag>] [--libdrm-ref <commit-or-tag>]
                                  [--update] [--mesa-only]

What it does:
  - Fetches the official OpenHarmony Mesa source tree used for the guest_gfx receiver work.
  - Optionally fetches the matching official OpenHarmony libdrm source tree.
  - Uses a sparse, blob-filtered clone for Mesa so the first download stays manageable.

Current default receiver direction:
  - Wine's OpenGL path in this tree goes through winewayland.drv + EGL_WAYLAND_KHR.
  - For the actual Windows guest receiver bundle, prefer building Mesa with the
    Wayland platform enabled.
  - platform_ohos.c matters only for direct native OHOS EGL experiments.

Defaults:
  mesa repo   : https://gitee.com/openharmony/third_party_mesa3d.git
  libdrm repo : https://gitee.com/openharmony/third_party_libdrm.git
  branch      : OpenHarmony-6.0-Beta1
  dest root   : tmp/ohos-guest-gfx-src

Important:
  - Stock OHOS Mesa already contains src/egl/drivers/dri2/platform_ohos.c.
  - The current upstream OHOS native platform path is zink-oriented.
  - That is not the primary path for Wine guest testing, because Wine uses
    the Wayland EGL platform in the current renderer integration.
  - Preferred reproducible path: manage Mesa/libdrm as child repos under
    thirdparty/ and let scripts/build_ohos_guest_gfx.sh consume those first.
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        --dest-root)
            [ $# -ge 2 ] || err "--dest-root requires a value"
            DEST_ROOT="$2"
            shift
            ;;
        --mesa-branch)
            [ $# -ge 2 ] || err "--mesa-branch requires a value"
            MESA_BRANCH="$2"
            shift
            ;;
        --libdrm-branch)
            [ $# -ge 2 ] || err "--libdrm-branch requires a value"
            LIBDRM_BRANCH="$2"
            shift
            ;;
        --mesa-ref)
            [ $# -ge 2 ] || err "--mesa-ref requires a value"
            MESA_REF="$2"
            shift
            ;;
        --libdrm-ref)
            [ $# -ge 2 ] || err "--libdrm-ref requires a value"
            LIBDRM_REF="$2"
            shift
            ;;
        --update)
            UPDATE_EXISTING=1
            ;;
        --mesa-only)
            MESA_ONLY=1
            ;;
        -h|--help|help)
            usage
            exit 0
            ;;
        *)
            err "unknown option: $1"
            ;;
    esac
    shift
done

DEST_ROOT="$(normalize_host_path_input "$DEST_ROOT")"
MESA_DIR="$DEST_ROOT/$MESA_DIR_NAME"
LIBDRM_DIR="$DEST_ROOT/$LIBDRM_DIR_NAME"

checkout_requested_ref() {
    local dir="$1"
    local label="$2"
    local ref="$3"

    [ -n "$ref" ] || return 0
    log "Checking out $label ref: $ref"
    if git -C "$dir" rev-parse --verify "$ref^{commit}" >/dev/null 2>&1; then
        git -C "$dir" checkout --detach "$ref"
        return 0
    fi

    git -C "$dir" fetch --depth 1 origin "$ref"
    git -C "$dir" checkout --detach FETCH_HEAD
}

clone_or_update_mesa() {
    local dir="$1"
    local attempt=1
    local seed_dir=""

    if [ -d "$dir" ] && ! repo_is_usable "$dir"; then
        warn "Removing incomplete Mesa checkout: $dir"
        remove_tree "$dir"
    fi

    if repo_is_usable "$dir"; then
        log "Mesa source already exists: $dir"
        if [ "$UPDATE_EXISTING" -eq 1 ]; then
            log "Updating Mesa branch $MESA_BRANCH"
            git -C "$dir" fetch --depth 1 origin "$MESA_BRANCH"
            git -C "$dir" checkout "$MESA_BRANCH"
            git -C "$dir" merge --ff-only FETCH_HEAD
        fi
        checkout_requested_ref "$dir" "Mesa" "$MESA_REF"
        return 0
    fi

    seed_dir="$(find_first_existing_dir \
        "${WINEHUA_OHOS_MESA_LOCAL_SEED:-}" \
        "$ROOT/tmp/third_party_mesa3d" \
        || true)"
    if [ -n "$seed_dir" ] && repo_is_usable "$seed_dir" && [ "$seed_dir" != "$dir" ]; then
        log "Reusing local OHOS Mesa clone: $seed_dir"
        place_path_reference "$seed_dir" "$dir"
        checkout_requested_ref "$dir" "Mesa" "$MESA_REF"
        return 0
    fi

    log "Cloning official OHOS Mesa: branch=$MESA_BRANCH"
    while :; do
        if git -c http.version=HTTP/1.1 -c core.compression=0 \
            clone --depth 1 --filter=blob:none --sparse --branch "$MESA_BRANCH" \
            "$MESA_URL" "$dir"; then
            break
        fi

        remove_tree "$dir"
        if [ "$attempt" -ge "$CLONE_RETRIES" ]; then
            err "failed to clone OHOS Mesa after $CLONE_RETRIES attempts"
        fi

        warn "Mesa clone attempt $attempt/$CLONE_RETRIES failed; retrying in 3s"
        attempt=$(( attempt + 1 ))
        sleep 3
    done

    git -C "$dir" sparse-checkout set \
        android \
        bin \
        build-support \
        docs \
        include \
        licenses \
        ohos \
        src \
        subprojects
    checkout_requested_ref "$dir" "Mesa" "$MESA_REF"
}

clone_or_update_libdrm() {
    local dir="$1"
    local attempt=1
    local seed_dir=""

    if [ -d "$dir" ] && ! repo_is_usable "$dir"; then
        warn "Removing incomplete libdrm checkout: $dir"
        remove_tree "$dir"
    fi

    if repo_is_usable "$dir"; then
        log "libdrm source already exists: $dir"
        if [ "$UPDATE_EXISTING" -eq 1 ]; then
            log "Updating libdrm branch $LIBDRM_BRANCH"
            git -C "$dir" fetch --depth 1 origin "$LIBDRM_BRANCH"
            git -C "$dir" checkout "$LIBDRM_BRANCH"
            git -C "$dir" merge --ff-only FETCH_HEAD
        fi
        checkout_requested_ref "$dir" "libdrm" "$LIBDRM_REF"
        return 0
    fi

    seed_dir="$(find_first_existing_dir \
        "${WINEHUA_OHOS_LIBDRM_LOCAL_SEED:-}" \
        "$ROOT/tmp/third_party_libdrm" \
        || true)"
    if [ -n "$seed_dir" ] && repo_is_usable "$seed_dir" && [ "$seed_dir" != "$dir" ]; then
        log "Reusing local OHOS libdrm clone: $seed_dir"
        place_path_reference "$seed_dir" "$dir"
        checkout_requested_ref "$dir" "libdrm" "$LIBDRM_REF"
        return 0
    fi

    log "Cloning official OHOS libdrm: branch=$LIBDRM_BRANCH"
    while :; do
        if git -c http.version=HTTP/1.1 -c core.compression=0 \
            clone --depth 1 --branch "$LIBDRM_BRANCH" \
            "$LIBDRM_URL" "$dir"; then
            break
        fi

        remove_tree "$dir"
        if [ "$attempt" -ge "$CLONE_RETRIES" ]; then
            err "failed to clone OHOS libdrm after $CLONE_RETRIES attempts"
        fi

        warn "libdrm clone attempt $attempt/$CLONE_RETRIES failed; retrying in 3s"
        attempt=$(( attempt + 1 ))
        sleep 3
    done

    checkout_requested_ref "$dir" "libdrm" "$LIBDRM_REF"
}

mkdir -p "$DEST_ROOT"

clone_or_update_mesa "$MESA_DIR"
if [ "$MESA_ONLY" -eq 0 ]; then
    clone_or_update_libdrm "$LIBDRM_DIR"
fi

log "OHOS guest_gfx sources ready"
printf '  mesa   : %s\n' "$MESA_DIR"
if [ "$MESA_ONLY" -eq 0 ]; then
printf '  libdrm : %s\n' "$LIBDRM_DIR"
fi
printf '  branch : mesa=%s libdrm=%s\n' "$MESA_BRANCH" "$LIBDRM_BRANCH"
printf '  ref    : mesa=%s libdrm=%s\n' "${MESA_REF:-<branch-head>}" "${LIBDRM_REF:-<branch-head>}"
printf '  note   : inspect %s\n' "$MESA_DIR/src/egl/drivers/dri2/platform_ohos.c"
printf '           stock OHOS native EGL currently hard-wires zink there,\n'
printf '           but Wine guest OpenGL in this tree primarily uses the Wayland EGL path.\n'
