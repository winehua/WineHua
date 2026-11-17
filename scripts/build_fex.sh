#!/bin/bash
# Compatibility entry point; the source recipe is shared with make fex.
set -euo pipefail
exec bash "$(dirname "$0")/build_locked_fex.sh" "$@"
