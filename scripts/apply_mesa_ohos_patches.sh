#!/bin/bash
# Apply the pinned Mesa OHOS deltas required by the Wine runtime.
set -euo pipefail

[ "$#" -eq 1 ] || {
    echo "usage: $0 <mesa-source-root>" >&2
    exit 64
}

MESA_SOURCE="$1"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PATCH="$SCRIPT_DIR/../patches/mesa/0001-ohos-arm64-keep-vtest-map-fd.patch"

[ -f "$MESA_SOURCE/meson.build" ] || {
    echo "Mesa source root is invalid: $MESA_SOURCE" >&2
    exit 1
}
[ -f "$PATCH" ] || {
    echo "Mesa OHOS patch is missing: $PATCH" >&2
    exit 1
}

if git -c safe.directory="$MESA_SOURCE" -C "$MESA_SOURCE" \
    apply --reverse --check "$PATCH" >/dev/null 2>&1; then
    exit 0
fi

git -c safe.directory="$MESA_SOURCE" -C "$MESA_SOURCE" apply --check "$PATCH" || {
    echo "Mesa source does not match the pinned OHOS patch: $MESA_SOURCE" >&2
    exit 1
}
git -c safe.directory="$MESA_SOURCE" -C "$MESA_SOURCE" apply "$PATCH"
git -c safe.directory="$MESA_SOURCE" -C "$MESA_SOURCE" diff --check
