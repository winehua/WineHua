# Makefile — Wine for HarmonyOS 构建编排
#
# 用法:
#   make                                          # 默认: x86_64 PC 全量构建
#   make NATIVE_ARCH=x86_64 DEVICE_TYPE=pc
#   make NATIVE_ARCH=arm64-v8a DEVICE_TYPE=pad
#   make NATIVE_ARCH=all DEVICE_TYPE=pc           # 双架构 HAP
#
#   单个模块: make deps | wine | box64 | native | assemble | hap
#   清理:     make clean

ROOT := $(realpath $(dir $(lastword $(MAKEFILE_LIST))))

# ── 配置 ──
NATIVE_ARCH ?= x86_64
DEVICE_TYPE ?= pc
export NATIVE_ARCH
export DEVICE_TYPE

CONFIG    := $(NATIVE_ARCH)-$(DEVICE_TYPE)
BUILD_DIR := $(ROOT)/build
STAMPS    := $(BUILD_DIR)/.stamps
SCRIPTS   := $(ROOT)/scripts

# 架构列表 (NATIVE_ARCH=all 时展开为两个)
ifeq ($(NATIVE_ARCH),all)
ARCHES := arm64-v8a x86_64
else
ARCHES := $(NATIVE_ARCH)
endif

# ── 关键产物 (用于验证构建是否完成) ──
DEPS_SENTINEL   := $(BUILD_DIR)/sysroot-ext/usr/lib/x86_64-linux-ohos/libfreetype.so.6
WINE_SENTINEL   := $(BUILD_DIR)/wine-native/tools/winegcc/winegcc

# ============================================================
# 默认目标
# ============================================================
.PHONY: all
all: hap

# 确保 stamps 目录存在
$(STAMPS):
	mkdir -p $(STAMPS)

# 确保架构子目录存在
$(STAMPS)/arm64-v8a $(STAMPS)/x86_64:
	mkdir -p $@

# ============================================================
# deps — 交叉编译依赖 → build/sysroot-ext/ (架构无关)
# ============================================================
.PHONY: deps
deps: $(STAMPS)/deps

$(STAMPS)/deps: $(SCRIPTS)/build_deps.sh $(SCRIPTS)/env.sh | $(STAMPS)
	@if [ -f $@ ] && [ -f $(DEPS_SENTINEL) ] && \
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
wine: $(STAMPS)/wine-$(CONFIG)

$(STAMPS)/wine-$(CONFIG): $(SCRIPTS)/build_wine.sh $(SCRIPTS)/env.sh $(STAMPS)/deps | $(STAMPS)
	@if [ -f $@ ] && [ -f $(WINE_SENTINEL) ] && \
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
# box64 — ARM64 翻译器 (始终 arm64-v8a 架构, PC: 可执行文件, Pad: .so)
# ============================================================
.PHONY: box64
box64: $(STAMPS)/box64-arm64-v8a-$(DEVICE_TYPE)

$(STAMPS)/box64-arm64-v8a-$(DEVICE_TYPE): $(SCRIPTS)/build_box64.sh $(SCRIPTS)/env.sh | $(STAMPS)
	@if [ "$(NATIVE_ARCH)" = "x86_64" ]; then \
	    echo "  [box64] skip (x86_64)"; \
	    mkdir -p $(dir $@) && touch $@; \
	elif [ -f $@ ] && \
	    ! find $(ROOT)/thirdparty/box64 \
	           -newer $@ -type f \
	           \( -name '*.c' -o -name '*.h' -o -name '*.cpp' -o -name '*.S' \
	              -o -name 'CMakeLists.txt' -o -name '*.cmake' \) \
	           2>/dev/null | grep -q .; then \
	    echo "  [box64] up to date"; \
	else \
	    echo "=== box64 ($(DEVICE_TYPE)) ==="; \
	    NATIVE_ARCH=arm64-v8a bash $(SCRIPTS)/build_box64.sh && touch $@; \
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

$$(STAMPS)/$(1)/native: $(SCRIPTS)/build_native.sh $(SCRIPTS)/env.sh | $$(STAMPS)/$(1)
	@sentinel="$(NATIVE_SENTINEL_$(subst -,_,$(1)))"; \
	if [ -f $$@ ] && [ -f "$$$$sentinel" ] && \
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
assemble: $(foreach a,$(ARCHES),$(STAMPS)/$(a)/assemble-$(DEVICE_TYPE))

define assemble_rule
.PHONY: assemble-$(1)-pc assemble-$(1)-pad

assemble-$(1)-pc:  $$(STAMPS)/$(1)/assemble-pc
assemble-$(1)-pad: $$(STAMPS)/$(1)/assemble-pad

$$(STAMPS)/$(1)/assemble-pc: $(SCRIPTS)/assemble.sh $(SCRIPTS)/env.sh \
	$$(STAMPS)/wine-$(1)-pc $$(STAMPS)/$(1)/native | $$(STAMPS)/$(1)
	@echo "=== assemble ($(1), pc) ==="
	NATIVE_ARCH=$(1) DEVICE_TYPE=pc bash $(SCRIPTS)/assemble.sh
	@touch $$@

$$(STAMPS)/$(1)/assemble-pad: $(SCRIPTS)/assemble.sh $(SCRIPTS)/env.sh \
	$$(STAMPS)/wine-$(1)-pad $$(STAMPS)/$(1)/native | $$(STAMPS)/$(1)
	@echo "=== assemble ($(1), pad) ==="
	NATIVE_ARCH=$(1) DEVICE_TYPE=pad bash $(SCRIPTS)/assemble.sh
	@touch $$@
endef
$(foreach a,arm64-v8a x86_64,$(eval $(call assemble_rule,$(a))))

# arm64 assemble 额外依赖 box64
$(STAMPS)/arm64-v8a/assemble-pc:  $(STAMPS)/box64-arm64-v8a-pc
$(STAMPS)/arm64-v8a/assemble-pad: $(STAMPS)/box64-arm64-v8a-pad

# ============================================================
# hnp — HNP 打包 (仅 PC)
# ============================================================
.PHONY: hnp
hnp: $(foreach a,$(ARCHES),$(STAMPS)/$(a)/hnp)

define hnp_rule
.PHONY: hnp-$(1)
hnp-$(1): $$(STAMPS)/$(1)/hnp

$$(STAMPS)/$(1)/hnp: $(SCRIPTS)/package.sh $$(STAMPS)/$(1)/assemble-pc | $$(STAMPS)/$(1)
	@echo "=== hnp ($(1)) ==="
	NATIVE_ARCH=$(1) DEVICE_TYPE=pc bash $(SCRIPTS)/package.sh hnp
	@touch $$@
endef
$(foreach a,arm64-v8a x86_64,$(eval $(call hnp_rule,$(a))))

# ============================================================
# hap — HAP 构建 + 签名 (PC: 含 HNP, Pad: rawfile)
# ============================================================
.PHONY: hap
hap: assemble
ifeq ($(DEVICE_TYPE),pc)
hap: hnp
endif
	@echo "=== hap ($(CONFIG)) ==="
	bash $(SCRIPTS)/package.sh hap
	@echo ""
	@echo "HAP: $(ROOT)/entry/build/default/outputs/default/entry-default-signed.hap"
	@ls -lh $(ROOT)/entry/build/default/outputs/default/entry-default-signed.hap 2>/dev/null || true

# ============================================================
# clean
# ============================================================
.PHONY: clean
clean:
	@echo "=== clean ==="
	rm -rf $(BUILD_DIR)
	rm -f $(ROOT)/entry/libs/arm64-v8a/*.so
	rm -f $(ROOT)/entry/libs/arm64-v8a/virgl_test_server
	rm -f $(ROOT)/entry/libs/x86_64/*.so
	rm -f $(ROOT)/entry/libs/x86_64/virgl_test_server
	rm -rf $(ROOT)/entry/hnp/*
	rm -rf $(ROOT)/entry/build
	rm -f $(ROOT)/entry/src/main/resources/rawfile/wine-data.zip
	@echo "  已清理所有中间产物"

# ============================================================
# 帮助
# ============================================================
.PHONY: help
help:
	@echo "用法: make [target] [NATIVE_ARCH=x86_64|arm64-v8a|all] [DEVICE_TYPE=pc|pad]"
	@echo ""
	@echo "默认: NATIVE_ARCH=x86_64 DEVICE_TYPE=pc"
	@echo ""
	@echo "全部构建:"
	@echo "  make                                          # 默认配置全量 → HAP"
	@echo "  make NATIVE_ARCH=arm64-v8a DEVICE_TYPE=pad    # ARM64 Pad"
	@echo "  make NATIVE_ARCH=all DEVICE_TYPE=pc           # 双架构 PC HAP"
	@echo ""
	@echo "单模块:"
	@echo "  make deps      # 交叉编译依赖 → sysroot-ext"
	@echo "  make wine      # Wine + wineserver"
	@echo "  make box64     # Box64 (仅 arm64)"
	@echo "  make native    # Native compositor 依赖"
	@echo "  make assemble  # 组装 HNP/Pad 布局"
	@echo "  make hap       # HAP 打包 + 签名"
	@echo ""
	@echo "每个架构:"
	@echo "  make native-x86_64  make native-arm64-v8a"
	@echo "  make hnp-x86_64     make hnp-arm64-v8a"
	@echo ""
	@echo "清理:"
	@echo "  make clean     # 删除所有中间产物"
	@echo ""
	@echo "产物统一在 build/ 下"
