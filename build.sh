#!/bin/bash
#
# build.sh — Wine for HarmonyOS 构建入口
#
# 用法:
#   ./build.sh {command} [device_ip] [arch]
#
# arch: arm64 (默认) | x86_64 | all
#
# 命令:
#   full       全量构建 (含依赖)
#   deps       模拟层交叉编译依赖 (Wine用, x86_64-linux-ohos)
#   native     Native compositor 依赖 (按架构)
#   wine       构建 Wine
#   box64      构建 Box64 (仅 arm64)
#   assemble   组装 HNP 布局 (按架构)
#   hnp        打包 HNP (按架构)
#   hap        构建 HAP + 签名 (按架构)
#   deploy     推送到设备并安装
#   quick      assemble → hnp → hap → deploy
#
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
SCRIPTS="$ROOT/scripts"

DEFAULT_IP="192.168.1.4:38879"

# ── 参数解析 ──
cmd="${1:-}"
device_ip="${DEFAULT_IP}"
arch="arm64"

case $# in
    0) ;;
    1) cmd="$1" ;;
    *)
        cmd="$1"
        shift
        device_ip="${DEFAULT_IP}"
        arch="arm64"
        for arg in "$@"; do
            if [[ "$arg" == *":"* ]] || [[ "$arg" =~ ^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
                device_ip="$arg"
            elif [ "$arg" = "arm64" ] || [ "$arg" = "x86_64" ] || [ "$arg" = "all" ]; then
                arch="$arg"
            fi
        done
        ;;
esac

# ── 验证 arch ──
case "$arch" in
    arm64) NATIVE_ARCH="arm64-v8a" ;;
    x86_64) NATIVE_ARCH="x86_64" ;;
    all) ;;
    *) echo "错误: arch 必须是 arm64 | x86_64 | all"; exit 1 ;;
esac

export NATIVE_ARCH

# ── 工具函数 ──
log()  { echo -e "\033[32m[BUILD]\033[0m $*"; }
warn() { echo -e "\033[33m[WARN]\033[0m $*"; }

run_native() {
    local a="${1:-arm64-v8a}"
    NATIVE_ARCH="$a" bash "$SCRIPTS/build_native.sh"
}

run_assemble() {
    local a="${1:-arm64-v8a}"
    NATIVE_ARCH="$a" bash "$SCRIPTS/assemble.sh"
}

run_deps() {
    bash "$SCRIPTS/build_deps.sh"
}

run_wine() {
    bash "$SCRIPTS/build_wine.sh"
}

run_box64() {
    bash "$SCRIPTS/build_box64.sh"
}

run_hnp() {
    local a="${1:-arm64-v8a}"
    NATIVE_ARCH="$a" bash "$SCRIPTS/package.sh" hnp
}

run_hap() {
    local a="${1:-arm64-v8a}"
    NATIVE_ARCH="$a" bash "$SCRIPTS/package.sh" hap
}

run_deploy() {
    bash "$SCRIPTS/package.sh" deploy "$device_ip"
}

# ── 多架构迭代 ──
for_each_arch() {
    local fn="$1"
    if [ "$arch" = "all" ]; then
        $fn arm64-v8a
        $fn x86_64
    else
        $fn "$NATIVE_ARCH"
    fi
}

# assemble + hnp 必须配对 (assemble 会清除 staging 目录)
for_each_arch_assemble_and_hnp() {
    if [ "$arch" = "all" ]; then
        log "=== 架构: arm64-v8a (assemble + hnp) ==="
        NATIVE_ARCH=arm64-v8a bash "$SCRIPTS/assemble.sh"
        NATIVE_ARCH=arm64-v8a bash "$SCRIPTS/package.sh" hnp
        log "=== 架构: x86_64 (assemble + hnp) ==="
        NATIVE_ARCH=x86_64 bash "$SCRIPTS/assemble.sh"
        NATIVE_ARCH=x86_64 bash "$SCRIPTS/package.sh" hnp
    else
        NATIVE_ARCH="$NATIVE_ARCH" bash "$SCRIPTS/assemble.sh"
        NATIVE_ARCH="$NATIVE_ARCH" bash "$SCRIPTS/package.sh" hnp
    fi
}

# ── 命令处理 ──
case "$cmd" in
    deps)
        run_deps
        ;;
    native)
        for_each_arch run_native
        ;;
    wine)
        run_wine
        ;;
    box64)
        run_box64
        ;;
    assemble)
        # all 模式下 assemble + hnp 配对, 避免 staging 被覆盖
        for_each_arch_assemble_and_hnp
        ;;
    hnp)
        # hnp 自动先 assemble (确保 staging 对应当前架构)
        for_each_arch_assemble_and_hnp
        ;;
    hap)
        run_hap "$NATIVE_ARCH"
        ;;
    deploy)
        run_deploy
        ;;
    quick)
        run_deps
        run_wine
        run_box64
        for_each_arch run_native
        for_each_arch_assemble_and_hnp
        if [ "$arch" = "all" ]; then
            NATIVE_ARCH=all bash "$SCRIPTS/package.sh" hap
        else
            NATIVE_ARCH="$NATIVE_ARCH" bash "$SCRIPTS/package.sh" hap
        fi
        run_deploy
        ;;
    full|all_cmd)
        run_deps
        run_wine
        run_box64
        for_each_arch run_native
        for_each_arch_assemble_and_hnp
        if [ "$arch" = "all" ]; then
            NATIVE_ARCH=all bash "$SCRIPTS/package.sh" hap
        else
            NATIVE_ARCH="$NATIVE_ARCH" bash "$SCRIPTS/package.sh" hap
        fi
        ;;
    *)
        echo "用法: $0 {full|deps|native|wine|box64|assemble|hnp|hap|deploy|quick} [device_ip] [arch]"
        echo ""
        echo "  arch: arm64 (默认) | x86_64 | all"
        echo ""
        echo "  full       全量构建 (首次使用)"
        echo "  deps       模拟层交叉编译依赖 (Wine用, x86_64-linux-ohos)"
        echo "  native     Native compositor 依赖 (按架构)"
        echo "  wine       构建 Wine"
        echo "  box64      构建 Box64 (仅 arm64)"
        echo "  assemble   组装 HNP 布局 (按架构)"
        echo "  hnp        打包 HNP (按架构)"
        echo "  hap        构建 HAP + 签名 (按架构)"
        echo "  deploy     推送到设备并安装"
        echo "  quick      快速: assemble → hnp → hap → deploy"
        exit 1
        ;;
esac
