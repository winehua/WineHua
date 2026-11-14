.PHONY: p0-wayland-bootstrap
p0-wayland-bootstrap:
	bash $(ROOT)/scripts/build_wayland.sh
.PHONY: p0-x11-native
p0-x11-native:
	/apps/harmony/sdk/default/openharmony/native/build-tools/cmake/bin/cmake -S $(ROOT)/entry/src/main/cpp -B /workspace/WineHua-root/WineHua/build/hgs-p0/2026-11-06/x11-native-arm64 -G Ninja -DCMAKE_MAKE_PROGRAM=/usr/bin/ninja -DCMAKE_TOOLCHAIN_FILE=/apps/harmony/sdk/default/hms/native/build/cmake/hmos.toolchain.bisheng.cmake -DOHOS_SDK_NATIVE=/apps/harmony/sdk/default/openharmony/native -DOHOS_ARCH=arm64-v8a -DCMAKE_OHOS_ARCH_ABI=arm64-v8a -DCMAKE_BUILD_TYPE=Release
	/apps/harmony/sdk/default/openharmony/native/build-tools/cmake/bin/cmake --build /workspace/WineHua-root/WineHua/build/hgs-p0/2026-11-06/x11-native-arm64 --parallel 8
.PHONY: p0-guest-x11-libs p0-wine-guest
p0-guest-x11-libs:
	SIDE=guest bash $(ROOT)/scripts/build_xcb_stack.sh
	SIDE=guest bash $(ROOT)/scripts/build_x11_client.sh
p0-wine-guest:
	bash $(ROOT)/scripts/build_wine.sh

.PHONY: p0-guest-gfx p0-guest-venus-icd p0-x11-assemble p0-x11-hap
# Run gfx and venus targets sequentially: their script shares a mode-specific cross file.
p0-guest-gfx:
	NATIVE_ARCH=x86_64 bash $(ROOT)/scripts/build_ohos_guest_gfx.sh --no-fetch
p0-guest-venus-icd:
	WINEHUA_GUEST_VULKAN_ONLY=1 WINEHUA_GUEST_GFX_BUILD_ROOT=$(ROOT)/build/guest_vulkan_build/x86_64/mesa-venus-p0 WINEHUA_GUEST_GFX_INSTALL_ROOT=$(ROOT)/build/guest_vulkan_build/x86_64/mesa-venus-install NATIVE_ARCH=x86_64 bash $(ROOT)/scripts/build_ohos_guest_gfx.sh --no-package --no-fetch
p0-x11-assemble:
	NATIVE_ARCH=arm64-v8a GUEST_ARCH=x86_64 BUILD_GUEST_GFX=1 BUILD_GUEST_VULKAN=1 bash $(ROOT)/scripts/assemble.sh
p0-x11-hap:
	PATH=/workspace/harmony-sdk/bin:/workspace/harmony-sdk/tool/node/bin:$(PATH) /workspace/harmony-sdk/bin/hvigorw assembleHap --no-daemon
