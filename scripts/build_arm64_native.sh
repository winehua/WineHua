#!/bin/bash
# Compatibility wrapper for the old arm64 native build entrypoint.
# Prefer scripts/build_native.sh directly.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
export NATIVE_ARCH=arm64-v8a
exec bash "$SCRIPT_DIR/build_native.sh"
