FROM ubuntu:26.04

# Aliyun mirrors (中国大陆加速)
RUN sed -i 's|http://archive.ubuntu.com/ubuntu/|http://mirrors.aliyun.com/ubuntu/|g' /etc/apt/sources.list.d/ubuntu.sources \
 && sed -i 's|http://security.ubuntu.com/ubuntu/|http://mirrors.aliyun.com/ubuntu/|g' /etc/apt/sources.list.d/ubuntu.sources

RUN apt-get update && apt-get install -y \
    # 编译工具链
    build-essential cmake ninja-build meson \
    bison flex autoconf automake libtool libltdl-dev \
    pkgconf zip git file python3 python3-pip glslang-tools \
    spirv-tools \
    # spirv-tools: build_ohos_guest_vulkan.sh 无条件调用 spirv-as/spirv-dis/spirv-val/spirv-opt
    # (BUILD_GUEST_VULKAN=1 默认开启). 目前作为 glslang-tools 的传递依赖被拉入,
    # 但依赖传递链不可靠, 显式声明保证未来 apt/发行版变动也不丢. (与 ci 镜像一致)
    # gettext (msgfmt): Wine po 翻译资源硬依赖 (build_wine.sh 缺失即 exit 1);
    # curl/wget: 构建期下载 (mono msi 等);
    # texinfo (makeinfo): gmp make all 重新生成 doc/gmp.info 的硬依赖
    gettext curl wget texinfo \
    # wayland-scanner 原生构建 (生成 Wayland 协议代码)
    libexpat1-dev libxml2-dev libffi-dev \
    # sfnt2fon 字体工具 (Wine .fon 生成)
    libfreetype-dev \
    # Wine OHOS 交叉 PE 编译 (i386 + x86_64 mingw, C++17 for icu.dll)
    gcc-mingw-w64-i686 g++-mingw-w64-i686 \
    gcc-mingw-w64-x86-64 g++-mingw-w64-x86-64 \
    # VKD3D-Proton meson 交叉文件硬依赖 x86_64-w64-mingw32-widl
    mingw-w64-tools \
    # HAP 签名
    default-jdk \
 && apt-get clean && rm -rf /var/lib/apt/lists/*

# 注意: libltdl-dev 必须显式列出.
# 它本是 libtool 的 Recommends 依赖, 但 Ubuntu 官方 docker base image 默认
# 关闭 Recommends 安装, 不显式声明就不会被拉入, 导致 aclocal 找不到
# ltdl.m4 里的 LT_SYS_SYMBOL_USCORE 宏, 让 libffi autogen.sh 失败.
# mingw-w64-tools 提供 x86_64-w64-mingw32-widl; gcc-mingw-w64 不会把它
# 作为硬依赖拉进来, VKD3D meson 交叉文件却要求这个程序存在.
# (与 ci/Dockerfile.buildenv 保持一致; CI 的 Ensure CI build dependencies
# step 也有 ltdl.m4 / widl 兜底检查)

# Python 包 (virglrenderer + Mesa guest_gfx 构建)
RUN pip3 install --break-system-packages pyyaml mako markupsafe \
 && rm -rf /root/.cache/pip

# libxml2.so.2 兼容性修复
# OHOS SDK 的 ld.lld 链接器依赖 libxml2.so.2，Ubuntu 26.04 提供的是 libxml2.so.16
RUN ln -sf /usr/lib/x86_64-linux-gnu/libxml2.so.16 /usr/lib/x86_64-linux-gnu/libxml2.so.2 \
 && ldconfig

WORKDIR /data/src/winehua

# 使用时挂载:
#   -v /path/to/wineohos:/data/src/winehua                  (项目源码)
#   -v /path/to/harmony-sdk:/apps/harmony                   (OHOS SDK)
# 可选: 从 Windows/DevEco Studio 直接导入签名, 免去复制到 WSL
#   -v /mnt/c/path/to/deveco_project:/mnt/user-profile      (含 build-profile.json5 的工程目录)
#   -v /mnt/c/path/to/signature_dir:/mnt/user-signature     (含 .cer / .p7b / .p12 的目录)
# 两者需同时挂载才生效; 缺失任一则回退到项目内置 build-profile.json5.
#
# 构建 (NATIVE_ARCH 取值: x86_64 | arm64-v8a | all):
#   docker run --rm -v $(pwd):/data/src/winehua -v ~/huawei/command-line-tools:/apps/harmony \
#     wineohos-build make NATIVE_ARCH=arm64-v8a
