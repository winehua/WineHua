#!/usr/bin/env bash
# prepare-wine-valve.sh — 物化 thirdparty/wine-valve（CI / 新 clone 必需）
#
# 背景：构建用的 wine 源码是 thirdparty/wine-valve（Valve proton_11.0 + WineHua OHOS 移植），
# 它不是 submodule，而是 winehua/wine.git 的一条分支在本地以 worktree 形式存在：
#
#     thirdparty/wine       ← 注册 submodule（winehua/wine.git）
#     thirdparty/wine-valve ← 本脚本创建：thirdparty/wine 的 worktree，指向 WINE_VALVE_REF
#
# 本地开发机通常已经有这个 worktree（脚本检测到就直接返回）；CI/新 clone 上由工作流先跑本脚本。
# 覆盖分支：WINE_VALVE_REF=ohos-port-steam-win64（默认）
#
# 用法：仓库根目录执行 ./scripts/prepare-wine-valve.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WS="$ROOT/thirdparty/wine"
WV="$ROOT/thirdparty/wine-valve"
REF="${WINE_VALVE_REF:-ohos-port-steam-win64}"

if [ -f "$WV/dlls/ntdll/loader.c" ]; then
    echo "[wine-valve] 已存在，跳过（$(git -C "$WV" rev-parse --short HEAD 2>/dev/null || echo '?')）"
    exit 0
fi

if [ ! -e "$WS/.git" ]; then
    echo "[wine-valve] 错误：$WS 不存在（先 git submodule update --init thirdparty/wine）" >&2
    exit 1
fi

# CI 上 origin 就是 winehua/wine.git；本地开发机的 origin 可能指向别的克隆（例如
# /home/liufeng/src/WineHua-arm64ec/thirdparty/wine），此时用 fork remote。
REMOTE=origin
if ! git -C "$WS" remote get-url origin 2>/dev/null | grep -q "winehua/wine"; then
    if git -C "$WS" remote get-url fork >/dev/null 2>&1; then
        REMOTE=fork
    fi
fi

echo "[wine-valve] fetch $REF from $REMOTE ($(git -C "$WS" remote get-url "$REMOTE"))"
git -C "$WS" fetch --depth 1 "$REMOTE" "$REF"

echo "[wine-valve] worktree add -> $WV"
mkdir -p "$(dirname "$WV")"
git -C "$WS" worktree add --detach "$WV" FETCH_HEAD

test -f "$WV/dlls/ntdll/unix/ohos_virtual.c" || {
    echo "[wine-valve] 错误：worktree 里缺少 OHOS 移植文件，检查 $REF 是否正确" >&2
    exit 1
}

echo "[wine-valve] ok: $(git -C "$WV" log --oneline -1)"
