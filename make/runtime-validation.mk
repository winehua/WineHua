# Focused merge checks. Invoke through the existing winehua-dev Docker image.
RUNTIME_DXVK_REPOSITORY ?= $(ROOT)/thirdparty/dxvk
RUNTIME_SDK ?= /apps/harmony/sdk/default/openharmony
RUNTIME_CHECK_DIR := $(ROOT)/workspace_temp/runtime-check
.PHONY: runtime-host-check runtime-native-check
runtime-host-check: test runtime-patch-check
	python3 $(ROOT)/scripts/verify_runtime_boundary.py
	python3 $(ROOT)/host_tests/runtime_recipe_test.py
	python3 $(ROOT)/host_tests/dxvk9_backbuffer_preservation_test.py --repository "$(RUNTIME_DXVK_REPOSITORY)"
	g++ -std=c++17 -Wall -Wextra -I $(ROOT)/entry/src/main/cpp $(ROOT)/host_tests/benchmark_statistics_test.cpp -o $(HOST_TEST_DIR)/benchmark_statistics_test
	$(HOST_TEST_DIR)/benchmark_statistics_test
	python3 $(ROOT)/host_tests/wine_broker_argv_test.py
	g++ -std=c++17 -Wall -Wextra -no-pie -I $(ROOT)/entry/src/main/cpp $(ROOT)/host_tests/wine_image_policy_test.cpp -o $(HOST_TEST_DIR)/wine_image_policy_test
	$(HOST_TEST_DIR)/wine_image_policy_test
	python3 $(ROOT)/host_tests/gamepad_bridge_test.py
	python3 $(ROOT)/host_tests/multiple_gamepads_test.py
	python3 $(ROOT)/host_tests/source_build_contract_test.py
	python3 $(ROOT)/host_tests/wine_build_identity_test.py
	g++ -std=c++17 -Wall -Wextra -I $(ROOT)/entry/src/main/cpp $(ROOT)/host_tests/display_route_metrics_test.cpp -o $(HOST_TEST_DIR)/display_route_metrics_test
	$(HOST_TEST_DIR)/display_route_metrics_test
	python3 $(ROOT)/scripts/verify_runtime_graphics.py
	python3 $(ROOT)/host_tests/runtime_pipeline_test.py
	python3 $(ROOT)/host_tests/signing_contract_test.py

runtime-native-check:
	cmake -S $(ROOT)/entry/src/main/cpp -B $(RUNTIME_CHECK_DIR)/native \
	  -DCMAKE_TOOLCHAIN_FILE=$(RUNTIME_SDK)/native/build/cmake/ohos.toolchain.cmake \
	  -DOHOS_ARCH=arm64-v8a -DOHOS_PLATFORM=OHOS -DCMAKE_BUILD_TYPE=RelWithDebInfo \
	  -DVPP_HOST_EXT_ROOT=$(VPP_HOST_EXT_ROOT)
	cmake --build $(RUNTIME_CHECK_DIR)/native --parallel 6

# Optional host Wayland dependency (distribution -dev package or an existing
# read-only host library); never linked into the OHOS application.
RUNTIME_HOST_WAYLAND ?= -lwayland-server
.PHONY: runtime-protocol-check runtime-zink-check
runtime-protocol-check:
	g++ -std=c++17 -Wall -Wextra -pthread -I $(ROOT)/entry/src/main/cpp/protocols -I $(ROOT)/entry/src/main/cpp $(ROOT)/host_tests/wayland_clipboard_lifecycle_test.cpp $(RUNTIME_HOST_WAYLAND) -o $(HOST_TEST_DIR)/wayland_clipboard_lifecycle_test
	LD_LIBRARY_PATH=$(RUNTIME_CHECK_DIR)/host-libs:$$LD_LIBRARY_PATH $(HOST_TEST_DIR)/wayland_clipboard_lifecycle_test
	g++ -std=c++17 -Wall -Wextra -pthread -I $(ROOT)/entry/src/main/cpp $(ROOT)/host_tests/open_protocol_test.cpp -o $(HOST_TEST_DIR)/open_protocol_test
	$(HOST_TEST_DIR)/open_protocol_test
	$(RUNTIME_SDK)/native/llvm/bin/clang++ --target=x86_64-linux-gnu --sysroot=/ --gcc-toolchain=/usr -std=c++17 -Wall -Wextra -Wno-unknown-attributes -pthread -ffunction-sections -fdata-sections -Wl,--gc-sections -idirafter $(RUNTIME_SDK)/native/sysroot/usr/include -I $(ROOT)/entry/src/main/cpp $(ROOT)/host_tests/system_open_callback_test.cpp -o $(HOST_TEST_DIR)/system_open_callback_test
	$(HOST_TEST_DIR)/system_open_callback_test

RUNTIME_MESA_REPOSITORY ?= $(ROOT)/thirdparty/mesa
runtime-zink-check:
	python3 $(ROOT)/host_tests/zink_shadow_buffer_test.py --mesa-git $(RUNTIME_MESA_REPOSITORY)
	python3 $(ROOT)/host_tests/zink_shadow_image_test.py --mesa-git $(RUNTIME_MESA_REPOSITORY)

# Execute production backend persistence and EntryAbility launch policy in a
# small Harmony adapter, without bringing up a device or a graphics session.
.PHONY: runtime-ui-check
runtime-ui-check:
	bash -c 'source "$(ROOT)/scripts/env.sh"; node "$(ROOT)/host_tests/graphics_settings_test.cjs"'

# Run before submodule/toolchain setup so cached libraries cannot hide a
# missing or local-only patch in a newly cloned checkout.
.PHONY: runtime-patch-check runtime-crypto-check
runtime-patch-check:
	python3 $(ROOT)/scripts/check_build_patch_inputs.py
	python3 $(ROOT)/host_tests/build_patch_inputs_test.py

runtime-crypto-check: runtime-patch-check
	python3 $(ROOT)/host_tests/nettle_arm64_dispatch_test.py

prepare-runtime-sources: runtime-patch-check
