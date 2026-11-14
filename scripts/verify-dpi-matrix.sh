#!/usr/bin/env bash
# DPI 一致性矩阵一键验证
#
# 正确命令 (仓库根目录执行):
#   scripts/verify-dpi-matrix.sh [设备地址]
#   例: scripts/verify-dpi-matrix.sh 192.168.1.7:44959
#
# 覆盖矩阵: 显示路线 {wayland, x11} × 桌面模式 {virtual, fusion}。
# 每格跑 smoke/suites/dpi.json (screen-enum / win-basic / win-fullscreen /
# input-mouse / input-relative), 由 suite 级判定 dpi-consistency 读取归档
# 的 wine stderr 打点 (WINEHUA_DPI_DIAG=1，job 声明) 验证:
#   - 所有进程 override=system_dpi=192
#   - 本路线监视器注册 dpi=192
#   - 所有窗口消息 raw==win 且 win_dpi=192
#   - 至少一次建窗为 aware (默认 system-aware 生效)
# fusion 没有 guest 坐标注入通道（按窗注入走 byTitle），只选三个基本用例。
# 单测轮询超时是 D30 首次用例时序抖动（D35 口径），用 --retry-failed 复跑一次；
# 首判留档在归档的 retry-R<N>/ 子目录。
#
# 归档: build/automation-logs/dpi-r<runId>/ (smoke.py 默认归档根)
# 退出码: 任一方格 FAIL 即非零。

set -eo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

DEVICE="${1:-${WINEHUA_DEVICE:-}}"
DEVICE_ARG=()
if [ -n "$DEVICE" ]; then
    DEVICE_ARG=(--device "$DEVICE")
fi

declare -a RESULTS=()
FAILED=0

# 载荷与套件定义同步（run 会拒绝过期载荷）
python3 automation/smoke.py build

for route in wayland x11; do
    for mode in virtual fusion; do
        echo "==== DPI 矩阵: route=${route} desktop=${mode} ===="
        # 冷启参数 (--display-route/--desktop-mode) 要求 app 未运行
        if [ -n "$DEVICE" ]; then
            hdc -t "$DEVICE" shell "aa force-stop app.hackeris.winehua" >/dev/null 2>&1 || true
        fi
        # fusion 没有 guest 坐标注入通道（按窗注入走 byTitle），注入用例
        # 只跑 virtual；fusion 由基本用例覆盖进程/监视器/建窗口径。
        TESTS_ARG=()
        if [ "$mode" = fusion ]; then
            TESTS_ARG=(--tests screen-enum-x64,win-basic-x64,win-fullscreen-x64)
        fi
        if python3 automation/smoke.py run \
                --job "smoke/jobs/dpi-${route}.json" \
                --display-route "$route" \
                --desktop-mode "$mode" \
                --timeout-minutes 40 \
                --retry-failed \
                "${TESTS_ARG[@]}" \
                "${DEVICE_ARG[@]}"; then
            RESULTS+=("${route}/${mode}  PASS")
        else
            RESULTS+=("${route}/${mode}  FAIL")
            FAILED=1
        fi
    done
done

echo
echo "==== DPI 矩阵汇总 ===="
printf '%s\n' "${RESULTS[@]}"
exit "$FAILED"
