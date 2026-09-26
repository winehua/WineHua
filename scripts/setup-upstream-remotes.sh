#!/usr/bin/env bash
# setup-upstream-remotes.sh — 为定制 submodule 配置 upstream remote（幂等，可重复执行）
#
# 背景：winehua fork 的 submodule（wine/box64/dxvk/libepoxy/mesa/virglrenderer）需要
# 跟踪上游做重合并。upstream remote 存在于 .git/config，不随仓库分发——
# 新 clone 环境必须跑一次本脚本。URL 与 docs/assets/submodule-maintainability.md §1.1 保持一致。
#
# 用法：仓库根目录执行 ./scripts/setup-upstream-remotes.sh
# 可选：执行后 git submodule foreach 'git fetch upstream <branch>' 拉取上游历史
set -euo pipefail

# 仓库根目录 = 脚本所在目录的上一级
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

declare -A UPSTREAMS=(
  [wine]="https://github.com/wine-mirror/wine.git"
  [box64]="https://github.com/ptitSeb/box64.git"
  [dxvk]="https://github.com/doitsujin/dxvk.git"
  [libepoxy]="https://github.com/anholt/libepoxy.git"
  [mesa]="https://gitee.com/openharmony/third_party_mesa.git"
  [virglrenderer]="https://gitlab.freedesktop.org/virgl/virglrenderer.git"
)

for sm in "${!UPSTREAMS[@]}"; do
  url="${UPSTREAMS[$sm]}"
  dir="$ROOT/thirdparty/$sm"
  if [ ! -d "$dir/.git" ] && [ ! -f "$dir/.git" ]; then
    echo "[$sm] 缺失或未初始化，跳过"
    continue
  fi
  if git -C "$dir" remote get-url upstream >/dev/null 2>&1; then
    current=$(git -C "$dir" remote get-url upstream)
    if [ "$current" != "$url" ]; then
      echo "[$sm] upstream 指向 $current，更新为 $url"
      git -C "$dir" remote set-url upstream "$url"
    else
      echo "[$sm] upstream 已配置 ✓"
    fi
  else
    echo "[$sm] 添加 upstream -> $url"
    git -C "$dir" remote add upstream "$url"
  fi
done

echo
echo "完成。需要上游历史时执行:"
echo "  git submodule foreach 'git fetch upstream <默认分支>'"
echo "注意: mesa 的 upstream 是 gitee (OpenHarmony 官方)，受限网络下可能无法 fetch。"
