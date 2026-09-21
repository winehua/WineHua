#!/usr/bin/env bash
# Read-only signature verification for a signed HAP via the SDK's hap-sign-tool.
# Never prints signing material: only the tool's verification output is shown.
set -euo pipefail
cd "$(dirname "$0")/.."
export TOOL_HOME="${TOOL_HOME:-/apps/harmony}"
# shellcheck disable=SC1091
source scripts/env.sh >/dev/null 2>&1

jar="$TOOL_HOME/sdk/default/openharmony/toolchains/lib/hap-sign-tool.jar"
test -f "$jar" || { echo "hap-sign-tool.jar not found at $jar" >&2; exit 1; }

hap="$1"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

java -jar "$jar" verify-app \
    -inFile "$hap" \
    -outCertChain "$work/chain.cer" \
    -outProfile "$work/profile.p7b" 2>&1 | grep -v -i 'password\|pwd' || true

if [ -s "$work/chain.cer" ]; then
    echo "outCertChain bytes: $(stat -c '%s' "$work/chain.cer")"
fi
if [ -s "$work/profile.p7b" ]; then
    echo "outProfile bytes: $(stat -c '%s' "$work/profile.p7b")"
fi
