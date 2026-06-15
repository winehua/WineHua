# Wine for HarmonyOS — 构建指南

> 最后更新: 2026-06-12
> 环境: WSL2 Ubuntu 26.04 + OHOS SDK 15.0.4

---

## 环境准备

```bash
# OHOS SDK (必须)
export OHOS_SDK=/apps/harmony/sdk/default/openharmony
export TOOL_HOME=/apps/harmony
export PATH=$TOOL_HOME/bin:$TOOL_HOME/tool/node/bin:$PATH

# 工具链
CLANG=$OHOS_SDK/native/llvm/bin/clang
SYSROOT=$OHOS_SDK/native/sysroot
```

---

## 1. 编译葡萄酒 x86_64 (OHOS musl)

### 1.1 Native 构建 (生成工具链 + PE DLLs)

```bash
cd thirdparty/wine
mkdir -p build-native && cd build-native

../configure --enable-win64 --disable-tests \
    --without-x --without-freetype --without-alsa \
    --without-opengl --without-vulkan

make -j$(nproc)
# 产物: tools/ wine PE DLLs
```

### 1.2 OHOS 交叉编译

```bash
mkdir -p build-ohos && cd build-ohos

# Configure (用 gcc 处理 PE, clang 后续用于 Unix 编译)
CC="gcc" ../configure \
    --host=x86_64-linux-ohos \
    --with-wine-tools=../build-native \
    --with-mingw=gcc \
    --disable-tests \
    --without-x --without-freetype --without-alsa \
    --without-opengl --without-vulkan

# 修复 config.h (移除 OHOS 不存在的头文件)
sed -i 's/#define HAVE_LINUX_NTSYNC_H 1/\/* OHOS *\/\n#undef HAVE_LINUX_NTSYNC_H/' include/config.h
sed -i 's/#define HAVE_NETIPX_IPX_H 1/\/* OHOS *\/\n#undef HAVE_NETIPX_IPX_H/' include/config.h

# 编译全部 Unix .so
TARGET=x86_64-linux-ohos
make -k -j$(nproc) \
    CC="$CLANG --target=$TARGET --sysroot=$SYSROOT" \
    CFLAGS="-g -O2 -D__MUSL__ -D_GNU_SOURCE -DWINE_UNIX_LIB \
            -D_NTSYSTEM_ -D__WINESRC__ -DFAR= -D_ACRTIMP= -DWINBASEAPI= -DZ_SOLO \
            -fPIC -fasynchronous-unwind-tables" \
    LDFLAGS="-fuse-ld=lld --sysroot=$SYSROOT --target=$TARGET"

# 产物: build-ohos/server/wineserver, build-ohos/dlls/*/*.so
```

### 1.3 Wineserver 编译 (带 HarmonyOS 修复)

HarmonyOS 没有 /tmp/ 写权限，wineserver 需要加 `-D__ANDROID__` 让 socket 放到 `~/.wine/.wineserver/`:

```bash
# 手动编译 wineserver
CFLAGS="$CLANG --target=$TARGET --sysroot=$SYSROOT -D__MUSL__ -D_GNU_SOURCE \
    -DWINE_UNIX_LIB -D_NTSYSTEM_ -D__WINESRC__ -DFAR= -D_ACRTIMP= -DWINBASEAPI= -DZ_SOLO \
    -D__ANDROID__ -fPIC -I../include -I../include/wine -I../server -Ibuild-ohos/include"

mkdir -p /tmp/wine_server && cd /tmp/wine_server
for f in ../server/*.c; do $CLANG $CFLAGS -c -o $(basename $f .c).o $f; done

# musl compat stub (epoll_pwait2 不存在于 musl)
cat > musl_compat.c << 'EOF'
#define _GNU_SOURCE
#include <sys/epoll.h>
int epoll_pwait2(int fd, struct epoll_event *ev, int n,
                 const struct timespec *ts, const sigset_t *s)
{ errno=ENOSYS; return -1; }
EOF
$CLANG $CFLAGS -c -o musl_compat.o musl_compat.c

# 链接
$CLANG --target=$TARGET --sysroot=$SYSROOT -fuse-ld=lld -o wineserver *.o -lm
```

### 1.4 Wine 源码修改 (Box64 兼容)

Box64 在 OHOS ARM64 上检测到 39 位地址空间，wine 的 TEB 预分配 (`virtual_alloc_first_teb`) 需要简化:

```bash
# 文件: dlls/ntdll/unix/virtual.c
# 函数: virtual_alloc_first_teb()
# 修改: 用 anon_mmap_alloc() 替代 NtAllocateVirtualMemory()
```

---

## 2. 编译 Box64 for OHOS ARM64

```bash
# 依赖
export BOX64_SRC=/src/ohos/wineohos/.temp/box64
export PATCH_DIR=/path/to/box64-ohos-patches

# 应用 HarmonyOS patches
cd $PATCH_DIR
BOX64=$BOX64_SRC bash scripts/patches.sh

# CMake 配置
mkdir -p /tmp/box64_build && cd /tmp/box64_build
cmake $BOX64_SRC \
    -GNinja \
    -DCMAKE_TOOLCHAIN_FILE=$OHOS_SDK/native/build/cmake/ohos.toolchain.cmake \
    -DOHOS_ARCH=arm64-v8a \
    -DOHOS_PLATFORM=OHOS \
    -DCMAKE_BUILD_TYPE=Release \
    -DARM_DYNAREC=ON \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON

ninja box64
# 产物: /tmp/box64_build/box64
```

---

## 3. 组装 Wine 发行版

```bash
OUT=out/hnp_combined
rm -rf $OUT
mkdir -p $OUT/opt/honwine/bin $OUT/opt/honwine/lib/x86_64 $OUT/opt/honwine/share/wine/nls

# 所有二进制放在 bin/ (标准 Wine 布局)
cp build-ohos/loader/wine           $OUT/opt/honwine/bin/   # musl 版本
cp /tmp/wine_server/wineserver      $OUT/opt/honwine/bin/   # 含 __ANDROID__ 修复
cp /tmp/box64_build/box64           $OUT/opt/honwine/bin/   # ARM64 原生

# Wine PE DLLs + Unix .so (从 native build)
cp build-native/dlls/**/*.dll       $OUT/opt/honwine/bin/
cp build-ohos/dlls/**/*.so          $OUT/opt/honwine/bin/

# x86_64 musl 运行时
cp $SYSROOT/usr/lib/x86_64-linux-ohos/libc.so $OUT/opt/honwine/lib/x86_64/

# NLS 文件 (Unicode 支持)
cp build-native/nls/*.nls           $OUT/opt/honwine/share/wine/nls/

# 启动脚本
cat > $OUT/opt/honwine/bin/wine.sh << 'SCRIPT'
#!/bin/sh
DIR="$(cd "$(dirname "$0")" && pwd)"
export WINEPREFIX="${WINEPREFIX:-$HOME/.wine}"
export BOX64_LD_LIBRARY_PATH="$DIR:$DIR/../lib/x86_64"
exec "$DIR/box64" "$DIR/wine" "$@"
SCRIPT
chmod +x $OUT/opt/honwine/bin/wine.sh
```

---

## 4. HNP 打包

```bash
HNPCLI=$OHOS_SDK/../toolchains/hnpcli

cat > $OUT/hnp.json << 'EOF'
{"type":"hnp-config","name":"honwine","version":"0.1.0","install":{"links":[
  {"source":"./opt/honwine/bin/wine.sh","target":"wine"},
  {"source":"./opt/honwine/bin/box64","target":"box64"}
]}}
EOF

$HNPCLI pack -i $OUT -o out -n honwine -v 0.1.0
# 产物: out/honwine.hnp (~250MB)
```

---

## 5. HAP 打包 (HonWine)

```bash
cd HonWine

# 1. 放置 HNP
cp ../out/honwine.hnp entry/hnp/arm64-v8a/honwine.hnp

# 2. 构建 HAP
hvigorw assembleHap

# 3. 把 HNP 添加到 HAP
cd entry
zip -r ../entry/build/default/outputs/default/entry-default-unsigned.hap hnp
cd ..

# 4. 签名
python3 sign.py \
    entry/build/default/outputs/default/entry-default-unsigned.hap \
    entry/build/default/outputs/default/entry-default-signed.hap

# 5. 推送到设备并安装
hdc tconn 192.168.1.4:38879
hdc file send entry/build/default/outputs/default/entry-default-signed.hap /data/local/tmp/
hdc shell bm install -p /data/local/tmp/entry-default-signed.hap -r
hdc shell aa start -a EntryAbility -b app.hackeris.honwine
```

**签名文件**: `.ohos/` 目录 (从 DevEco Studio 导出) 和 `build-profile.json5` (签名路径指向 `./.ohos/`)

---

## 6. 设备端运行

HAP 安装后，HNP 自动提取到 `/data/service/hnp/honwine.org/honwine_0.1.0/opt/honwine/`:

```bash
cd /data/service/hnp/honwine.org/honwine_0.1.0/opt/honwine
export WINEPREFIX=/data/local/tmp/.wine
rm -rf $WINEPREFIX
./bin/box64 ./bin/wine ./bin/cmd.exe
```

---

## 关键编译标志

| 标志 | 用途 |
|------|------|
| `-D__MUSL__` | 指示 musl libc |
| `-D_GNU_SOURCE` | 启用 GNU 扩展 |
| `-DWINE_UNIX_LIB` | 避免 msvcrt 头冲突 |
| `-D_NTSYSTEM_` | 修复 ntuser.h include 顺序 |
| `-D__ANDROID__` | OHOS 上用 `~/.wine/` 替代 `/tmp/` |
| `-D__WINESRC__`, `-DFAR=`, `-D_ACRTIMP=`, `-DWINBASEAPI=`, `-DZ_SOLO` | Wine build 标准宏 |
| `-fPIC` | 位置无关代码 (.so 必需) |

## 已知源码修改

| 文件 | 修改 | 原因 |
|------|------|------|
| `dlls/ntdll/unix/virtual.c:virtual_alloc_first_teb()` | 用 `anon_mmap_alloc()` 替代 `NtAllocateVirtualMemory()` | Box64 不支持 Wine 的复杂 VM 分配 |
| `dlls/ntdll/unix/server.c` (编译标志 `-D__ANDROID__`) | socket 路径 `~/.wine/` 替代 `/tmp/` | OHOS `/tmp` 只读 |
| Musl compat: `epoll_pwait2` | stub 返回 ENOSYS | musl 不提供 |

## 最终产物

```
out/
├── honwine.hnp          247MB   HNP 安装包 (含 Box64 + Wine + 依赖)
├── honwine-arm64.tar.gz 238MB   tar.gz 备用分发
├── box64-ohos-arm64      29MB   Box64 ARM64 二进制
├── wineserver            894K   x86_64 musl wineserver
└── hnp_combined/              HNP 构建临时目录
    └── opt/honwine/
        ├── bin/          750 文件  (wine, wineserver, box64, PE DLLs, .so)
        ├── lib/x86_64/   libc.so  (x86_64 musl 运行时)
        └── share/wine/nls/ 76 NLS 文件
```
