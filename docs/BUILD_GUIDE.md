# Wine for HarmonyOS — 构建指南

> 最后更新: 2026-06-18

---

## 环境

- WSL2 Ubuntu 26.04
- OHOS SDK 15.0.4 (`/apps/harmony`)
- 设备: ARM64 HarmonyOS (HAD-W32)

---

## 快速开始

```bash
# 完整构建链: assemble → hnp → hap → 签名 → 推送 → 安装 → 启动
bash build.sh quick <device_ip>

# 例如:
bash build.sh quick 192.168.1.3:41121
```

---

## 构建子命令

| 命令 | 说明 |
|------|------|
| `bash build.sh hap` | 编译 ArkTS + Native → 打包 .hap → 签名 |
| `bash build.sh deploy <ip>` | 推送 HAP → 安装 → 启动 |
| `bash build.sh assemble` | 组装 HNP 布局 |
| `bash build.sh hnp` | 打包 HNP |
| `bash build.sh quick <ip>` | assemble → hnp → hap → deploy 一键 |

### 构建依赖

```bash
# 首次构建前需要编译所有交叉编译依赖
bash scripts/build_deps.sh
# 包含: freetype → wayland → xkbcommon → xkeyboard-config → ARM64 native
```

---

## 项目结构

```
wineohos/
├── build.sh                       # 统一构建入口
├── scripts/
│   ├── env.sh                     # 环境变量
│   ├── build_deps.sh              # 构建全部依赖
│   ├── build_wine.sh              # Wine 构建
│   ├── build_box64.sh             # Box64 构建
│   ├── build_xkbconfig.sh         # XKB 数据构建
│   ├── assemble.sh                # 组装 HNP 布局
│   └── package.sh                 # 打包 HNP/HAP
├── thirdparty/                    # git submodule
│   ├── wine/                      # honwine/wine (fork)
│   ├── box64/                     # honwine/box64 (fork)
│   ├── freetype/
│   ├── wayland/
│   ├── libxkbcommon/
│   ├── xkeyboard-config/
│   └── ...
├── HonWine/                       # HarmonyOS 应用
│   └── entry/src/main/
│       ├── cpp/                   # C++ (Wayland compositor, InputManager)
│       └── ets/                   # ArkTS (WineWindow, WineWindowManager)
└── docs/                          # 技术文档
```
