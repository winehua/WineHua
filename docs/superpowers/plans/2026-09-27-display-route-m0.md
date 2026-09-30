# 显示路线 M0 实现计划（wlroots 交叉构建 + Xwayland NCP 启动 + pixman 出图 + X client 端到端）

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在真机 192.168.1.6:33363 上跑通 `X client → Xwayland(NCP 子进程) → wlroots(app 进程, headless+pixman) → NativeWindow` 全链出图，即 spec §7 M0 出口。

**Architecture:** wlroots 钉 0.20.2 上游 tag（仅 1 处 `server.c` 启动点补丁），Xwayland 由 NCP 子进程承载（fd 经 broker spawn 协议下发，禁 fork/exec），自定义 `wlr_allocator` 出 OH_NativeBuffer 帧、pixman 合成后直推 NativeWindow。所有新库交叉构建进 `build/sysroot-ext/`，由 `make deps` 统一驱动。

**Tech Stack:** meson 交叉构建（复用 `scripts/env.sh` 的 `meson_build()`）、OHOS NDK clang、OHOS NCP（`OH_Ability_CreateNativeChildProcess`）、OH_NativeBuffer/NativeWindow NDK。

**Spec:** [docs/superpowers/specs/2026-09-27-display-route-x11-wlroots-design.md](../specs/2026-09-27-display-route-x11-wlroots-design.md)（本计划从 spec §5 风险表、§6 构建清单、§7 M0 行展开；执行者需同时持有两文）

## Global Constraints

- 子进程一律走 NCP（平台硬约束，用户 2026-09-27 定案），**任何代码不得 fork/exec**；`wlr_xwayland_server_create` 的 fork/execvp 路径必须被补丁替换
- wayland 库版本 ≥1.26.0（wlroots 0.20.2 要 server/client ≥1.24，xserver 要 client ≥1.26）；wlroots 钉 `0.20.2`，构建集合 `-Dauto_features=disabled -Dxwayland=enabled`
- 所有交叉构建只进 `$SYSROOT_EXT`（`build/sysroot-ext/`），头/.pc 随装随验（`pkg-config --modversion` 断言），禁止手工伪造 .pc（R-REPRO 教训：`build_ohos_guest_gfx.sh:589-601` 的伪 pc 是隐藏依赖源头）
- 构建脚本一律 `set -euo pipefail`、分阶段 log、产物断言失败即非零退出（`.claude/rules/reproducibility.md`）
- 每个新脚本完成后做一次毁灭性重建：`rm -rf build/sysroot-ext && make deps` 全绿
- 改动验证范围跟随变更范围：wayland 升级后必须 `make NATIVE_ARCH=arm64-v8a` + core 套件回归（`.claude/rules/smoke-regression.md`）
- 提交即推送（主仓库 + submodule 一视同仁）；功能分支主仓库与 submodule 同名 `feature/display-route-m0`（wlroots/xserver 的钉版分支同样用此名，分支内容 = 钉版 tag + 各自的 OHOS 补丁提交）
- 联网步骤（submodule add / tarball 下载）按需 `export https_proxy=http://192.168.1.2:7897`（npm 例外，直连）
- 设备 192.168.1.6:33363 为调试机；部署用 `bash scripts/package.sh deploy 192.168.1.6`；沙箱文件读取用 `hdc file recv -b`（`.claude/rules/build-and-log.md`）

## Review Focus

以下失败模式 spec 未被用例覆盖，执行时必须被对应任务的门钉住：

1. **.so 文件名与 SONAME 错配**（wayland 升级后 `libwayland-client.so.0.26.0` 之类硬拷文件名写错）→ Task 1 用 `llvm-readelf -d` 断言 SONAME，且试链接通过
2. **meson 交叉构建漏用 cross file 退化成 host 编译**（每个新脚本的第一颗雷）→ 每个构建任务都以 `llvm-readelf -h | grep AArch64` 断言产物架构
3. **fork/exec 假设混入 NCP 子进程**（Xwayland 内部 popen xkbcomp、任何 `system()/popen()` 调用）→ Task 7 shim 启动时显式探针 fork/exec 并把结果写 hilog + 回填 spec（决定 M1 的 R-xkb 形态）
4. **NativeWindow 直推的 buffer usage/格式不被接受**（CPU 写入 buffer 需要 CPU_READ|CPU_WRITE 类 usage，格式需 NativeWindow 支持）→ Task 8 首验含 `OH_NativeWindow_NativeWindow*` 返回码检查与错误即失败
5. **sysroot-ext 半新半旧混用**（1.26 头配 1.22 库之类）→ Task 1 起每个任务脚本带 modversion 断言；全计划做 2 次毁灭性重建演练（Task 1、Task 10）

---

### Task 1: wayland 1.22.0 → 1.26.0 升级 + server 头/.pc 固化 + 重建演练

**Files:**
- Modify: `thirdparty/wayland`（submodule → tag 1.26.0，分支 `feature/display-route-m0`）
- Modify: `scripts/build_wayland.sh`
- Modify: `Makefile`（无需改，stamp 机制自动感知脚本变更；确认即可）

**Interfaces:**
- Produces: `$SYSROOT_EXT_LIB/libwayland-{client,server}.so.0`（SONAME 不变，`.0`）、`$SYSROOT_EXT_INC/wayland-{client,server}*.h`、`$SYSROOT_EXT_PC/wayland-{client,server,egl,protocols}.pc`——Task 2/3/5/6/7 全部经 `pkg-config` 消费
- 版本判据：`pkg-config --modversion wayland-server` ≥ `1.26.0`

- [ ] **Step 1: submodule 切分支钉 tag**

```bash
cd /home/rianm/workspace/WineHua/thirdparty/wayland
git fetch origin && git checkout -b feature/display-route-m0 1.26.0
git push -u origin feature/display-route-m0
cd /home/rianm/workspace/WineHua
```

- [ ] **Step 2: 改 scripts/build_wayland.sh（四处）**

1. 顶部注释与卫语句不变；`.so` 拷贝三行改为按构建产物动态取（**消灭写死版本号**）：

```bash
# 安装 .so (文件名 = SONAME; 版本尾缀随 tag 变, 用通配取实际产物并断言 SONAME)
install_soname() {  # $1=产物 glob  $2=目标 SONAME 名
    local src; src=$(ls "$1" 2>/dev/null | head -1)
    [ -n "$src" ] || { err "未找到产物: $1"; return 1; }
    cp "$src" "$SYSROOT_EXT_LIB/$2"
}
install_soname "$WL_BUILD/x86_64/src/libwayland-client.so.0.*" "libwayland-client.so.0"
install_soname "$WL_BUILD/x86_64/src/libwayland-server.so.0.*" "libwayland-server.so.0"
install_soname "$WL_BUILD/x86_64/egl/libwayland-egl.so.1.*"    "libwayland-egl.so.1"
ln -sf libwayland-client.so.0 "$SYSROOT_EXT_LIB/libwayland-client.so"
ln -sf libwayland-server.so.0 "$SYSROOT_EXT_LIB/libwayland-server.so"
ln -sf libwayland-egl.so.1    "$SYSROOT_EXT_LIB/libwayland-egl.so"
```

2. 头文件区补 **server 头**（R-REPRO 固化，wlroots 消费）：

```bash
cp "$WL_SRC/src/wayland-server.h" \
   "$WL_SRC/src/wayland-server-core.h" \
   "$WL_SRC/src/wayland-server-protocol.h" \
   "$WL_SRC/src/wayland-shm.h" \
   "$WL_BUILD/x86_64/src/wayland-version.h" \
   "$SYSROOT_EXT_INC/"
```

3. .pc 区补 **wayland-server.pc**（参照现有 wayland-client.pc 模板，`Libs: -L${libdir} -lwayland-server`，Version 写 `1.26.0`），并把 client/egl 两份 .pc 的 `Version:` 同步改 `1.26.0`
4. 卫语句清单补两项：`wayland-server.h` 与 `wayland-server.pc` 存在才允许"已就绪跳过"

- [ ] **Step 3: 构建并断言版本**

```bash
rm -rf build/sysroot-ext   # 毁灭性重建演练（R-REPRO）
make deps
```

断言（追加到 build_wayland.sh 末尾，失败非零退出）：

```bash
WLV=$(pkg-config --modversion wayland-server)
case "$WLV" in 1.2[6-9]*|1.[3-9]*) log "wayland-server=$WLV OK";; *) err "wayland 版本不达标: $WLV"; exit 1;; esac
"$CC" --target=aarch64-linux-ohos --sysroot="$SYSROOT" -I"$SYSROOT_EXT_INC" \
    -L"$SYSROOT_EXT_LIB" -fuse-ld=lld -shared -o /dev/null -x c - <<'EOF' 2>/dev/null <<< $'\n' || \
"$CC" --target=aarch64-linux-ohos --sysroot="$SYSROOT" -I"$SYSROOT_EXT_INC" -L"$SYSROOT_EXT_LIB" \
    -fuse-ld=lld -o "$BUILD_DIR/wl_smoke.o" -c - <<'EOF2'
#include <wayland-server-core.h>
int wl_probe(struct wl_display *d){ return wl_display_get_empty_serial(d); }
EOF2
EOF
```

（实现时以两条独立命令落地即可：一条编译 `wayland-server-core.h` 包含测试，一条链接 `-lwayland-server`；两条都必须 rc=0。）

- [ ] **Step 4: 断言 SONAME 与架构**

```bash
llvm-readelf -d build/sysroot-ext/usr/lib/x86_64-linux-ohos/libwayland-server.so.0 | grep SONAME
# 期望: SONAME: libwayland-server.so.0
llvm-readelf -h build/sysroot-ext/usr/lib/x86_64-linux-ohos/libwayland-server.so.0 | grep Machine
# 期望: AArch64
```

- [ ] **Step 5: 产品回归（wayland 是 entry 直链库，必须整机验证）**

```bash
make NATIVE_ARCH=arm64-v8a
bash scripts/package.sh deploy 192.168.1.6
python3 automation/smoke.py run --suite core
```

Expected: 构建绿、core 套件不回退（对齐最近一次基线）。

- [ ] **Step 6: 提交推送**

```bash
git add scripts/build_wayland.sh thirdparty/wayland
git commit -m "build(deps): wayland 1.22.0 → 1.26.0, 固化 server 头/.pc 入 build_wayland.sh (R-REPRO)"
git push origin feature/display-route-m0
```

---

### Task 2: 图形基础库交叉构建（pixman / libxkbcommon / libdrm / wayland-protocols 升版）

**Files:**
- Create: `scripts/build_display_libs.sh`
- Modify: `Makefile`（deps 目标追加调用 + stamp）
- Modify: `thirdparty/wayland-protocols`（submodule 升 ≥1.47，同 feature 分支）

**Interfaces:**
- Consumes: Task 1 的 wayland .pc
- Produces: `$SYSROOT_EXT` 内 `pixman-1`（≥0.43.0）、`xkbcommon`（≥1.8.0）、`libdrm`（≥2.4.129）的头/.pc/.so；`wayland-protocols.pc` 版本 ≥1.47——Task 3/5/6 消费

- [ ] **Step 1: 钉版本（联网步骤，取最新满足下限的稳定 tag，逐个断言下限）**

```bash
git -C thirdparty/wayland-protocols ls-remote --tags origin | grep -oE 'refs/tags/1\.[0-9]+$' | sort -V | tail -3
# 选 ≥1.47 最新, 切 feature/display-route-m0 分支, 推送
```

pixman/libxkbcommon/libdrm 用 pinned tarball + sha256（URL 写死进脚本，`curl -fsSL` 下载到 `build/downloads/`，`sha256sum -c` 校验后才解压；版本下限不满足即 `exit 1`）。

- [ ] **Step 2: 写 scripts/build_display_libs.sh**

骨架（三个库同构，全部走 `meson_build`，**不得**绕过 cross file；卫语句 + 逐库 modversion 断言 + 末尾汇总）：

```bash
#!/bin/bash
# build_display_libs.sh — 显示路线基础库: pixman + libxkbcommon + libdrm → sysroot-ext
# 用法: make deps 时被 Makefile 调用; 也可独立 bash scripts/build_display_libs.sh
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/env.sh"

DL="$BUILD_DIR/downloads"; mkdir -p "$DL"
fetch_and_unpack() {  # $1=url $2=sha256 $3=解压目录名
    local tgz="$DL/$(basename "$1")"
    [ -f "$tgz" ] || curl -fsSL -o "$tgz" "$1"
    echo "$2  $tgz" | sha256sum -c - || { err "sha256 不匹配: $tgz"; exit 1; }
    [ -d "$BUILD_DIR/$3" ] || tar -C "$BUILD_DIR" -xf "$tgz"
}

# --- pixman (wlroots pixman 渲染器 + Xwayland 依赖, ≥0.43.0) ---
if ! pkg-config --exists pixman-1; then
    fetch_and_unpack "https://www.cairographics.org/releases/pixman-<钉版>.tar.gz" "<sha256>" "pixman-<钉版>"
    meson_build "$BUILD_DIR/pixman_build" "$BUILD_DIR/pixman-<钉版>" -Dtests=disabled -Ddemos=disabled
    ninja -C "$BUILD_DIR/pixman_build" install
fi
[ "$(pkg-config --modversion pixman-1)" \> "0.42.99" ] || { err "pixman < 0.43.0"; exit 1; }

# --- libxkbcommon (wlroots 键盘, ≥1.8.0; xkbregistry 关闭) ---
if ! pkg-config --exists xkbcommon; then
    fetch_and_unpack "https://github.com/xkbcommon/libxkbcommon/archive/refs/tags/xkbcommon-<钉版>.tar.gz" "<sha256>" "libxkbcommon-xkbcommon-<钉版>"
    meson_build "$BUILD_DIR/xkbcommon_build" "$BUILD_DIR/libxkbcommon-xkbcommon-<钉版>" \
        -Denable-xkbregistry=false -Denable-docs=false -Denable-tools=disabled
    ninja -C "$BUILD_DIR/xkbcommon_build" install
fi
[ "$(pkg-config --modversion xkbcommon)" \> "1.7.99" ] || { err "xkbcommon < 1.8.0"; exit 1; }

# --- libdrm (wlroots 无条件依赖, ≥2.4.129; 只需头与 .pc, 全功能可编) ---
if ! pkg-config --exists drm; then
    fetch_and_unpack "https://dri.freedesktop.org/libdrm/libdrm-<钉版>.tar.xz" "<sha256>" "libdrm-<钉版>"
    meson_build "$BUILD_DIR/drm_build" "$BUILD_DIR/libdrm-<钉版>" -Domap=disabled -Dintel=disabled \
        -Dradeon=disabled -Damdgpu=disabled -Dnouveau=disabled -Dvmwgfx=disabled -Dtests=false
    ninja -C "$BUILD_DIR/drm_build" install
fi
[ "$(pkg-config --modversion drm)" \> "2.4.128" ] || { err "libdrm < 2.4.129"; exit 1; }

log "display libs OK: pixman=$(pkg-config --modversion pixman-1) xkbcommon=$(pkg-config --modversion xkbcommon) drm=$(pkg-config --modversion drm)"
```

注意：libxkbcommon 的 xkb_data 路径经 `-Dxkb-config-root` 指到 `$SYSROOT_EXT_SHARE/X11/xkb`（Task 1 确认 `build/sysroot-ext/usr/share/X11/xkb` 已存在）；若 meson 探测缺 xkeyboard-config，用该参数显式指定。

- [ ] **Step 3: Makefile deps 挂载（DXVK_STAMP 同款）**

```make
DISPLAY_LIBS_STAMP := $(BUILD_DIR)/.stamps/display-libs
$(DISPLAY_LIBS_STAMP): $(SCRIPTS)/build_display_libs.sh | $(STAMPS)
	bash $(SCRIPTS)/build_display_libs.sh
	@touch $@
```

并加入 `deps` 目标依赖序列（wayland 之后）。

- [ ] **Step 4: 门——毁灭性重建 + 全量断言**

```bash
rm -rf build/sysroot-ext && make deps
for p in pixman-1 xkbcommon drm wayland-server wayland-client wayland-protocols; do
  pkg-config --modversion $p || exit 1
done
llvm-readelf -h build/sysroot-ext/usr/lib/x86_64-linux-ohos/libpixman-1.so | grep -q AArch64
```

Expected: 四库 modversion 全部打印且达下限；`make deps` rc=0。

- [ ] **Step 5: 提交推送**（脚本 + Makefile + wayland-protocols 指针，message: `build(deps): 显示路线基础库 pixman/xkbcommon/libdrm 交叉构建, wayland-protocols 升 ≥1.47`）

---

### Task 3: X 协议头与 libxcb 栈（wlroots xwayland 与 X client 的共同前置）

**Files:**
- Create: `scripts/build_xcb_stack.sh` + Makefile stamp

**Interfaces:**
- Produces: `xorgproto`（头）、`xcb-proto`（数据）、`libxcb` 及 wlroots 实际需要的 xcb 辅助模块 `.so/.pc`

- [ ] **Step 1: 从钉版源码枚举依赖清单（不凭记忆）**

```bash
grep -n "dependency('" .temp/gamescope/subprojects/wlroots/xwayland/meson.build .temp/gamescope/subprojects/wlroots/meson.build | grep -i xcb
```

记录 wlroots 要求的 xcb 模块全集（预期含 `xcb`、`xcb-shm`、`xcb-render`、`xcb-composite`、`xcb-res`、`xcb-xfixes`、`xcb-icccm`(xcb-util-wm)、`xcb-image`/`xcb-keysyms`(xcb-util, 以实际 grep 为准)）。

- [ ] **Step 2: 写脚本构建**（autotools 序列；每个库用同一套 OHOS configure 模板）

xorgproto 与 xcb-proto 只装数据/头（`./configure --prefix=$SYSROOT_EXT/usr`，无编译）；libxcb 需要 host 侧 `xcb-proto` + python；xcb 辅助库（xcb-util 系列）逐个 `--host` 交叉 configure。OHOS configure 模板：

```bash
conf_ohos() {  # $1=源码目录
    ( cd "$1" && ./configure --host=aarch64-linux-ohos --prefix="$SYSROOT_EXT/usr" \
        CC="$CC" CXX="$CXX" LD="$LD" \
        CFLAGS="--target=$TARGET --sysroot=$SYSROOT -I$SYSROOT_EXT_INC" \
        LDFLAGS="--target=$TARGET --sysroot=$SYSROOT -fuse-ld=lld -L$SYSROOT_EXT_LIB" \
        PKG_CONFIG_PATH="$SYSROOT_EXT_PC:$SYSROOT_EXT/usr/lib/x86_64-linux-ohos/pkgconfig" \
        --disable-static --enable-shared )
}
```

（`$CC/$CXX/$LD/$TARGET` 均来自 env.sh；若 env.sh 变量名不同以 env.sh 为准并在脚本注释记录映射。）

- [ ] **Step 3: 门**——逐模块 `pkg-config --modversion xcb` 等可查 + 试编译：

```bash
printf '#include <xcb/xcb.h>\n#include <xcb/xcb_icccm.h>\nint main(){return 0;}\n' > /tmp/x.c && \
$CC --target=$TARGET --sysroot=$SYSROOT -I$SYSROOT_EXT_INC -L$SYSROOT_EXT_LIB -fuse-ld=lld \
    /tmp/x.c $(pkg-config --cflags --libs xcb xcb-icccm) -o /dev/null
```

Expected: rc=0。产物 `llvm-readelf -h` 断言 AArch64。毁灭性重建一次（`rm -rf build/sysroot-ext && make deps`）。

- [ ] **Step 4: 提交推送**（`build(deps): xorgproto/xcb-proto/libxcb+xcb-util 栈交叉构建`）

---

### Task 4: libX11 + libXext（X client 硬依赖，即 winex11 的硬依赖面）

**Files:**
- Create: `scripts/build_x11_client.sh` + Makefile stamp

**Interfaces:**
- Consumes: Task 3 的 libxcb/xorgproto
- Produces: `libX11.so.6`、`libXext.so.6` + `.pc`（Task 7 mini client 直接链接；这正是 spec §3 winex11 行的硬依赖清单）

- [ ] **Step 1: 构建**（libX11 需 xtrans——libX11 tarball 自带；`--disable-udc --disable-xlocale` 收窄；libXext 常规）+ libX11 的 keysymdef 数据来自 xorgproto（Task 3 已装）。
- [ ] **Step 2: 门**

```bash
pkg-config --modversion x11 xext
llvm-readelf -h build/sysroot-ext/usr/lib/x86_64-linux-ohos/libX11.so.6 | grep -m1 Machine  # AArch64
printf '#include <X11/Xlib.h>\n#include <X11/Xutil.h>\nint main(){return 0;}\n' > /tmp/xx.c && \
$CC --target=$TARGET --sysroot=$SYSROOT -I$SYSROOT_EXT_INC -L$SYSROOT_EXT_LIB -fuse-ld=lld \
    /tmp/xx.c $(pkg-config --cflags --libs x11 xext) -o /dev/null
```

Expected: rc=0，架构断言 AArch64。提交推送（`build(deps): libX11/libXext 交叉构建`）。

---

### Task 5: wlroots 0.20.2 submodule + 最小集合交叉构建

**Files:**
- Create: `thirdparty/wlroots`（submodule → tag 0.20.2，默认分支 `ohos/0.20.2`，推送远程）
- Create: `scripts/build_wlroots.sh` + Makefile stamp

**Interfaces:**
- Consumes: Task 1 wayland-server ≥1.26、Task 2 pixman/xkbcommon/drm/wayland-protocols、Task 3 xcb 栈
- Produces: `$SYSROOT_EXT_LIB/libwlroots.a`（静态）+ `$SYSROOT_EXT_INC/wlr/**` 头——Task 7/8 链接消费

- [ ] **Step 1: submodule 钉版**

```bash
export https_proxy=http://192.168.1.2:7897
git submodule add https://gitlab.freedesktop.org/wlroots/wlroots.git thirdparty/wlroots
cd thirdparty/wlroots && git checkout -b feature/display-route-m0 0.20.2 && git push -u origin feature/display-route-m0 && cd -
git add thirdparty/wlroots   # 主仓库指针（Task 7 会在此分支提交启动补丁）
```

- [ ] **Step 2: 写 scripts/build_wlroots.sh**（关键构建项，其余卫语句/断言同前）：

```bash
meson_build "$BUILD_DIR/wlroots_build" "$ROOT/thirdparty/wlroots" \
    -Dauto_features=disabled \
    -Dxwayland=enabled \
    -Ddefault-library=static \
    -Dexamples=false
ninja -C "$BUILD_DIR/wlroots_build"
# 安装: 静态库 + 头 + 生成协议头
cp "$BUILD_DIR/wlroots_build/libwlroots.a" "$SYSROOT_EXT_LIB/"
meson install -C "$BUILD_DIR/wlroots_build" --destdir "$BUILD_DIR/wlroots_dest"
cp -r "$BUILD_DIR"/wlroots_dest/usr/local/include/wlr "$SYSROOT_EXT_INC/"
```

- [ ] **Step 3: 门**

```bash
llvm-readelf -h build/sysroot-ext/usr/lib/x86_64-linux-ohos/libwlroots.a | grep -m1 Machine  # AArch64
llvm-nm build/sysroot-ext/usr/lib/x86_64-linux-ohos/libwlroots.a | grep -E "T wlr_headless_backend_create|T wlr_seat_create|T wlr_xwayland_create|pixman_renderer_create"
```

Expected: 符号齐备（pixman 渲染器入口以 `include/wlr/render/pixman.h` 实际签名为准，构建前先 `grep "wlr_.*renderer_create" thirdparty/wlroots/include/wlr/render/pixman.h` 记录）。试链接：编译引用上述符号的 `main` 并 `-lwlroots -lwayland-server -lpixman-1 ...` rc=0。

- [ ] **Step 4: 提交推送**（`build(deps): wlroots 0.20.2 钉版 OHOS 交叉构建(最小集合, 静态)`；主仓库指针与 submodule 分支同步推）

---

### Task 6: Xwayland 钉版裁决 + libxwayland 共享库构建

**Files:**
- Create: `thirdparty/xserver`（submodule → 裁决后的 tag，默认分支 `ohos/<tag>`）
- Create: `scripts/build_xwayland.sh` + Makefile stamp
- Modify: xserver 的 `hw/xwayland/meson.build`（submodule 内提交：新增 shared_library 目标，与可执行文件同源对象）

**Interfaces:**
- Consumes: Task 1-5 全部产物（wayland-client ≥1.26、pixman、xtrans/libxau/libxdmcp/libxfont2/libxshmfence/libxcvt、xorgproto、xcb 栈、xkbcomp 数据树）
- Produces: `$SYSROOT_EXT_LIB/libxwayland.so`（导出 `main` 或等价入口符号）——Task 7 shim dlopen/直链调用

- [ ] **Step 1: 钉版裁决（输出证据表回填 spec §6）**

对候选 `xwayland-24.1.13`（稳定系最新，tag 已确认存在）与主线 `26.1.99.1`（.temp 已有）各做五项 grep，写进提交与 spec：①根 meson.build 的 `wayland_req`；②`-shm` 选项存在（hw/xwayland）；③`xwayland_shell_v1` bind（xwayland-screen.c）；④`-force-xrandr-emulation` 选项存在；⑤xkbcomp 路径（ddxLoad.c）。默认推荐 **24.1.13**（行为面与 spec §3/§4.4 引证同代、风险最小），若五项有任何一项缺失则回到主线比较。submodule checkout 对应 tag、切 `feature/display-route-m0` 分支推送。

- [ ] **Step 2: Xwayland 伴生库构建**（libxau、libxdmcp、xtrans、libxfont2[链接 sysroot-ext 既有 libfreetype]、libxshmfence、libxcvt；autotools `conf_ohos` 模板同 Task 3；libxfont2 若需 fontenc/unicode 数据按 configure 输出收窄 `--disable-devel-docs --without-xmlto`）

- [ ] **Step 3: libxwayland.so 构建改造**（submodule 内 meson 补丁，一个提交）

`hw/xwayland/meson.build` 末尾追加（保持可执行目标不动，两者并存）：

```meson
# OHOS: NCP 子进程无法 exec 可执行文件, 以共享库形态承载 Xwayland (spec R-SPAWN)
shared_library('xwayland_ohos',
    xwayland_sources,           # 与可执行目标同一源列表变量, 以文件内实际变量名为准
    include_directories: [inc, ..],   # 复制可执行目标的 include_directories/d链
    dependencies: [..],               # 复制可执行目标的 dependencies
    name_prefix: 'lib', install: false)
```

（执行时以该文件内可执行目标的实际变量名展开，禁止照抄省略号——把可执行目标块的对应行原样复制。）

- [ ] **Step 4: 构建脚本 + 门**

```bash
meson_build "$BUILD_DIR/xwayland_build" "$ROOT/thirdparty/xserver" \
    -Dxkb_output_dir="$SYSROOT_EXT/tmp/xkb" \
    -Dxkb_dir="$SYSROOT_EXT_SHARE/X11/xkb" \
    -Dglamor=false -Dxwayland=true -Dxorg=false -Ddocs=false \
    <按 meson configure 输出继续关闭无关后端, 直到依赖闭包最小>
ninja -C "$BUILD_DIR/xwayland_build" hw/xwayland/libxwayland_ohos.so
cp "$BUILD_DIR/xwayland_build/hw/xwayland/libxwayland_ohos.so" "$SYSROOT_EXT_LIB/"
```

门：`llvm-nm .../libxwayland_ohos.so | grep " T main"` 命中；`llvm-readelf -h` AArch64；构建期记录 Xwayland 实际 `-xkbdir`/XKB 数据路径假设写进脚本注释（R-xkb 运行时配置点）。

- [ ] **Step 5: 提交推送**（xserver submodule 两个提交：钉版分支 + meson 补丁；主仓库 `build(deps): Xwayland 钉版 + libxwayland 共享库构建` + spec §6 回填裁决表）

---

### Task 7: NCP shim + wlroots 启动补丁 + 真机 Xwayland bring-up

**Files:**
- Create: `entry/src/main/cpp/display/ncp/xwayland_child.cpp`（NCP shim，编为 `libxwayland_child.so`）
- Modify: `entry/src/main/cpp/CMakeLists.txt`（新增 `libxwayland_child` 目标，形态同 `virgl_child`）
- Modify: `thirdparty/wlroots/xwayland/server.c`（启动点补丁，submodule 分支 `ohos/0.20.2` 新提交）
- Modify: `entry/src/main/cpp/display/display_compositor.cpp`（app 侧 glue：wl_event_loop/wl_display + headless backend + wlr_xwayland 启动 + NCP spawn 调用）

**Interfaces:**
- Consumes: broker spawn 的 fd 下发机制（`broker.cpp:184` "FDS 行" 命名 fd 链表——实现前先读 `broker.cpp:150-250` 与 `proc/ncp_dispatch.cpp` 确认协议字段，fd 名建议 `x_fd0/x_fd1/wl_fd/wm_fd/displayfd`）
- Produces: 真机上 wlroots 日志出现 Xwayland ready（`wlr_xwayland` ready 标志 / displayfd 读到 display 号）

- [ ] **Step 1: 写 wlroots 启动点补丁**

`server.c` 中 `server_start()`：保留 fd 创建（`x_fd/wl_fd/wm_fd/displayfd` 的 socketpair/listening 建立），把 `safe_fork()` + `exec_xwayland()` 分支替换为「标记 fd 待下发 + 由外部经 NCP 回调注入」，对外新增一个注入函数：

```c
// server.c 新增公开入口 (xwayland/server.h 同步声明):
// 由 NCP shim 在子进程内调用: 收到父进程下发的 fd 后原地代 exec_xwayland 的职责
void wlr_xwayland_server_ohos_child_main(struct wlr_xwayland_server *server,
        int x_fd0, int x_fd1, int wl_fd, int wm_fd, int display_fd);
// 父进程侧: spawn 回调里把上述 fd 经 broker 协议写出去, 之后照旧读 displayfd/握 wl_fd/wm_fd
```

实现时：子进程路径复制 `exec_xwayland` 的 cloexec 处理与 argv 构造，但不 exec，改为 `setenv` + 直接调用 libxwayland 导出的 `main(argc, argv)`（`argv` 按 `server.c:48-100` 现有构造逻辑原样生成，含 `-listenfd/-displayfd/-wm/-rootless/-shm`）。补丁范围**仅限 server.c + server.h**，XWM/渲染/协议逻辑零触碰。

- [ ] **Step 2: 写 shim（xwayland_child.cpp）**

骨架（NCP 入口形态 = `virgl_child.cpp:471`）：

```cpp
// 入口三件套同 virgl_child: Main(NativeChildProcess_Args) / NativeChildProcess_OnConnect / NativeChildProcess_MainProc
// Main: 1) 起 abstract unix socket (名字: winehua_xwayland_fdchan_<pid>) 监听;
//       2) hilog 报 ready; 3) accept 后收 SCM_RIGHTS 五连 fd;
//       4) extern "C" int main(int, char**) 由 libxwayland.so 导入, 构造 argv 调用之。
// 探针(启动时一次): fork() 返回值与 execve("/proc/self/exe") 结果写 hilog,
//   标签 XWAYLAND-NCP —— 结果回填 spec, 决定 M1 R-xkb (xkbcomp) 形态。
```

fd 通道协议优先复用 broker 既有 FDS 下发（若 broker 支持任意 fd 列表）；不可行则用上述 SCM_RIGHTS 通道（P4 已证 abstract 可用）。二选一的依据与结论写进代码注释。

- [ ] **Step 3: app 侧 glue（display_compositor.cpp 最小版）**

`wl_display_create` → `wlr_backend` 用 `wlr_headless_backend_create`（签名以 `include/wlr/backend/headless.h` 为准）→ `wlr_xwayland_create`（或 `wlr_xwayland_server_create` + `create_with_server` 二段式，以补丁后 API 为准）→ spawn NCP `libxwayland_child.so`（`OH_Ability_CreateNativeChildProcess`，形态同 `graphics_broker.cpp:1321`）→ fd 下发 → 等 ready。触发方式：走 smoke_napi 增加一个调试命令（测试设施不进产品路径，`.claude/rules/principles.md` #22）。

- [ ] **Step 4: 构建 + 真机 bring-up 门**

```bash
make NATIVE_ARCH=arm64-v8a hap && bash scripts/package.sh deploy 192.168.1.6
hdc -t 192.168.1.6 shell "aa start -a EntryAbility -b app.hackeris.winehua"
: > .temp/m0-bringup.log
hdc -t 192.168.1.6 hilog 2>/dev/null | grep --line-buffered -E "XWAYLAND-NCP|DisplayRoute|wlr_" >> .temp/m0-bringup.log &
# 触发 smoke 调试命令, 观察 30s
```

门（全部满足才算过）：hilog 出现 `XWAYLAND-NCP ready`；五连 fd 收齐；wlroots 日志无 `failed to exec`/`waitpid` 错误；`wlr_xwayland` ready/display 号就绪；Xwayland 进程在 `hidumper` 进程表存活 >60s 不退出。fork/exec 探针结果记录在案。

- [ ] **Step 5: 提交推送**（wlroots submodule 补丁提交 + 主仓库 shim/glue 提交；wlroots 指针更新推送）

---

### Task 8: 自定义 wlr_buffer/wlr_allocator + pixman 出图直推 NativeWindow

**Files:**
- Create: `entry/src/main/cpp/display/ohos_buffer.cpp/.h`（自定义 `wlr_buffer_impl` 包 OH_NativeBuffer + `wlr_allocator_interface` 实现）
- Modify: `entry/src/main/cpp/display/display_compositor.cpp`（headless output 创建 + allocator 挂接 + output commit 回调直推 NativeWindow）

**Interfaces:**
- Consumes: Task 5 libwlroots.a（`include/wlr/interfaces/wlr_buffer.h` 5 函数指针 impl、`include/wlr/render/allocator.h` 2 函数指针 interface、`backend/headless.h:25` add_output）
- Produces: 真机屏幕上出现测试图案；`output commit` 每帧回调带帧校验和

- [ ] **Step 1: 写 buffer/allocator**（要点：`OH_NativeBuffer_Alloc`（`native_buffer.h:176`）配置 `width/height/format(NAKNativeBuffer PIXEL_FMT_RGBA_8888)/usage`；`OH_NativeBuffer_Map`（`:233`）实现 `begin_data_ptr_access`；`get_dmabuf` 返回失败（合法，pixman 走 data_ptr 门 `render/pixman/renderer.c:251`）；usage 需含 CPU 读写位——以首验返回码为准调整，错误码记进注释）
- [ ] **Step 2: headless output + commit → NativeWindow**（output 尺寸 800×600 起步；commit 回调里 `OH_NativeWindow_*` 直推该 buffer，API 以 SDK `native_window.h` 实际签名为准；首验失败矩阵：换 usage 组合/换 flush API，逐次记录）
- [ ] **Step 3: 真机门**：屏幕可见稳定测试图案（渐变+边框），hilog 每 commit 打 `commit seq=%d crc=%x`；连续 300 帧 crc 稳定无错误码。人工过屏（`.claude/rules/principles.md` #21：涉画面改动必须人工过一遍）。
- [ ] **Step 4: 提交推送**（`feat(display): M0 出图链——OH_NativeBuffer wlr_buffer/allocator + pixman 直推 NativeWindow`）

---

### Task 9: X client 端到端（M0 出口验收）

**Files:**
- Create: `entry/src/main/cpp/display/ncp/xclient_child.cpp`（mini X client：libX11+libXext ONLY，NCP 形态同 Task 7 shim；连接 abstract `:0`，建窗、XStoreName、XPutImage 渐变、XMapWindow、事件循环处理 Expose）
- Modify: `entry/src/main/cpp/CMakeLists.txt`（`libxclient_child` 目标，链 libX11/libXext）

**Interfaces:**
- Consumes: Task 4 libX11/libXext、Task 7 的 Xwayland 运行态、Task 8 出图链
- Produces: 屏幕上一个由真实 X client 绘制的窗口 = spec §7 M0 出口（「Xwayland 跑 xterm」的等价最小件，见下方说明）

> **范围决议（2026-09-27 用户定案）**：M0 出口件 = mini client（依赖面恰 = winex11 硬依赖 libX11+libXext，构建栈即最终栈）。spec 的「Xwayland 跑 xterm」由 mini client 等价承担——xterm 全栈（libXt/libXaw/libXmu/libXpm/libICE/libSM/ncurses 7 库）无输入链时也只能看不能敲，留作 M1 可选延伸。

- [ ] **Step 1: 写 mini client + 部署**
- [ ] **Step 2: 真机门（M0 出口判据）**

```bash
# 顺序: 起 compositor → 起 Xwayland → 起 mini client
hilog 断言: X client connect OK / window mapped / Xwayland 无 error
屏幕判据:   真机屏幕可见 mini client 窗口内容(独立于 wlroots 测试图案的另一图案)
存活判据:   全链进程(Xwayland NCP + mini client NCP + app) 5 分钟无崩溃(hidumper 确认)
```

- [ ] **Step 3: 提交推送**（`feat(display): M0 出口——X client 端到端出图`）

---

### Task 10: M0 收尾——结论回填 + 终验

- [ ] **Step 1: 结论回填 spec**（§6 钉版裁决表、§5 R-SPAWN 实测结论、R-xkb 前瞻探针结果、NativeWindow 首验 usage/格式结论、M0 出口状态；按 spec 头部证据纪律写）
- [ ] **Step 2: 终验门**

```bash
rm -rf build/sysroot-ext && make deps && make NATIVE_ARCH=arm64-v8a   # 毁灭性重建全绿
python3 automation/smoke.py run --suite core        # 旧链路不回退
python3 automation/smoke.py run --suite wine-vulkan # 现行图形链不回退
```

- [ ] **Step 3: 推送全部分支与 submodule 指针，向用户交 M0 验收报告（过/未过项 + M1 入口建议）**
