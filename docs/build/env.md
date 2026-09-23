# 构建环境搭建

> 在裸容器（Docker / WSL2 / fresh install）中从零搭建 Wine for HarmonyOS 的构建环境。
> 构建命令与增量构建说明见 [guide.md](guide.md) 和 `.claude/rules/build-and-log.md`。

## 前提

- **OS:** Ubuntu 26.04 (或其他 Linux, 包名对应调整)
- **OHOS SDK:** `/apps/harmony/` (通过 volume 挂载或本地安装)
- **项目源码:** `/data/src/winehua/` (带完整 thirdparty submodules)

## 虚拟化场景

### Docker 容器

项目根目录提供了 `Dockerfile`，可以直接构建镜像：

```bash
docker build -t wineohos-build .
```

然后挂载源码和 SDK 运行：

```bash
docker run --rm \
  -v /path/to/wineohos:/data/src/winehua \
  -v /path/to/harmony-sdk:/apps/harmony \
  wineohos-build make NATIVE_ARCH=arm64-v8a
```

或手动启动容器：

```bash
docker run -d --name wine \
  -v /path/to/wineohos:/data/src/winehua \
  -v /path/to/harmony-sdk:/apps/harmony \
  ubuntu:26.04 bash -c 'sleep infinity'
```

### WSL2

直接在工作目录操作，OHOS SDK 路径参考 `scripts/env.sh` 中的默认值 `/apps/harmony/sdk/default/openharmony`。

---

## 依赖安装

### 1. 系统包（apt）

```bash
apt-get update && apt-get install -y \
  build-essential cmake ninja-build meson         `# 编译工具链` \
  bison flex autoconf automake libtool             `# autotools (libffi, Wine configure)` \
  pkgconf zip git file python3 python3-pip         `# 工具` \
  libexpat1-dev libxml2-dev libffi-dev             `# wayland-scanner 原生构建 (wayland 依赖 libffi)` \
  libfreetype-dev                                  `# sfnt2fon 字体工具 (Wine 字体 .fon 生成)` \
  glslang-tools                                    `# DXVK 配置阶段硬依赖 (生成内置 SPIR-V)` \
  gcc-mingw-w64-i686 g++-mingw-w64-i686 gcc-mingw-w64-x86-64 g++-mingw-w64-x86-64                             `# Wine OHOS 交叉 PE 编译` \
  default-jdk                                      `# HAP 签名 (java)` \
 && apt-get clean && rm -rf /var/lib/apt/lists/*
```

> Wine native 构建只编译 `tools/` 下的 host 工具，不编译任何 DLL。
> 9 个工具中仅 `sfnt2fon` 需要 host freetype（字体转换），其余工具只需 libc。
> wayland/xkbcommon/GL host 头文件完全不需要。

### 2. Python 包（pip）

```bash
pip3 install --break-system-packages pyyaml mako markupsafe
```

| 包 | 用途 |
|---|------|
| `pyyaml` | virglrenderer 构建 (Python YAML 解析) |
| `mako` | Mesa guest_gfx 构建 (代码生成模板) |
| `markupsafe` | mako 依赖 |

### 3. libxml2.so.2 兼容性修复

OHOS SDK 自带的 `ld.lld` 链接器依赖 `libxml2.so.2`，而 Ubuntu 26.04 提供的是 `libxml2.so.16`。创建符号链接：

```bash
ln -sf /usr/lib/x86_64-linux-gnu/libxml2.so.16 /usr/lib/x86_64-linux-gnu/libxml2.so.2
ldconfig
```

验证：

```bash
/apps/harmony/sdk/default/hms/native/BiSheng/bin/ld.lld --version
# LLD 15.0.4 (compatible with GNU linkers)
```

> "no version information available" 警告可忽略，不影响链接。

---

## 验证环境

```bash
# 进入构建环境
docker exec -it wine bash

# 验证关键工具
for tool in gcc g++ make cmake ninja meson bison flex autoconf libtoolize \
            pkg-config git python3 java x86_64-w64-mingw32-gcc; do
  which $tool >/dev/null 2>&1 && echo "✓ $tool" || echo "✗ $tool MISSING"
done

# 验证 Python 模块
for mod in yaml mako markupsafe; do
  python3 -c "import $mod" 2>/dev/null && echo "✓ $mod" || echo "✗ $mod MISSING"
done

# 验证 OHOS SDK
test -f /apps/harmony/sdk/default/openharmony/native/llvm/bin/clang && echo "✓ clang" || echo "✗ clang MISSING"
test -f /apps/harmony/bin/hvigorw && echo "✓ hvigorw" || echo "✗ hvigorw MISSING"

# 验证 ld.lld 可用
/apps/harmony/sdk/default/hms/native/BiSheng/bin/ld.lld --version >/dev/null 2>&1 \
  && echo "✓ ld.lld" || echo "✗ ld.lld MISSING (check libxml2.so.2 symlink)"

# 验证 wayland-scanner
test -f /usr/local/bin/wayland-scanner && echo "✓ wayland-scanner" || echo "~ wayland-scanner (will be built by build_deps.sh)"
```

---

## 脚本修复

项目构建脚本已适配裸容器环境（`build_xkbconfig.sh` symlink 解引用、`build_wine.sh` 仅编译 host 工具、`assemble.sh` 产物路径等），无需手动处理。guest 侧组件的默认开关注意区分两个入口：

- 通过 Makefile 构建：`BUILD_GUEST_GFX ?= 1`、`BUILD_GUEST_VULKAN ?= 1`（默认开启）
- 直接运行 `scripts/build_deps.sh`：guest_gfx/guest_vulkan 默认跳过，需显式 `BUILD_GUEST_GFX=1 BUILD_GUEST_VULKAN=1`

---

## 构建

```bash
cd /data/src/winehua

# Makefile 方式（推荐）
make NATIVE_ARCH=arm64-v8a

# 或 build.sh 方式
bash build.sh pad arm64
```

构建阶段、stamp 机制与增量构建命令详见 [guide.md](guide.md)。

---



## 补充说明

- `scripts/env.sh` 自动设置 `NATIVE_ARCH`、交叉编译工具链等
- Wine native 构建只编译 host 工具（winegcc 等），不编译 DLL，通过 autoconf cache variables 绕过所有库检测
- `gcc-mingw-w64-x86-64 g++-mingw-w64-x86-64` 是 OHOS 交叉构建需要的 PE 编译器后端，不是 native 构建用的
- Ohos SDK 中的 BiSheng 工具链（`ld.lld`）是闭源预编译的，依赖旧版系统库
