# Makefile — Wine for HarmonyOS 构建编排
#
# 用法:
#   make                                          # 默认: x86_64 全量构建
#   make NATIVE_ARCH=x86_64
#   make NATIVE_ARCH=arm64-v8a
#
#   单个模块: make deps | wine | fex | box64 | box64-wow64 | native | assemble | hap
#   清理:     make clean

ROOT := $(realpath $(dir $(lastword $(MAKEFILE_LIST))))
.DEFAULT_GOAL := all

# ── 配置 ──
NATIVE_ARCH ?= x86_64
# guest 栈架构与 Wine 对齐: arm64 原生 wine → aarch64 venus/virgl guest (同架构 dlopen);
# x86_64 → x86_64 guest。(NATIVE_ARCH=all 已移除, 见下方 ARCHES 注释)
GUEST_ARCH ?= $(WINE_ARCH)
# guest gfx/vulkan 按架构构建 (mesa venus/virgl 交叉编译 aarch64|x86_64-linux-ohos);
# 需要 dlopen 的关键 guest 库由 assemble 复制到 entry/libs/<NATIVE_ARCH> (el1 bundle)
BUILD_GUEST_GFX ?= 1
BUILD_GUEST_VULKAN ?= 1
BUILD_WINE_MONO ?= 1
TARGET_SDK_VERSION ?= 6.1.0(23)
COMPATIBLE_SDK_VERSION ?= 6.1.0(23)
export NATIVE_ARCH
export GUEST_ARCH
export BUILD_GUEST_GFX
export BUILD_GUEST_VULKAN
export BUILD_WINE_MONO
export TARGET_SDK_VERSION
export COMPATIBLE_SDK_VERSION

# Wine 模拟层架构 (arm64 真机 → aarch64 原生 wine + FEX; x86_64 → x86_64 同目标)
WINE_ARCH ?= $(if $(filter arm64-v8a,$(NATIVE_ARCH)),aarch64,x86_64)
export WINE_ARCH

CONFIG    := $(NATIVE_ARCH)
BUILD_DIR := $(ROOT)/build
STAMPS    := $(BUILD_DIR)/.stamps
SCRIPTS   := $(ROOT)/scripts
DXVK_ARTIFACTS := \
	$(BUILD_DIR)/dxvk/legacy/x64/bin/d3d11.dll \
	$(BUILD_DIR)/dxvk/legacy/x64/bin/dxgi.dll \
	$(BUILD_DIR)/dxvk/legacy/x86/bin/d3d11.dll \
	$(BUILD_DIR)/dxvk/legacy/x86/bin/dxgi.dll
# 方案③ (aarch64): 追加 ARM64X 双图 DLL (FEX native view); stamp 按 WINE_ARCH 隔离
ifeq ($(WINE_ARCH),aarch64)
DXVK_ARTIFACTS += \
	$(BUILD_DIR)/dxvk/legacy/arm64x/bin/d3d11.dll \
	$(BUILD_DIR)/dxvk/legacy/arm64x/bin/dxgi.dll
endif
DXVK_STAMP := $(STAMPS)/dxvk-legacy-$(WINE_ARCH)
DXVK_SOURCE_INPUTS := $(shell find $(ROOT)/thirdparty/dxvk/src -type f 2>/dev/null; find $(ROOT)/thirdparty/dxvk -maxdepth 1 -type f 2>/dev/null)
# dxvk-modern 产物按 WINE_ARCH 分支:
#   方案③ (arm64 原生 wine + FEX): ARM64X 双图 DLL (FEX native view 执行);
#   方案①/②: 经典 x64/x86 转译版本 (meson cross)。
#   stamp 同样按 WINE_ARCH 隔离, 避免两方案互相吞 stamp。
ifeq ($(WINE_ARCH),aarch64)
DXVK_MODERN_ARTIFACTS := \
	$(BUILD_DIR)/dxvk/modern-2.6/arm64x/bin/d3d11.dll \
	$(BUILD_DIR)/dxvk/modern-2.6/arm64x/bin/dxgi.dll
DXVK_MODERN_BUILD := build_dxvk_modern_arm64x.sh
else
DXVK_MODERN_ARTIFACTS := \
	$(BUILD_DIR)/dxvk/modern-2.6/x64/bin/d3d11.dll \
	$(BUILD_DIR)/dxvk/modern-2.6/x64/bin/dxgi.dll \
	$(BUILD_DIR)/dxvk/modern-2.6/x86/bin/d3d11.dll \
	$(BUILD_DIR)/dxvk/modern-2.6/x86/bin/dxgi.dll
DXVK_MODERN_BUILD := build_dxvk_modern.sh
endif
DXVK_MODERN_STAMP := $(STAMPS)/dxvk-modern-2.6-$(WINE_ARCH)
DXVK_MODERN_SOURCE_INPUTS := $(shell find $(ROOT)/thirdparty/dxvk-modern/src -type f 2>/dev/null; find $(ROOT)/thirdparty/dxvk-modern -maxdepth 1 -type f 2>/dev/null)
VKD3D_PROTON_ARTIFACTS := \
	$(BUILD_DIR)/vkd3d-proton/limited-500k/x64/d3d12.dll \
	$(BUILD_DIR)/vkd3d-proton/limited-500k/x64/winehua-d3d12-smoke.exe \
	$(BUILD_DIR)/vkd3d-proton/limited-500k/x64/triangle.exe \
	$(BUILD_DIR)/vkd3d-proton/limited-500k/x64/gears.exe \
	$(BUILD_DIR)/vkd3d-proton/limited-500k/manifest.json
# 方案③ (aarch64): 追加 ARM64X d3d12.dll (FEX native view); stamp 按 WINE_ARCH 隔离
ifeq ($(WINE_ARCH),aarch64)
VKD3D_PROTON_ARTIFACTS += $(BUILD_DIR)/vkd3d-proton/limited-500k/arm64x/bin/d3d12.dll
endif
VKD3D_PROTON_STAMP := $(STAMPS)/vkd3d-proton-limited-500k-$(WINE_ARCH)
VKD3D_PROTON_SOURCE_INPUTS := $(shell find $(ROOT)/patches/vkd3d-proton -type f 2>/dev/null; \
	find $(ROOT)/thirdparty/vkd3d-proton -maxdepth 2 -type f 2>/dev/null)

# 架构列表 (NATIVE_ARCH=all 已移除: 单一 WINE_ARCH 无法同时满足 arm64 原生与
# box64+wine 两个 arm64 assemble, 双架构请分别 make NATIVE_ARCH=x86_64 / arm64-v8a)
ARCHES := $(NATIVE_ARCH)

# ── 关键产物 (用于验证构建是否完成) ──
DEPS_SENTINEL   := $(BUILD_DIR)/sysroot-ext/usr/lib/$(WINE_ARCH)-linux-ohos/libfreetype.so.6
WINE_SENTINEL   := $(BUILD_DIR)/wine-native/tools/winegcc/winegcc
GUEST_GFX_SENTINEL := $(BUILD_DIR)/guest_gfx/$(GUEST_ARCH)/winehua-guest-gfx.env
GUEST_VULKAN_SENTINEL := $(BUILD_DIR)/guest_vulkan/$(GUEST_ARCH)/manifest.json
WINE_MONO_SENTINEL := $(BUILD_DIR)/wine-ohos/share/wine/mono/wine-mono-11.1.0-x86.msi
HOST_VULKAN_SOURCE := $(ROOT)/smoke/venus_heaven_material_replay.c

# Guest runtime build scripts can also be invoked directly while iterating on
# Mesa/Venus. Track their manifests as assemble inputs so a subsequent
# `make hap` cannot silently reuse an older staged wine-data.zip.
ASSEMBLE_GUEST_INPUTS :=
ifeq ($(BUILD_GUEST_GFX),1)
ASSEMBLE_GUEST_INPUTS += $(wildcard $(GUEST_GFX_SENTINEL))
endif

# ============================================================
# dxvk — managed WineHua DXVK Legacy fork (x64 + x86)
# ============================================================
.PHONY: dxvk
dxvk: $(DXVK_STAMP)

$(DXVK_STAMP): $(SCRIPTS)/build_dxvk.sh $(SCRIPTS)/build_dxvk_arm64x.sh $(DXVK_SOURCE_INPUTS) | $(STAMPS)
	@echo "=== dxvk legacy ($(WINE_ARCH)) ==="
	bash $(SCRIPTS)/build_dxvk.sh
	@if [ "$(WINE_ARCH)" = "aarch64" ]; then bash $(SCRIPTS)/build_dxvk_arm64x.sh; fi
	touch $@

# DXVK is produced as a four-file side effect of the stamp recipe.  Give each
# packaged DLL an explicit rule so a clean checkout can resolve the assemble
# dependency before the stamp exists (the previous bare sentinel made CI stop
# with "No rule to make target .../d3d11.dll").  The size check also prevents
# packaging a partial or truncated DXVK install.
$(DXVK_ARTIFACTS): $(DXVK_STAMP)
	@test -s "$@" || { echo "ERROR: DXVK artifact missing after build: $@" >&2; exit 1; }

# ============================================================
# dxvk-modern — WineHua DXVK 2.6.2 compatibility profile (x64 + x86)
# ============================================================
.PHONY: dxvk-modern
dxvk-modern: $(DXVK_MODERN_STAMP)

$(DXVK_MODERN_STAMP): $(SCRIPTS)/$(DXVK_MODERN_BUILD) $(DXVK_MODERN_SOURCE_INPUTS) | $(STAMPS)
	@echo "=== dxvk modern 2.6 ($(WINE_ARCH)) ==="
	bash $(SCRIPTS)/$(DXVK_MODERN_BUILD)
	touch $@

$(DXVK_MODERN_ARTIFACTS): $(DXVK_MODERN_STAMP)
	@test -s "$@" || { echo "ERROR: DXVK Modern artifact missing after build: $@" >&2; exit 1; }
ifeq ($(BUILD_GUEST_VULKAN),1)
ASSEMBLE_GUEST_INPUTS += $(wildcard $(GUEST_VULKAN_SENTINEL))
endif

# ============================================================
# vkd3d-proton — x64-only, explicit, default-off 2.6 limited-500K profile
# ============================================================
.PHONY: vkd3d-proton
vkd3d-proton: $(VKD3D_PROTON_STAMP)

$(VKD3D_PROTON_STAMP): $(SCRIPTS)/build_vkd3d_proton.sh $(SCRIPTS)/build_vkd3d_proton_arm64x.sh $(VKD3D_PROTON_SOURCE_INPUTS) | $(STAMPS)
	@echo "=== vkd3d-proton 2.6 limited-500K ($(WINE_ARCH)) ==="
	bash $(SCRIPTS)/build_vkd3d_proton.sh
	@if [ "$(WINE_ARCH)" = "aarch64" ]; then bash $(SCRIPTS)/build_vkd3d_proton_arm64x.sh; fi
	touch $@

$(VKD3D_PROTON_ARTIFACTS): $(VKD3D_PROTON_STAMP)
	@test -s "$@" || { echo "ERROR: VKD3D-Proton artifact missing after build: $@" >&2; exit 1; }

# ============================================================
# 默认目标
# ============================================================
.PHONY: all
all: hap

# FORCE: 伪目标，永远"过期"，让 make 总是进入 recipe
# recipe 内部的 find -newer 才是真正的增量判断
.PHONY: FORCE
FORCE:

# 确保 stamps 目录存在
$(STAMPS):
	mkdir -p $(STAMPS)

# 确保架构子目录存在
$(STAMPS)/arm64-v8a $(STAMPS)/x86_64:
	mkdir -p $@

# ============================================================
# host-vulkan — native Host Vulkan exact replay diagnostic
# ============================================================
.PHONY: host-vulkan
host-vulkan: $(foreach a,$(ARCHES),$(STAMPS)/$(a)/host-vulkan)

define host_vulkan_rule
.PHONY: host-vulkan-$(1)
host-vulkan-$(1): $$(STAMPS)/$(1)/host-vulkan

$$(STAMPS)/$(1)/host-vulkan: $(SCRIPTS)/build_ohos_host_vulkan.sh $(SCRIPTS)/env.sh \
	$(HOST_VULKAN_SOURCE) FORCE | $$(STAMPS)/$(1)
	@manifest="$(BUILD_DIR)/host_vulkan/$(1)/manifest.json"; \
	module="$(BUILD_DIR)/host_vulkan/$(1)/lib/libwinehua_host_heaven_replay.so"; \
	if [ -f $$@ ] && [ -f "$$$$manifest" ] && [ -f "$$$$module" ] && \
	    ! [ "$(SCRIPTS)/build_ohos_host_vulkan.sh" -nt $$@ ] && \
	    ! [ "$(HOST_VULKAN_SOURCE)" -nt $$@ ]; then \
	    echo "  [host-vulkan/$(1)] up to date"; \
	else \
	    NATIVE_ARCH=$(1) bash $(SCRIPTS)/build_ohos_host_vulkan.sh && touch $$@; \
	fi
endef
$(foreach a,arm64-v8a x86_64,$(eval $(call host_vulkan_rule,$(a))))

# ============================================================
# deps — 交叉编译依赖 → build/sysroot-ext/ (架构无关)
# ============================================================
.PHONY: deps
deps: $(STAMPS)/deps

$(STAMPS)/deps: $(SCRIPTS)/build_deps.sh $(SCRIPTS)/build_gnutls.sh $(SCRIPTS)/build_gstreamer.sh \
	$(SCRIPTS)/build_ohos_guest_gfx.sh \
	$(SCRIPTS)/build_ohos_guest_vulkan.sh $(ROOT)/smoke/guest_vulkan_smoke.c \
	$(ROOT)/smoke/venus_sampled_image_probe.c \
	$(ROOT)/smoke/venus_depth_cube_probe.inc \
	$(ROOT)/smoke/venus_depth_cube_graphics_replay.inc \
	$(ROOT)/smoke/venus_fullscreen_triangle.vert \
	$(ROOT)/smoke/venus_heaven_material.vert \
	$(ROOT)/smoke/venus_heaven_material_replay.c \
	$(ROOT)/smoke/venus_depth_cube_golden.frag \
	$(ROOT)/smoke/venus_depth_cube_fail.spvasm \
	$(ROOT)/smoke/venus_storage_write.comp \
	$(ROOT)/smoke/venus_storage_read.comp \
	$(ROOT)/smoke/venus_image_fetch.comp \
	$(ROOT)/smoke/venus_combined_sample.comp \
	$(ROOT)/smoke/venus_dxvk_contract_sample.comp \
	$(ROOT)/smoke/venus_dxvk_contract_unknown_sample.comp \
	$(ROOT)/smoke/venus_dxvk_contract_spec_sample.comp \
	$(ROOT)/smoke/venus_dxvk_contract_vector_spec_sample.comp \
	$(ROOT)/smoke/venus_depth_array_compare.comp \
	$(ROOT)/smoke/venus_depth_cube_sample.comp \
	$(ROOT)/smoke/venus_depth_cube_compare.comp \
	$(ROOT)/smoke/venus_depth_cube_separated_compare.comp \
	$(ROOT)/smoke/venus_depth_cube_dxvk_contract_compare.spvasm \
	$(ROOT)/smoke/venus_depth_cube_array_sample.comp \
	$(ROOT)/smoke/venus_depth_cube_array_2d_compare.comp \
	$(ROOT)/smoke/venus_depth_cube_array_compare.comp \
	$(ROOT)/smoke/venus_spirv_replay.c \
	$(wildcard $(ROOT)/replay_spv/CS_*.remapped.spv) \
	$(ROOT)/smoke/venus_separated_sample.comp \
	$(SCRIPTS)/env.sh FORCE | $(STAMPS)
	@guest_gfx_ready=1; \
	if [ "$(BUILD_GUEST_GFX)" = "1" ] && [ ! -f "$(GUEST_GFX_SENTINEL)" ]; then \
	    guest_gfx_ready=0; \
	fi; \
	guest_vulkan_ready=1; \
	if [ "$(BUILD_GUEST_VULKAN)" = "1" ] && [ ! -f "$(GUEST_VULKAN_SENTINEL)" ]; then \
	    guest_vulkan_ready=0; \
	fi; \
	mono_ready=1; \
	if [ "$(BUILD_WINE_MONO)" = "1" ] && [ ! -s "$(WINE_MONO_SENTINEL)" ]; then \
	    mono_ready=0; \
	fi; \
	if [ -f $@ ] && [ -f $(DEPS_SENTINEL) ] && [ "$$guest_gfx_ready" = "1" ] && \
	    [ "$$guest_vulkan_ready" = "1" ] && [ "$$mono_ready" = "1" ] && \
	    ! [ "$(SCRIPTS)/build_ohos_guest_gfx.sh" -nt $@ ] && \
	    ! [ "$(SCRIPTS)/build_ohos_guest_vulkan.sh" -nt $@ ] && \
	    ! [ "$(SCRIPTS)/build_gnutls.sh" -nt $@ ] && \
	    ! [ "$(SCRIPTS)/build_gstreamer.sh" -nt $@ ] && \
	    ! [ "$(SCRIPTS)/build_deps.sh" -nt $@ ] && \
	    ! [ "$(SCRIPTS)/build_libffi.sh" -nt $@ ] && \
	    ! [ "$(SCRIPTS)/build_freetype.sh" -nt $@ ] && \
	    ! [ "$(SCRIPTS)/build_wayland.sh" -nt $@ ] && \
	    ! [ "$(SCRIPTS)/build_xkbcommon.sh" -nt $@ ] && \
	    ! [ "$(SCRIPTS)/build_xkbconfig.sh" -nt $@ ] && \
	    ! [ "$(ROOT)/smoke/guest_vulkan_smoke.c" -nt $@ ] && \
	    ! [ "$(ROOT)/smoke/guest_vulkan_smoke.c" -nt $@ ] && \
	    ! [ "$(ROOT)/smoke/venus_sampled_image_probe.c" -nt $@ ] && \
	    ! [ "$(ROOT)/smoke/venus_depth_cube_probe.inc" -nt $@ ] && \
	    ! [ "$(ROOT)/smoke/venus_depth_cube_graphics_replay.inc" -nt $@ ] && \
	    ! [ "$(ROOT)/smoke/venus_fullscreen_triangle.vert" -nt $@ ] && \
	    ! [ "$(ROOT)/smoke/venus_heaven_material.vert" -nt $@ ] && \
	    ! [ "$(ROOT)/smoke/venus_heaven_material_replay.c" -nt $@ ] && \
	    ! [ "$(ROOT)/smoke/venus_depth_cube_golden.frag" -nt $@ ] && \
	    ! [ "$(ROOT)/smoke/venus_depth_cube_fail.spvasm" -nt $@ ] && \
	    ! [ "$(ROOT)/smoke/venus_storage_write.comp" -nt $@ ] && \
	    ! [ "$(ROOT)/smoke/venus_storage_read.comp" -nt $@ ] && \
	    ! [ "$(ROOT)/smoke/venus_image_fetch.comp" -nt $@ ] && \
	    ! [ "$(ROOT)/smoke/venus_combined_sample.comp" -nt $@ ] && \
	    ! [ "$(ROOT)/smoke/venus_dxvk_contract_sample.comp" -nt $@ ] && \
	    ! [ "$(ROOT)/smoke/venus_depth_array_compare.comp" -nt $@ ] && \
	    ! [ "$(ROOT)/smoke/venus_depth_cube_sample.comp" -nt $@ ] && \
	    ! [ "$(ROOT)/smoke/venus_depth_cube_compare.comp" -nt $@ ] && \
	    ! [ "$(ROOT)/smoke/venus_depth_cube_separated_compare.comp" -nt $@ ] && \
	    ! [ "$(ROOT)/smoke/venus_depth_cube_dxvk_contract_compare.spvasm" -nt $@ ] && \
	    ! [ "$(ROOT)/smoke/venus_depth_cube_array_sample.comp" -nt $@ ] && \
	    ! [ "$(ROOT)/smoke/venus_depth_cube_array_2d_compare.comp" -nt $@ ] && \
	    ! [ "$(ROOT)/smoke/venus_depth_cube_array_compare.comp" -nt $@ ] && \
	    ! [ "$(ROOT)/smoke/venus_spirv_replay.c" -nt $@ ] && \
	    ! [ "$(ROOT)/smoke/venus_separated_sample.comp" -nt $@ ] && \
	    ! find $(ROOT)/thirdparty/freetype \
	           $(ROOT)/thirdparty/libffi \
	           $(ROOT)/thirdparty/wayland \
	           $(ROOT)/thirdparty/wayland-protocols \
	           $(ROOT)/thirdparty/libxml2 \
	           $(ROOT)/thirdparty/libxkbcommon \
	           $(ROOT)/thirdparty/xkeyboard-config \
	           $(ROOT)/thirdparty/mesa \
	           $(ROOT)/thirdparty/libdrm \
	           -newer $@ -type f \
	           \( -name '*.c' -o -name '*.h' -o -name '*.cpp' -o -name '*.cc' \
	              -o -name 'meson.build' -o -name 'CMakeLists.txt' \
	              -o -name 'configure' -o -name '*.py' -o -name '*.xml' \
	              -o -name '*.ac' -o -name 'Makefile.am' -o -name '*.m4' \) \
	           2>/dev/null | grep -q .; then \
	    echo "  [deps] up to date"; \
	else \
	    echo "=== deps ==="; \
	    bash $(SCRIPTS)/build_deps.sh && touch $@; \
	fi

# ============================================================
# wine — Wine 交叉编译 + wineserver
# ============================================================
.PHONY: wine
# wine stamp 按 WINE_ARCH 区分: 方案② (arm64 设备 + x86_64 wine) 与方案③ (aarch64)
# 的 NATIVE_ARCH 相同 (arm64-v8a), 共用 stamp 会让方案② 误用方案③ 的 stamp 跳过构建
# → assemble 找不到 wine-ohos-x86_64。方案① (NATIVE_ARCH=x86_64) 无冲突但同样带后缀。
wine: $(STAMPS)/wine-$(CONFIG)-$(WINE_ARCH)

$(STAMPS)/wine-$(CONFIG)-$(WINE_ARCH): $(SCRIPTS)/build_wine.sh $(SCRIPTS)/env.sh $(STAMPS)/deps FORCE | $(STAMPS)
	@if [ -f $@ ] && [ -f $(WINE_SENTINEL) ] && \
	    ! [ "$(SCRIPTS)/build_wine.sh" -nt $@ ] && \
	    ! find $(ROOT)/thirdparty/wine \
	           -newer $@ -type f \
	           \( -name '*.c' -o -name '*.h' -o -name '*.cpp' -o -name '*.cc' \
	              -o -name 'meson.build' -o -name 'CMakeLists.txt' \
	              -o -name 'configure' -o -name '*.ac' -o -name 'Makefile.am' \
	              -o -name '*.m4' -o -name '*.in' -o -name '*.rc' -o -name '*.spec' \) \
	           2>/dev/null | grep -q .; then \
	    echo "  [wine] up to date"; \
	else \
	    echo "=== wine ($(CONFIG)) ==="; \
	    bash $(SCRIPTS)/build_wine.sh && touch $@; \
	fi

# ============================================================
# fex — FEX arm64ec 模拟器 (arm64 原生 wine 转译 x86_64 应用, libarm64ecfex.dll)
# ============================================================
.PHONY: fex
fex: $(STAMPS)/fex-arm64-v8a

$(STAMPS)/fex-arm64-v8a: $(SCRIPTS)/build_fex.sh $(SCRIPTS)/env.sh FORCE | $(STAMPS)
	@if [ "$(WINE_ARCH)" = "x86_64" ]; then \
	    echo "  [fex] skip (x86_64)"; \
	    mkdir -p $(dir $@) && touch $@; \
	elif [ -f $@ ] && \
	    ! [ "$(SCRIPTS)/build_fex.sh" -nt $@ ] && \
	    ! find $(ROOT)/thirdparty/fex \
	           -newer $@ -type f \
	           \( -name '*.c' -o -name '*.cpp' -o -name '*.h' -o -name '*.S' \
	              -o -name 'CMakeLists.txt' -o -name '*.cmake' \) \
	           2>/dev/null | grep -q .; then \
	    echo "  [fex] up to date"; \
	else \
	    echo "=== fex ==="; \
	    bash $(SCRIPTS)/build_fex.sh && touch $@; \
	fi

# ============================================================
# box64 — Box64 in-process 转译器 box64.so (box64+wine 方案②)
#         (arm64 设备 + x86_64 wine 全转译; NATIVE_ARCH=arm64-v8a + WINE_ARCH=x86_64)
# ============================================================
.PHONY: box64
box64: $(STAMPS)/box64-arm64-v8a

$(STAMPS)/box64-arm64-v8a: $(SCRIPTS)/build_box64.sh $(SCRIPTS)/env.sh FORCE | $(STAMPS)
	@if [ "$(WINE_ARCH)" = "aarch64" ] || [ "$(NATIVE_ARCH)" = "x86_64" ]; then \
	    echo "  [box64] skip (非 box64+wine 方案, WINE_ARCH=$(WINE_ARCH))"; \
	    mkdir -p $(dir $@) && touch $@; \
	elif [ -f $@ ] && \
	    [ -f $(ROOT)/entry/libs/arm64-v8a/box64.so ] && \
	    ! [ "$(SCRIPTS)/build_box64.sh" -nt $@ ] && \
	    ! find $(ROOT)/thirdparty/box64 \
	           -newer $@ -type f \
	           \( -name '*.c' -o -name '*.h' -o -name '*.S' -o -name '*.py' \
	              -o -name 'CMakeLists.txt' -o -name '*.cmake' \) \
	           2>/dev/null | grep -q .; then \
	    echo "  [box64] up to date"; \
	else \
	    echo "=== box64 (box64+wine) ==="; \
	    bash $(SCRIPTS)/build_box64.sh && touch $@; \
	fi

# ============================================================
# box64-wow64 — Box64 WoW64 DLL (arm64 原生 wine 方案③, wowbox64.dll)
#         (转译 32 位 x86 应用, HODLL 默认引擎; fex 的 libwow64fex.dll 同级备选)
# ============================================================
.PHONY: box64-wow64
box64-wow64: $(STAMPS)/box64-wow64-arm64-v8a

$(STAMPS)/box64-wow64-arm64-v8a: $(SCRIPTS)/build_box64_wow64.sh $(SCRIPTS)/env.sh FORCE | $(STAMPS)
	@if [ "$(WINE_ARCH)" = "x86_64" ]; then \
	    echo "  [box64-wow64] skip (非 arm64 原生 wine, WINE_ARCH=x86_64)"; \
	    mkdir -p $(dir $@) && touch $@; \
	elif [ -f $@ ] && \
	    [ -f $(BUILD_DIR)/box64-pe/wowbox64-prefix/src/wowbox64-build/wowbox64.dll ] && \
	    ! [ "$(SCRIPTS)/build_box64_wow64.sh" -nt $@ ] && \
	    ! find $(ROOT)/thirdparty/box64 \
	           -newer $@ -type f \
	           \( -name '*.c' -o -name '*.h' -o -name '*.S' -o -name '*.py' \
	              -o -name 'CMakeLists.txt' -o -name '*.cmake' \) \
	           2>/dev/null | grep -q .; then \
	    echo "  [box64-wow64] up to date"; \
	else \
	    echo "=== box64-wow64 (wowbox64.dll) ==="; \
	    bash $(SCRIPTS)/build_box64_wow64.sh && touch $@; \
	fi

# ============================================================
# native — Native compositor 依赖 → entry/libs/ (架构相关)
# ============================================================
.PHONY: native
native: $(foreach a,$(ARCHES),$(STAMPS)/$(a)/native)

NATIVE_SENTINEL_arm64_v8a := $(ROOT)/entry/libs/arm64-v8a/libvirglrenderer.so.1
NATIVE_SENTINEL_x86_64    := $(ROOT)/entry/libs/x86_64/libvirglrenderer.so.1

define native_rule
.PHONY: native-$(1)
native-$(1): $$(STAMPS)/$(1)/native

$$(STAMPS)/$(1)/native: $(SCRIPTS)/build_native.sh $(SCRIPTS)/env.sh FORCE | $$(STAMPS)/$(1)
	@sentinel="$(NATIVE_SENTINEL_$(subst -,_,$(1)))"; \
	libs_dir="$(ROOT)/entry/libs/$(1)"; \
		if [ -f $$@ ] && [ -f "$$$$sentinel" ] && \
		    [ -f "$$$$libs_dir/libfreetype.so.6" ] && \
		    [ -f "$$$$libs_dir/libxkbcommon.so.0" ] && \
		    [ -f "$$$$libs_dir/libxml2.so.2" ] && \
		    [ -f "$$$$libs_dir/libwayland-server.so.0" ] && \
		    [ -f "$$$$libs_dir/libffi.so.8" ] && \
		    [ -f "$$$$libs_dir/libwinehua_vtest_server.so" ] && \
	    ! [ "$(SCRIPTS)/build_native.sh" -nt $$@ ] && \
	    ! find $(ROOT)/thirdparty/wayland \
	           $(ROOT)/thirdparty/libffi \
	           $(ROOT)/thirdparty/libepoxy \
	           $(ROOT)/thirdparty/virglrenderer \
	           -newer $$@ -type f \
	           \( -name '*.c' -o -name '*.h' -o -name '*.cpp' -o -name '*.cc' \
	              -o -name 'meson.build' -o -name 'CMakeLists.txt' \
	              -o -name 'configure' -o -name '*.ac' -o -name 'Makefile.am' \) \
	           2>/dev/null | grep -q .; then \
	    echo "  [native/$(1)] up to date"; \
	else \
	    echo "=== native ($(1)) ==="; \
	    NATIVE_ARCH=$(1) bash $(SCRIPTS)/build_native.sh && touch $$@; \
	fi
endef
$(foreach a,arm64-v8a x86_64,$(eval $(call native_rule,$(a))))

# ============================================================
# assemble — 组装布局 (架构 + 设备类型相关)
# ============================================================
.PHONY: assemble
assemble: $(foreach a,$(ARCHES),$(STAMPS)/$(a)/assemble)

define assemble_rule
.PHONY: assemble-$(1)

assemble-$(1): $$(STAMPS)/$(1)/assemble

$$(STAMPS)/$(1)/assemble: $(SCRIPTS)/assemble.sh $(SCRIPTS)/env.sh $(DXVK_ARTIFACTS) $(DXVK_MODERN_ARTIFACTS) \
	$(VKD3D_PROTON_ARTIFACTS) \
	$(ROOT)/smoke/winehua_d3d8_smoke.c \
	$(ROOT)/smoke/winehua_d3d_switch_cube.c \
	$(ROOT)/smoke/winehua_gpu_diagnostics.c \
	$(ROOT)/smoke/winehua_dxvk26_requirements.c \
	$(ROOT)/smoke/winehua_win32_driver.c \
	$$(STAMPS)/deps $$(STAMPS)/wine-$(1)-$(WINE_ARCH) $$(STAMPS)/$(1)/native \
	$$(STAMPS)/$(1)/host-vulkan \
	$$(ASSEMBLE_GUEST_INPUTS) | $$(STAMPS)/$(1)
	@echo "=== assemble ($(1)) ==="
	NATIVE_ARCH=$(1) GUEST_ARCH=$(GUEST_ARCH) BUILD_GUEST_GFX=$(BUILD_GUEST_GFX) bash $(SCRIPTS)/assemble.sh
	@touch $$@
endef
$(foreach a,arm64-v8a x86_64,$(eval $(call assemble_rule,$(a))))

# arm64 assemble 额外依赖: 方案③ fex (libarm64ecfex.dll) + box64-wow64 (wowbox64.dll);
# 方案② box64 (box64.so)。各 target 内部按 WINE_ARCH skip。
# 32-bit PE DLL (i386-windows) 已由 wine 主构建 --enable-archs=i386 提供
$(STAMPS)/arm64-v8a/assemble: $(STAMPS)/fex-arm64-v8a $(STAMPS)/box64-arm64-v8a $(STAMPS)/box64-wow64-arm64-v8a

# ============================================================
# hap — HAP 构建 + 签名 (统一 rawfile zip)
# ============================================================
.PHONY: hap
hap: assemble
	@echo "=== hap ($(CONFIG)) ==="
	bash $(SCRIPTS)/package.sh hap
	@echo ""
	@echo "HAP: $(ROOT)/entry/build/default/outputs/default/entry-default-signed.hap"
	@ls -lh $(ROOT)/entry/build/default/outputs/default/entry-default-signed.hap 2>/dev/null || true

# ============================================================
# test: 宿主机单元测试 (纯函数, 不依赖 OHOS SDK, 用宿主 g++ 编译)
# ============================================================
HOST_TEST_DIR := $(BUILD_DIR)/host_tests

.PHONY: test
test:
	@mkdir -p $(HOST_TEST_DIR)
	g++ -std=c++17 -Wall -Wextra -I $(ROOT)/entry/src/main/cpp \
	    -o $(HOST_TEST_DIR)/geometry_test \
	    $(ROOT)/host_tests/geometry_test.cpp \
	    $(ROOT)/entry/src/main/cpp/compositor/frame/geometry.cpp
	$(HOST_TEST_DIR)/geometry_test
	g++ -std=c++17 -Wall -Wextra -I $(ROOT)/entry/src/main/cpp \
	    -o $(HOST_TEST_DIR)/blit_scaled_test \
	    $(ROOT)/host_tests/blit_scaled_test.cpp \
	    $(ROOT)/entry/src/main/cpp/compositor/frame/compositor_blit.cpp
	$(HOST_TEST_DIR)/blit_scaled_test
	g++ -std=c++17 -Wall -Wextra -I $(ROOT)/entry/src/main/cpp \
	    -o $(HOST_TEST_DIR)/blit_clip_test \
	    $(ROOT)/host_tests/blit_clip_test.cpp
	$(HOST_TEST_DIR)/blit_clip_test
	g++ -std=c++17 -Wall -Wextra -I $(ROOT)/entry/src/main/cpp \
	    -o $(HOST_TEST_DIR)/shm_frame_source_test \
	    $(ROOT)/host_tests/shm_frame_source_test.cpp \
	    $(ROOT)/entry/src/main/cpp/compositor/frame/shm_frame_source.cpp
	$(HOST_TEST_DIR)/shm_frame_source_test
	g++ -std=c++17 -Wall -Wextra -I $(ROOT)/entry/src/main/cpp \
	    -o $(HOST_TEST_DIR)/zorder_test \
	    $(ROOT)/host_tests/zorder_test.cpp
	$(HOST_TEST_DIR)/zorder_test
	g++ -std=c++17 -Wall -Wextra -I $(ROOT)/entry/src/main/cpp \
	    -o $(HOST_TEST_DIR)/env_spec_test \
	    $(ROOT)/host_tests/env_spec_test.cpp \
	    $(ROOT)/entry/src/main/cpp/env_spec.cpp
	$(HOST_TEST_DIR)/env_spec_test
	g++ -std=c++17 -Wall -Wextra -I $(ROOT)/entry/src/main/cpp \
	    -o $(HOST_TEST_DIR)/env_baseline_test \
	    $(ROOT)/host_tests/env_baseline_test.cpp
	$(HOST_TEST_DIR)/env_baseline_test
	g++ -std=c++17 -Wall -Wextra -I $(ROOT)/entry/src/main/cpp \
	    -o $(HOST_TEST_DIR)/input_state_test \
	    $(ROOT)/host_tests/input_state_test.cpp \
	    $(ROOT)/entry/src/main/cpp/compositor/input/input_state_tracker.cpp
	$(HOST_TEST_DIR)/input_state_test
	g++ -std=c++17 -Wall -Wextra -I $(ROOT)/entry/src/main/cpp \
	    -o $(HOST_TEST_DIR)/toplevel_event_test \
	    $(ROOT)/host_tests/toplevel_event_test.cpp
	$(HOST_TEST_DIR)/toplevel_event_test
	g++ -std=c++17 -Wall -Wextra -I $(ROOT)/entry/src/main/cpp \
	    -o $(HOST_TEST_DIR)/presenter_common_test \
	    $(ROOT)/host_tests/presenter_common_test.cpp
	$(HOST_TEST_DIR)/presenter_common_test
	g++ -std=c++17 -Wall -Wextra -I $(ROOT)/entry/src/main/cpp \
	    -o $(HOST_TEST_DIR)/controller_merge_test \
	    $(ROOT)/host_tests/controller_merge_test.cpp \
	    $(ROOT)/entry/src/main/cpp/input/controller/controller_hub.cpp
	$(HOST_TEST_DIR)/controller_merge_test


# ============================================================
# clean
# ============================================================
.PHONY: clean
clean:
	@echo "=== clean ==="
	rm -rf $(BUILD_DIR)
	rm -f $(ROOT)/entry/libs/arm64-v8a/*.so*
	rm -f $(ROOT)/entry/libs/arm64-v8a/virgl_test_server
	rm -f $(ROOT)/entry/libs/x86_64/*.so*
	rm -f $(ROOT)/entry/libs/x86_64/virgl_test_server
	rm -rf $(ROOT)/entry/build
	rm -f $(ROOT)/entry/src/main/resources/rawfile/wine-data.zip
	@echo "  已清理所有中间产物"

# ============================================================
# 帮助
# ============================================================
.PHONY: help
help:
	@echo "用法: make [target] [NATIVE_ARCH=x86_64|arm64-v8a|all]"
	@echo ""
	@echo "默认: NATIVE_ARCH=x86_64"
	@echo "SDK: target=$(TARGET_SDK_VERSION), compatible=$(COMPATIBLE_SDK_VERSION)"
	@echo ""
	@echo "全部构建:"
	@echo "  make                                          # 默认配置全量 → HAP"
	@echo "  make NATIVE_ARCH=arm64-v8a                    # ARM64 (方案② box64+wine: 加 WINE_ARCH=x86_64)"
	@echo "  make NATIVE_ARCH=x86_64                       # x86_64 (方案①)"
	@echo ""
	@echo "单模块:"
	@echo "  make deps      # 交叉编译依赖 → sysroot-ext"
	@echo "  make wine      # Wine + wineserver"
	@echo "  make fex       # FEX 模拟器 DLL (arm64 转译 x64/x86 应用)"
	@echo "  make box64     # Box64 in-process 转译器 box64.so (box64+wine 方案②)"
	@echo "  make box64-wow64 # Box64 WoW64 DLL wowbox64.dll (arm64 原生方案③, HODLL)"
	@echo "  make native    # Native compositor 依赖"
	@echo "  make host-vulkan # Host Vulkan exact replay"
	@echo "  make assemble  # 组装布局"
	@echo "  make hap       # HAP 打包 + 签名"
	@echo ""
	@echo "每个架构:"
	@echo "  make native-x86_64  make native-arm64-v8a"
	@echo ""
	@echo "清理:"
	@echo "  make clean     # 删除所有中间产物"
	@echo ""
	@echo "产物统一在 build/ 下"
