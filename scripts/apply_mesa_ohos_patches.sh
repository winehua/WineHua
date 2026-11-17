#!/bin/bash
# Apply the pinned Mesa OHOS deltas required by the Wine runtime.
set -euo pipefail

[ "$#" -eq 1 ] || {
    echo "usage: $0 <mesa-source-root>" >&2
    exit 64
}

MESA_SOURCE="$1"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PATCHES=(
    "$SCRIPT_DIR/../patches/mesa/0001-ohos-arm64-keep-vtest-map-fd.patch"
    "$SCRIPT_DIR/../patches/mesa/0002-virgl-vtest-wine-owned-low-map.patch"
    "$SCRIPT_DIR/../patches/mesa/0003-venus-present-retry-single-ring-drain.patch"
    "$SCRIPT_DIR/../patches/mesa/0004-venus-wine-owned-low-map.patch"
    "$SCRIPT_DIR/../patches/mesa/0005-zink-ohos-explicit-vulkan-loader.patch"
    "$SCRIPT_DIR/../patches/mesa/0006-virgl-vtest-serialize-socket-transactions.patch"
    "$SCRIPT_DIR/../patches/mesa/0007-virgl-gpu-srgb-surface-storage.patch"
    "$SCRIPT_DIR/../patches/mesa/0008-virgl-explicit-pbuffer-front-present.patch"
    "$SCRIPT_DIR/../patches/mesa/0009-venus-order-shadow-export-after-ring.patch"
    "$SCRIPT_DIR/../patches/mesa/0010-venus-check-after-final-present-backoff.patch"
    "$SCRIPT_DIR/../patches/mesa/0012-venus-pack-small-vtest-submits.patch"
    "$SCRIPT_DIR/../patches/mesa/0013-virgl-vtest-write-command-pair.patch"
    "$SCRIPT_DIR/../patches/mesa/0015-zink-opt-in-shadow-readback.patch"
    "$SCRIPT_DIR/../patches/mesa/0016-zink-opt-in-shadow-uploader.patch"
    "$SCRIPT_DIR/../patches/mesa/0017-zink-shadow-image-staging-byte-range.patch"
    "$SCRIPT_DIR/../patches/mesa/0018-zink-opt-in-full-texcoord.patch"
    "$SCRIPT_DIR/../patches/mesa/0019-zink-opt-in-dynamic-extract.patch"
    "$SCRIPT_DIR/../patches/mesa/0020-zink-opt-in-full-vertex-inputs.patch"
    "$SCRIPT_DIR/../patches/mesa/0021-zink-opt-in-strip-debug-names.patch"
    "$SCRIPT_DIR/../patches/mesa/0022-zink-opt-in-full-generic-varyings.patch"
    "$SCRIPT_DIR/../patches/mesa/0023-zink-opt-in-slice-3d-pbo-upload.patch"
    "$SCRIPT_DIR/../patches/mesa/0024-zink-opt-in-batch-private-uploads.patch"
    "$SCRIPT_DIR/../patches/mesa/0025-zink-opt-in-gpu-pbuffer-present.patch"
    "$SCRIPT_DIR/../patches/mesa/0026-zink-shadow-buffer-map-offset.patch"
    "$SCRIPT_DIR/../patches/mesa/0027-zink-isolate-shadow-image-staging.patch"
)

[ -f "$MESA_SOURCE/meson.build" ] || {
    echo "Mesa source root is invalid: $MESA_SOURCE" >&2
    exit 1
}
MESA_SOURCE="$(cd "$MESA_SOURCE" && pwd)"
# A later reviewed overlay may change an earlier hunk's context. Prove the
# complete stack on a small disposable copy of touched files before accepting
# an incremental tree; never reverse or reset the caller's source.
if python3 - "$MESA_SOURCE" "$SCRIPT_DIR" "${PATCHES[@]}" <<'PY'
import sys
from pathlib import Path
sys.path.insert(0, sys.argv[2])
from apply_dxvk_compat_patches import stack_applied
sys.exit(0 if stack_applied(Path(sys.argv[1]), [Path(p) for p in sys.argv[3:]]) else 1)
PY
then
    echo "Mesa: complete reviewed overlay already applied"
    exit 0
fi
for PATCH in "${PATCHES[@]}"; do
    [ -f "$PATCH" ] || {
        echo "Mesa OHOS patch is missing: $PATCH" >&2
        exit 1
    }
    if git -c safe.directory="$MESA_SOURCE" -C "$MESA_SOURCE" \
        apply --reverse --check "$PATCH" >/dev/null 2>&1; then
        continue
    fi
    git -c safe.directory="$MESA_SOURCE" -C "$MESA_SOURCE" apply --check "$PATCH" || {
        echo "Mesa source does not match the pinned OHOS patch: $MESA_SOURCE ($PATCH)" >&2
        exit 1
    }
    git -c safe.directory="$MESA_SOURCE" -C "$MESA_SOURCE" apply "$PATCH"
done
# Mounted linked worktrees can lack their external Git metadata. git apply
# still validates every hunk there; only the repository diff needs metadata.
if git -c safe.directory="$MESA_SOURCE" -C "$MESA_SOURCE" rev-parse \
    --show-toplevel >/dev/null 2>&1; then
    git -c safe.directory="$MESA_SOURCE" -C "$MESA_SOURCE" diff --check
fi
