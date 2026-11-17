# make generates protocol sources outside the tracked snapshots. DevEco builds
# using prebuilt dependencies may still use the committed protocol snapshots.
get_filename_component(VPP_PROJECT_ROOT "${NATIVERENDER_ROOT_PATH}/../../../.." ABSOLUTE)
set(VPP_NATIVE_GENERATED_ROOT "${VPP_PROJECT_ROOT}/build/native_${OHOS_ARCH}"
    CACHE PATH "Native dependency build tree (override for a custom BUILD_DIR)")
set(VPP_WAYLAND_PROTOCOLS "${NATIVERENDER_ROOT_PATH}/protocols")
if(EXISTS "${VPP_NATIVE_GENERATED_ROOT}/protocols/xdg-shell-protocol.c" AND
   EXISTS "${VPP_NATIVE_GENERATED_ROOT}/protocols/winehua-toplevel-protocol.c")
    set(VPP_WAYLAND_PROTOCOLS "${VPP_NATIVE_GENERATED_ROOT}/protocols")
    # Both bare header names and explicit protocols/header.h includes are used.
    include_directories(BEFORE "${VPP_NATIVE_GENERATED_ROOT}" "${VPP_WAYLAND_PROTOCOLS}")
endif()
