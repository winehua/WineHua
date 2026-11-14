# P0 build helper. Invoke together with the repository Makefile and explicit ROOT.
P0_SDK ?= /apps/harmony/sdk/default
P0_TOOLS ?= /workspace/harmony-sdk
P0_BUILD_DIR ?= /workspace/WineHua-root/WineHua/build/hgs-p0/2026-11-06/native-arm64

.PHONY: p0-hgs-native p0-hap-unsigned
p0-hgs-native:
	$(P0_SDK)/openharmony/native/build-tools/cmake/bin/cmake -S $(ROOT)/hgs/src/main/cpp -B $(P0_BUILD_DIR) -G Ninja -DCMAKE_MAKE_PROGRAM=/usr/bin/ninja -DCMAKE_TOOLCHAIN_FILE=$(P0_SDK)/hms/native/build/cmake/hmos.toolchain.bisheng.cmake -DOHOS_SDK_NATIVE=$(P0_SDK)/openharmony/native -DOHOS_ARCH=arm64-v8a -DCMAKE_OHOS_ARCH_ABI=arm64-v8a -DCMAKE_BUILD_TYPE=Release
	$(P0_SDK)/openharmony/native/build-tools/cmake/bin/cmake --build $(P0_BUILD_DIR) --parallel 8

# Requires prepared dependencies/assets and an isolated unsigned build profile.
p0-hap-unsigned:
	PATH=$(P0_TOOLS)/bin:$(P0_TOOLS)/tool/node/bin:$(PATH) $(P0_TOOLS)/bin/hvigorw assembleHap --no-daemon
