#!/bin/bash
#
# build.sh — Wine for HarmonyOS 构建入口
#
# 用法:
#   ./build.sh all          # 全量构建
#   ./build.sh wine         # 只构建 Wine
#   ./build.sh box64        # 只构建 Box64
#   ./build.sh assemble     # 组装 HNP 布局
#   ./build.sh hnp          # 打包 HNP
#   ./build.sh hap          # 构建 HAP + 签名
#   ./build.sh deploy       # 推送到设备并安装
#   ./build.sh quick        # assemble → hnp → hap → deploy (不改源码时)
#
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"

case "${1:-all}" in
    all)
        bash "$ROOT/scripts/build_wine.sh"
        bash "$ROOT/scripts/build_box64.sh"
        bash "$ROOT/scripts/assemble.sh"
        bash "$ROOT/scripts/package.sh" all "${2:-}"
        ;;
    wine)     bash "$ROOT/scripts/build_wine.sh" ;;
    box64)    bash "$ROOT/scripts/build_box64.sh" ;;
    assemble) bash "$ROOT/scripts/assemble.sh" ;;
    hnp)      bash "$ROOT/scripts/package.sh" hnp ;;
    hap)      bash "$ROOT/scripts/package.sh" hap ;;
    deploy)   bash "$ROOT/scripts/package.sh" deploy "${2:-}" ;;
    quick)
        bash "$ROOT/scripts/assemble.sh"
        bash "$ROOT/scripts/package.sh" all "${2:-}"
        ;;
    *)
        echo "Usage: $0 {all|wine|box64|assemble|hnp|hap|deploy|quick} [device_ip]"
        echo ""
        echo "  all      全量构建 (首次使用)"
        echo "  wine     只构建 Wine"
        echo "  box64    只构建 Box64"
        echo "  assemble 从已有构建产物组装 HNP 布局"
        echo "  hnp      打包 HNP"
        echo "  hap      打包 HAP + 签名"
        echo "  deploy   推送到设备并安装"
        echo "  quick    快速: assemble → hnp → hap → deploy"
        exit 1
        ;;
esac
