#!/bin/bash
# Build the guest Vulkan stack for the active WINE_ARCH:
# Vulkan Loader -> Mesa Venus ICD -> vtest -> host virglrenderer/Vulkan.
# arm64 原生 wine → aarch64-linux-ohos venus guest (与 wine 同架构, 系统 linker 直接 dlopen);
# x86_64 → x86_64-linux-ohos。
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/env.sh"

# guest 栈架构与 Wine 对齐, 值域 aarch64|x86_64 (不是 NATIVE_ARCH 的 arm64-v8a)
GUEST_ARCH="${GUEST_ARCH:-$WINE_ARCH}"
case "$GUEST_ARCH" in
    aarch64|x86_64) ;;
    *) err "guest Vulkan requires GUEST_ARCH=aarch64 or x86_64, got $GUEST_ARCH" ;;
esac

LOADER_TAG="v1.3.290"
LOADER_COMMIT="f8616928ee19f6c7fd648c1cf1f456cba3771855"
HEADERS_TAG="v1.3.290"
HEADERS_COMMIT="b379292b2ab6df5771ba9870d53cf8b2c9295daf"

LOADER_SOURCE="$ROOT/tmp/Vulkan-Loader-$LOADER_TAG"
HEADERS_SOURCE="$ROOT/tmp/Vulkan-Headers-$HEADERS_TAG"
BUILD_ROOT="$ROOT/build/guest_vulkan_build/$GUEST_ARCH"
HEADERS_INSTALL="$BUILD_ROOT/headers-install"
LOADER_INSTALL="$BUILD_ROOT/loader-install"
OUTPUT_ROOT="$ROOT/build/guest_vulkan/$GUEST_ARCH"
MESA_INSTALL="$BUILD_ROOT/mesa-venus-install"
LOADER_PATCH="$ROOT/patches/vulkan-loader-v1.3.290-ohos.patch"

fetch_pinned_source() {
    local url="$1" tag="$2" commit="$3" destination="$4"
    if [ ! -d "$destination/.git" ]; then
        [ ! -e "$destination" ] || err "incomplete managed source exists: $destination"
        git clone --depth 1 --branch "$tag" "$url" "$destination"
    fi
    local actual
    actual="$(git -C "$destination" rev-parse HEAD)"
    [ "$actual" = "$commit" ] || \
        err "pinned source mismatch for $destination: expected $commit got $actual"
}

fetch_pinned_source \
    https://github.com/KhronosGroup/Vulkan-Headers.git \
    "$HEADERS_TAG" "$HEADERS_COMMIT" "$HEADERS_SOURCE"
fetch_pinned_source \
    https://github.com/KhronosGroup/Vulkan-Loader.git \
    "$LOADER_TAG" "$LOADER_COMMIT" "$LOADER_SOURCE"

[ -f "$LOADER_PATCH" ] || err "Vulkan Loader OHOS patch missing: $LOADER_PATCH"
if ! grep -q 'Linux|BSD|DragonFly|GNU|OHOS' "$LOADER_SOURCE/CMakeLists.txt"; then
    git -C "$LOADER_SOURCE" apply --check "$LOADER_PATCH"
    git -C "$LOADER_SOURCE" apply "$LOADER_PATCH"
fi

mkdir -p "$BUILD_ROOT"

log "--- Mesa Venus ICD ($TARGET, offscreen) ---"
WINEHUA_GUEST_VULKAN_ONLY=1 \
WINEHUA_GUEST_GFX_PLATFORM=wayland \
WINEHUA_GUEST_GFX_BUILD_ROOT="$BUILD_ROOT/mesa-venus-offscreen-v2" \
WINEHUA_GUEST_GFX_INSTALL_ROOT="$MESA_INSTALL" \
NATIVE_ARCH="$NATIVE_ARCH" \
    bash "$SCRIPT_DIR/build_ohos_guest_gfx.sh" --platform wayland --no-package
[ -f "$MESA_INSTALL/lib/libvulkan_virtio.so" ] || \
    err "Mesa Venus ICD build did not produce libvulkan_virtio.so"

log "--- Vulkan-Headers $HEADERS_TAG ---"
cmake -S "$HEADERS_SOURCE" -B "$BUILD_ROOT/headers" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$HEADERS_INSTALL" \
    -DVULKAN_HEADERS_ENABLE_TESTS=OFF
cmake --build "$BUILD_ROOT/headers" --target install --parallel "$JOBS"

log "--- Vulkan-Loader $LOADER_TAG ($TARGET) ---"
cmake -S "$LOADER_SOURCE" -B "$BUILD_ROOT/loader" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$OHOS_SDK/native/build/cmake/ohos.toolchain.cmake" \
    -DOHOS_ARCH="$OHOS_ARCH" \
    -DOHOS_PLATFORM=OHOS \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$LOADER_INSTALL" \
    -DCMAKE_INSTALL_LIBDIR=lib \
    -DCMAKE_PREFIX_PATH="$HEADERS_INSTALL" \
    -DBUILD_TESTS=OFF \
    -DBUILD_WERROR=OFF \
    -DBUILD_WSI_XCB_SUPPORT=OFF \
    -DBUILD_WSI_XLIB_SUPPORT=OFF \
    -DBUILD_WSI_WAYLAND_SUPPORT=OFF \
    -DBUILD_WSI_DIRECTFB_SUPPORT=OFF
cmake --build "$BUILD_ROOT/loader" --parallel "$JOBS"
cmake --install "$BUILD_ROOT/loader"

loader_binary="$(find "$LOADER_INSTALL/lib" -maxdepth 1 -type f -name 'libvulkan.so.1*' | sort | tail -n 1)"
[ -n "$loader_binary" ] || err "Vulkan Loader install did not produce libvulkan.so.1"

rm -rf "$OUTPUT_ROOT"
mkdir -p "$OUTPUT_ROOT/bin" "$OUTPUT_ROOT/lib" "$OUTPUT_ROOT/share/vulkan/icd.d"

# Rawfile extraction and HAP packaging must not depend on symlink preservation.
cp -L "$loader_binary" "$OUTPUT_ROOT/lib/libvulkan.so.1"
cp -L "$loader_binary" "$OUTPUT_ROOT/lib/libvulkan.so"
cp -L "$MESA_INSTALL/lib/libvulkan_virtio.so" "$OUTPUT_ROOT/lib/libvulkan_virtio.so"

# ICD library_path 决定 loader dlopen 哪个 libvulkan_virtio.so。arm64 下 guest 原生库
# 必须放 el1 bundle (el2 data 区 dlopen 被拒), 用 app 视角绝对路径; x86_64 保留 el2
# bundle 相对路径 (box64 加载)。
if [ "$WINE_ARCH" = "aarch64" ]; then
    ICD_LIBRARY_PATH="/data/storage/el1/bundle/libs/arm64/libvulkan_virtio.so"
else
    ICD_LIBRARY_PATH="../../../lib/libvulkan_virtio.so"
fi
cat > "$OUTPUT_ROOT/share/vulkan/icd.d/venus_icd.$GUEST_ARCH.json" <<EOF
{
  "file_format_version": "1.0.0",
  "ICD": {
    "library_path": "$ICD_LIBRARY_PATH",
    "api_version": "1.3.0"
  }
}
EOF

log "--- winehua_guest_vulkan_smoke ($TARGET) ---"
SHADER_OUTPUT="$OUTPUT_ROOT/share/winehua"
GLSLANG_VALIDATOR="${GLSLANG_VALIDATOR:-glslangValidator}"
mkdir -p "$SHADER_OUTPUT"
for shader in \
    venus_storage_write \
    venus_storage_read \
    venus_image_fetch \
    venus_combined_sample \
    venus_separated_sample \
    venus_depth_array_compare \
    venus_depth_cube_sample \
    venus_depth_cube_compare \
    venus_depth_cube_separated_compare \
    venus_depth_cube_array_sample \
    venus_depth_cube_array_2d_compare \
    venus_depth_cube_array_compare \
    venus_dxvk_contract_sample \
    venus_dxvk_contract_unknown_sample \
    venus_dxvk_contract_spec_sample \
    venus_dxvk_contract_vector_spec_sample; do
    "$GLSLANG_VALIDATOR" -V --target-env vulkan1.1 \
        "$ROOT/smoke/$shader.comp" -o "$SHADER_OUTPUT/$shader.spv" \
        >/dev/null
done
spirv-as --target-env vulkan1.1 \
    "$ROOT/smoke/venus_depth_cube_dxvk_contract_compare.spvasm" \
    -o "$SHADER_OUTPUT/venus_depth_cube_dxvk_contract_compare.spv"
"$GLSLANG_VALIDATOR" -V --target-env vulkan1.1 \
    "$ROOT/smoke/venus_fullscreen_triangle.vert" \
    -o "$SHADER_OUTPUT/venus_fullscreen_triangle.vert.spv" >/dev/null
"$GLSLANG_VALIDATOR" -V --target-env vulkan1.1 \
    "$ROOT/smoke/venus_heaven_material.vert" \
    -o "$SHADER_OUTPUT/venus_heaven_material.vert.spv" >/dev/null
"$GLSLANG_VALIDATOR" -V --target-env vulkan1.1 \
    "$ROOT/smoke/venus_depth_cube_golden.frag" \
    -o "$SHADER_OUTPUT/venus_depth_cube_golden.frag.spv" >/dev/null
# Turn the hand-written fragment Golden into the exact DXVK Legacy image type
# contract: the descriptor variable remains a color Cube image (Depth=0),
# while OpSampledImage uses the depth Cube type (Depth=1).  The compute form of
# this split already passes; this isolates the same contract in fragment stage.
spirv-dis "$SHADER_OUTPUT/venus_depth_cube_golden.frag.spv" \
    -o "$SHADER_OUTPUT/venus_depth_cube_golden_dxvk_contract.spvasm"
awk '
    /OpTypeImage %float Cube 1 0 0 1 Unknown$/ {
        print
        print "%winehua_color_cube = OpTypeImage %float Cube 0 0 0 1 Unknown"
        print "%winehua_ptr_color_cube = OpTypePointer UniformConstant %winehua_color_cube"
        next
    }
    /%sourceDepth = OpVariable/ {
        sub(/%_ptr_UniformConstant_[0-9]+/, "%winehua_ptr_color_cube")
    }
    /OpLoad .* %sourceDepth$/ {
        sub(/OpLoad %[A-Za-z0-9_]+ %sourceDepth/, "OpLoad %winehua_color_cube %sourceDepth")
    }
    { print }
' "$SHADER_OUTPUT/venus_depth_cube_golden_dxvk_contract.spvasm" \
    > "$SHADER_OUTPUT/venus_depth_cube_golden_dxvk_contract.tmp.spvasm"
mv "$SHADER_OUTPUT/venus_depth_cube_golden_dxvk_contract.tmp.spvasm" \
    "$SHADER_OUTPUT/venus_depth_cube_golden_dxvk_contract.spvasm"
spirv-as --target-env vulkan1.1 \
    "$SHADER_OUTPUT/venus_depth_cube_golden_dxvk_contract.spvasm" \
    -o "$SHADER_OUTPUT/venus_depth_cube_golden_dxvk_contract.spv"
spirv-val --target-env vulkan1.1 \
    "$SHADER_OUTPUT/venus_depth_cube_golden_dxvk_contract.spv"
spirv-as --target-env vulkan1.1 \
    "$ROOT/smoke/venus_depth_cube_fail.spvasm" \
    -o "$SHADER_OUTPUT/venus_depth_cube_fail.spv"
spirv-opt -O "$SHADER_OUTPUT/venus_depth_cube_fail.spv" \
    -o "$SHADER_OUTPUT/venus_depth_cube_fail_optimized.spv"
spirv-val --target-env vulkan1.1 \
    "$SHADER_OUTPUT/venus_depth_cube_fail_optimized.spv"
# DXVK emits the minimum vec3 Cube coordinate for OpImageSampleDref*.  The
# glslang Golden keeps the shadow reference as a fourth coordinate component
# as well as the separate Dref operand.  Both validate, so test that exact
# driver-facing representation without changing any other shader behavior.
awk '
    /%320 = OpBitcast %float %uint_1056964608$/ {
        print
        print " %winehua_padded_dref_coord = OpCompositeConstruct %v4float %318 %320"
        next
    }
    /OpImageSampleDrefExplicitLod %float %324 %318 %320 Lod %float_0$/ {
        sub(/%318 %320/, "%winehua_padded_dref_coord %320")
    }
    { print }
' "$ROOT/smoke/venus_depth_cube_fail.spvasm" \
    > "$SHADER_OUTPUT/venus_depth_cube_fail_padded_dref.spvasm"
spirv-as --target-env vulkan1.1 \
    "$SHADER_OUTPUT/venus_depth_cube_fail_padded_dref.spvasm" \
    -o "$SHADER_OUTPUT/venus_depth_cube_fail_padded_dref.spv"
spirv-val --target-env vulkan1.1 \
    "$SHADER_OUTPUT/venus_depth_cube_fail_padded_dref.spv"
# Independently exclude advertised float-control modes from the same exact
# replay; these modes are unrelated to the descriptor/resource contract.
awk '
    /OpCapability DenormFlushToZero$/ { next }
    /OpCapability SignedZeroInfNanPreserve$/ { next }
    /OpExtension "SPV_KHR_float_controls"$/ { next }
    /OpExecutionMode %main DenormFlushToZero 32$/ { next }
    /OpExecutionMode %main SignedZeroInfNanPreserve 32$/ { next }
    { print }
' "$ROOT/smoke/venus_depth_cube_fail.spvasm" \
    > "$SHADER_OUTPUT/venus_depth_cube_fail_no_float_controls.spvasm"
spirv-as --target-env vulkan1.1 \
    "$SHADER_OUTPUT/venus_depth_cube_fail_no_float_controls.spvasm" \
    -o "$SHADER_OUTPUT/venus_depth_cube_fail_no_float_controls.spv"
spirv-val --target-env vulkan1.1 \
    "$SHADER_OUTPUT/venus_depth_cube_fail_no_float_controls.spv"
# Keep DXVK's original coordinate/control-flow lowering, but replace the
# texture operation with RGB encoding of the final Cube direction.  This
# separates bad coordinates from a fragment compiler issue at the Dref sample.
awk '
    /OpDecorate %331 NoContraction$/ { next }
    /%322 = OpLoad %22 %s0$/ {
        print " %winehua_coord_half = OpVectorTimesScalar %v3float %318 %320"
        print " %winehua_coord_bias = OpCompositeConstruct %v3float %320 %320 %320"
        print " %winehua_coord_color = OpFAdd %v3float %winehua_coord_half %winehua_coord_bias"
        print " %winehua_coord_r0 = OpLoad %v4float %r0"
        print " %winehua_coord_out = OpVectorShuffle %v4float %winehua_coord_r0 %winehua_coord_color 4 5 6 3"
        print "               OpStore %r0 %winehua_coord_out"
        skip = 1
        next
    }
    skip && /OpStore %r0 %333$/ {
        skip = 0
        next
    }
    !skip { print }
' "$ROOT/smoke/venus_depth_cube_fail.spvasm" \
    > "$SHADER_OUTPUT/venus_depth_cube_fail_coordinate_trace.spvasm"
spirv-as --target-env vulkan1.1 \
    "$SHADER_OUTPUT/venus_depth_cube_fail_coordinate_trace.spvasm" \
    -o "$SHADER_OUTPUT/venus_depth_cube_fail_coordinate_trace.spv"
spirv-val --target-env vulkan1.1 \
    "$SHADER_OUTPUT/venus_depth_cube_fail_coordinate_trace.spv"
# Diagnostic A/B: keep the captured final DXVK module byte-for-byte in the
# normal replay, and produce a second module whose only semantic difference is
# deterministic initialization of DXVK's private temporary registers.  Some
# mobile fragment compilers propagate undef from the unselected OpSelect arm.
awk '
    { print }
    /%float_0 = OpConstant %float 0$/ {
        print " %winehua_zero_v4 = OpConstantNull %v4float"
    }
    /%14 = OpLabel$/ {
        print "               OpStore %r0 %winehua_zero_v4"
        print "               OpStore %r1 %winehua_zero_v4"
        print "               OpStore %r2 %winehua_zero_v4"
    }
' "$ROOT/smoke/venus_depth_cube_fail.spvasm" \
    > "$SHADER_OUTPUT/venus_depth_cube_fail_initialized.spvasm"
spirv-as --target-env vulkan1.1 \
    "$SHADER_OUTPUT/venus_depth_cube_fail_initialized.spvasm" \
    -o "$SHADER_OUTPUT/venus_depth_cube_fail_initialized.spv"
spirv-val --target-env vulkan1.1 \
    "$SHADER_OUTPUT/venus_depth_cube_fail_initialized.spv"
# Optional diagnostic payload: freeze the currently captured DXVK CS shaders
# so the replay can separate specialization/default handling from generated
# instruction semantics.  This never changes the product DXVK binaries.
FROZEN_OUTPUT="$OUTPUT_ROOT/share/winehua/replay_frozen"
mkdir -p "$FROZEN_OUTPUT"
for replay_shader in "$ROOT"/replay_spv/CS_*.remapped.spv; do
    [ -f "$replay_shader" ] || continue
    replay_name="$(basename "$replay_shader")"
    spirv-opt --freeze-spec-const "$replay_shader" \
        -o "$FROZEN_OUTPUT/$replay_name" >/dev/null 2>&1 || rm -f "$FROZEN_OUTPUT/$replay_name"
done
# Optional Heaven material captures are staged inside the managed Guest
# runtime for deterministic replay.  The source files are generated diagnostic
# artifacts under build/diagnostics and are not part of the product DXVK path.
HEAVEN_OUTPUT="$OUTPUT_ROOT/share/winehua/replay_external"
HEAVEN_INPUT="$ROOT/build/diagnostics/heaven-final"
if [ -f "$HEAVEN_INPUT/heaven_final_vs.spv" ] &&
   [ -f "$HEAVEN_INPUT/heaven_final_fs.spv" ]; then
    mkdir -p "$HEAVEN_OUTPUT"
    cp -L "$HEAVEN_INPUT/heaven_final_vs.spv" "$HEAVEN_OUTPUT/heaven_final_vs.spv"
    cp -L "$HEAVEN_INPUT/heaven_final_fs.spv" "$HEAVEN_OUTPUT/heaven_final_fs.spv"
    [ -f "$HEAVEN_INPUT/heaven_final_fs_no_float.spv" ] &&
        cp -L "$HEAVEN_INPUT/heaven_final_fs_no_float.spv" "$HEAVEN_OUTPUT/heaven_final_fs_no_float.spv"
    [ -f "$HEAVEN_INPUT/heaven_constant.frag.spv" ] &&
        cp -L "$HEAVEN_INPUT/heaven_constant.frag.spv" "$HEAVEN_OUTPUT/heaven_constant.frag.spv"
    [ -f "$HEAVEN_INPUT/heaven_contract_simple.frag.spv" ] &&
        cp -L "$HEAVEN_INPUT/heaven_contract_simple.frag.spv" "$HEAVEN_OUTPUT/heaven_contract_simple.frag.spv"
    [ -f "$HEAVEN_INPUT/heaven_exact_probe.frag.spv" ] &&
        cp -L "$HEAVEN_INPUT/heaven_exact_probe.frag.spv" "$HEAVEN_OUTPUT/heaven_exact_probe.frag.spv"
fi
# Optional exact-draw payload. The source directory is generated under build/
# from a local benchmark capture and is deliberately not version-controlled.
# Keeping this in the managed Guest runtime lets SmokeRunner replay the same
# draw after an overwrite install without relying on HDC sandbox writes.
HEAVEN_EXACT_INPUT="$ROOT/build/diagnostics/heaven-exact-capture"
HEAVEN_EXACT_OUTPUT="$HEAVEN_OUTPUT/heaven_exact"
if [ -f "$HEAVEN_EXACT_INPUT/frame-180-geometry.jsonl" ] &&
   [ -f "$HEAVEN_EXACT_INPUT/frame-180-pass-2-draw-148-index.bin" ] &&
   [ -f "$HEAVEN_EXACT_INPUT/VS_809ef7d3d23ff811f90c51be8d0cfddf6994cdb0.remapped-3d526c03b048bdf1.spv" ] &&
   [ -f "$HEAVEN_EXACT_INPUT/FS_844c52a11b923f49d2afee97a8d6119eef8efeb3.remapped-d5aa1ffafa7f5fa1.spv" ]; then
    mkdir -p "$HEAVEN_EXACT_OUTPUT"
    find "$HEAVEN_EXACT_INPUT" -maxdepth 1 -type f \
        \( -name 'frame-180-pass-2-*' -o -name 'frame-180-geometry.jsonl' \
           -o -name '*.remapped-*.spv' \) \
        -exec cp -L {} "$HEAVEN_EXACT_OUTPUT/" \;
    # Keep two minimal fragment-stage A/B variants next to the exact capture.
    # They are generated from the captured DXVK binary on every guest Vulkan
    # build, so descriptor/resource inputs remain byte-for-byte identical.
    HEAVEN_CAPTURE_FS_ASM="$BUILD_ROOT/heaven_exact_capture_fs.spvasm"
    HEAVEN_NO_KILL_ASM="$BUILD_ROOT/heaven_exact_capture_fs_no_kill.spvasm"
    HEAVEN_FORCE_OPAQUE_ASM="$BUILD_ROOT/heaven_exact_capture_fs_force_opaque.spvasm"
    HEAVEN_T0_IMPLICIT_ASM="$BUILD_ROOT/heaven_exact_capture_fs_t0_implicit.spvasm"
    HEAVEN_T0_LOD0_ASM="$BUILD_ROOT/heaven_exact_capture_fs_t0_lod0.spvasm"
    HEAVEN_EXPLICIT_KILL_INIT_ASM="$BUILD_ROOT/heaven_exact_capture_fs_explicit_kill_init.spvasm"
    HEAVEN_KILL_DIRECT_BIT_ASM="$BUILD_ROOT/heaven_exact_capture_fs_kill_direct_bit.spvasm"
    HEAVEN_KILL_DIRECT_COMPARE_ASM="$BUILD_ROOT/heaven_exact_capture_fs_kill_direct_compare.spvasm"
    HEAVEN_KILL_RECOMPARE_ASM="$BUILD_ROOT/heaven_exact_capture_fs_kill_recompare.spvasm"
    HEAVEN_LOCAL_KILL_ASM="$BUILD_ROOT/heaven_exact_capture_fs_local_kill.spvasm"
    HEAVEN_COMPARE_COLOR_ASM="$BUILD_ROOT/heaven_exact_capture_fs_compare_color.spvasm"
    HEAVEN_INLINE_KILL_ASM="$BUILD_ROOT/heaven_exact_capture_fs_inline_kill.spvasm"
    HEAVEN_UINT_KILL_ASM="$BUILD_ROOT/heaven_exact_capture_fs_uint_kill.spvasm"
    HEAVEN_COMPARE_ALPHA_ASM="$BUILD_ROOT/heaven_exact_capture_fs_compare_alpha.spvasm"
    HEAVEN_NO_FLOAT_ASM="$BUILD_ROOT/heaven_exact_capture_fs_no_float.spvasm"
    HEAVEN_COMPARE_ALPHA_NO_FLOAT_ASM="$BUILD_ROOT/heaven_exact_capture_fs_compare_alpha_no_float.spvasm"
    HEAVEN_SAMPLE_MASK_DISCARD_ASM="$BUILD_ROOT/heaven_exact_capture_fs_sample_mask_discard.spvasm"
    HEAVEN_SAMPLE_MASK_KEEP_ASM="$BUILD_ROOT/heaven_exact_capture_fs_sample_mask_keep.spvasm"
    HEAVEN_RETURN_COMPARE_ASM="$BUILD_ROOT/heaven_exact_capture_fs_return_compare.spvasm"
    HEAVEN_SAMPLE_MASK_FINAL_SELECT_ASM="$BUILD_ROOT/heaven_exact_capture_fs_sample_mask_final_select.spvasm"
    HEAVEN_SAMPLE_MASK_FINAL_BRANCH_ASM="$BUILD_ROOT/heaven_exact_capture_fs_sample_mask_final_branch.spvasm"
    HEAVEN_TERMINATE_INVOCATION_ASM="$BUILD_ROOT/heaven_exact_capture_fs_terminate_invocation.spvasm"
    HEAVEN_KILL_DIRECT_CLEAN_ASM="$BUILD_ROOT/heaven_exact_capture_fs_kill_direct_clean.spvasm"
    HEAVEN_RETURN_COMPARE_CLEAN_ASM="$BUILD_ROOT/heaven_exact_capture_fs_return_compare_clean.spvasm"
    HEAVEN_SAMPLE_MASK_DIRECT_CLEAN_ASM="$BUILD_ROOT/heaven_exact_capture_fs_sample_mask_direct_clean.spvasm"
    spirv-dis \
        "$HEAVEN_EXACT_INPUT/FS_844c52a11b923f49d2afee97a8d6119eef8efeb3.remapped-d5aa1ffafa7f5fa1.spv" \
        -o "$HEAVEN_CAPTURE_FS_ASM"
    # DXVK's binding-presence booleans are already frozen in this exact
    # capture.  Fold the resulting all-true vector selects without applying
    # broad optimization passes so this remains a narrow compiler A/B.
    spirv-opt --ccp --simplify-instructions \
        "$HEAVEN_EXACT_INPUT/FS_844c52a11b923f49d2afee97a8d6119eef8efeb3.remapped-d5aa1ffafa7f5fa1.spv" \
        -o "$HEAVEN_EXACT_OUTPUT/heaven_exact_fs_ccp.spv"
    awk '
        /OpKill$/ { print "               OpBranch %570"; next }
        { print }
    ' "$HEAVEN_CAPTURE_FS_ASM" > "$HEAVEN_NO_KILL_ASM"
    awk '
        /OpStore %ps_kill %/ { print "               OpStore %ps_kill %false"; next }
        { print }
    ' "$HEAVEN_CAPTURE_FS_ASM" > "$HEAVEN_FORCE_OPAQUE_ASM"
    awk '
        /%r7 = OpVariable %_ptr_Private_v4float Private$/ {
            print
            print "%winehua_t0_probe = OpVariable %_ptr_Private_v4float Private"
            next
        }
        /%81 = OpImageSampleImplicitLod %v4float %80 %76$/ {
            print
            print "               OpStore %winehua_t0_probe %81"
            next
        }
        /OpStore %ps_kill %/ {
            print "               OpStore %ps_kill %false"
            next
        }
        /OpStore %o0 %584$/ {
            print "%winehua_t0_value = OpLoad %v4float %winehua_t0_probe"
            print "               OpStore %o0 %winehua_t0_value"
            next
        }
        { print }
    ' "$HEAVEN_CAPTURE_FS_ASM" > "$HEAVEN_T0_IMPLICIT_ASM"
    awk '
        /%r7 = OpVariable %_ptr_Private_v4float Private$/ {
            print
            print "%winehua_t0_probe = OpVariable %_ptr_Private_v4float Private"
            next
        }
        /%81 = OpImageSampleImplicitLod %v4float %80 %76$/ {
            print "         %81 = OpImageSampleExplicitLod %v4float %80 %76 Lod %float_0"
            print "               OpStore %winehua_t0_probe %81"
            next
        }
        /OpStore %ps_kill %/ {
            print "               OpStore %ps_kill %false"
            next
        }
        /OpStore %o0 %584$/ {
            print "%winehua_t0_value = OpLoad %v4float %winehua_t0_probe"
            print "               OpStore %o0 %winehua_t0_value"
            next
        }
        { print }
    ' "$HEAVEN_CAPTURE_FS_ASM" > "$HEAVEN_T0_LOD0_ASM"
    awk '
        /%547 = OpLabel$/ {
            print
            print "               OpStore %ps_kill %false"
            next
        }
        { print }
    ' "$HEAVEN_CAPTURE_FS_ASM" > "$HEAVEN_EXPLICIT_KILL_INIT_ASM"
    awk '
        /%110 = OpLogicalOr %bool %109 %108$/ {
            print "        %110 = OpCopyObject %bool %108"
            next
        }
        { print }
    ' "$HEAVEN_CAPTURE_FS_ASM" > "$HEAVEN_KILL_DIRECT_BIT_ASM"
    awk '
        /%110 = OpLogicalOr %bool %109 %108$/ {
            print "        %110 = OpCopyObject %bool %98"
            next
        }
        { print }
    ' "$HEAVEN_CAPTURE_FS_ASM" > "$HEAVEN_KILL_DIRECT_COMPARE_ASM"
    awk '
        /%110 = OpLogicalOr %bool %109 %108$/ {
            print "        %110 = OpFOrdLessThanEqual %bool %97 %95"
            next
        }
        { print }
    ' "$HEAVEN_CAPTURE_FS_ASM" > "$HEAVEN_KILL_RECOMPARE_ASM"
    awk '
        /%_ptr_Private_bool = OpTypePointer Private %bool$/ {
            print
            print "%_ptr_Function_bool = OpTypePointer Function %bool"
            next
        }
        /%ps_kill = OpVariable %_ptr_Private_bool Private %false$/ { next }
        /%14 = OpLabel$/ {
            print
            print "    %ps_kill = OpVariable %_ptr_Function_bool Function %false"
            next
        }
        /OpStore %o0 %546$/ {
            print
            print "%winehua_kill_state = OpLoad %bool %ps_kill"
            print "               OpSelectionMerge %winehua_kill_end None"
            print "               OpBranchConditional %winehua_kill_state %winehua_kill_if %winehua_kill_end"
            print "%winehua_kill_if = OpLabel"
            print "               OpKill"
            print "%winehua_kill_end = OpLabel"
            next
        }
        /%571 = OpLoad %bool %ps_kill$/ { skip_main_kill = 1; next }
        skip_main_kill {
            if (/%570 = OpLabel$/) skip_main_kill = 0
            next
        }
        { print }
    ' "$HEAVEN_CAPTURE_FS_ASM" > "$HEAVEN_LOCAL_KILL_ASM"
    awk '
        /OpStore %ps_kill %110$/ {
            print "               OpStore %ps_kill %false"
            next
        }
        /OpStore %o0 %546$/ {
            print "%winehua_compare_red = OpSelect %float %98 %float_1 %float_0"
            print "%winehua_compare_green = OpSelect %float %98 %float_0 %float_1"
            print "%winehua_compare_color = OpCompositeConstruct %v4float %winehua_compare_red %winehua_compare_green %float_0 %float_1"
            print "               OpStore %o0 %winehua_compare_color"
            next
        }
        { print }
    ' "$HEAVEN_CAPTURE_FS_ASM" > "$HEAVEN_COMPARE_COLOR_ASM"
    awk '
        /%98 = OpFOrdGreaterThanEqual %bool %95 %97$/ {
            print
            print "               OpSelectionMerge %winehua_inline_kill_end None"
            print "               OpBranchConditional %98 %winehua_inline_kill_if %winehua_inline_kill_end"
            print "%winehua_inline_kill_if = OpLabel"
            print "               OpKill"
            print "%winehua_inline_kill_end = OpLabel"
            next
        }
        /OpStore %ps_kill %110$/ {
            print "               OpStore %ps_kill %false"
            next
        }
        { print }
    ' "$HEAVEN_CAPTURE_FS_ASM" > "$HEAVEN_INLINE_KILL_ASM"
    awk '
        /%_ptr_Private_bool = OpTypePointer Private %bool$/ {
            print
            print "%winehua_ptr_Private_uint = OpTypePointer Private %uint"
            print "%winehua_uint_0 = OpConstant %uint 0"
            print "%winehua_uint_1 = OpConstant %uint 1"
            next
        }
        /%ps_kill = OpVariable %_ptr_Private_bool Private %false$/ {
            print "    %ps_kill = OpVariable %winehua_ptr_Private_uint Private %winehua_uint_0"
            next
        }
        /%109 = OpLoad %bool %ps_kill$/ {
            print "        %109 = OpLoad %uint %ps_kill"
            next
        }
        /%110 = OpLogicalOr %bool %109 %108$/ {
            print "        %110 = OpSelect %uint %108 %winehua_uint_1 %winehua_uint_0"
            print "%winehua_kill_uint = OpBitwiseOr %uint %109 %110"
            next
        }
        /OpStore %ps_kill %110$/ {
            print "               OpStore %ps_kill %winehua_kill_uint"
            next
        }
        /%571 = OpLoad %bool %ps_kill$/ {
            print "        %571 = OpLoad %uint %ps_kill"
            print "%winehua_kill_test = OpINotEqual %bool %571 %winehua_uint_0"
            next
        }
        /OpBranchConditional %571 %569 %570$/ {
            print "               OpBranchConditional %winehua_kill_test %569 %570"
            next
        }
        { print }
    ' "$HEAVEN_CAPTURE_FS_ASM" > "$HEAVEN_UINT_KILL_ASM"
    awk '
        /OpStore %ps_kill %110$/ {
            print "               OpStore %ps_kill %false"
            next
        }
        /OpStore %o0 %546$/ {
            print "%winehua_compare_bit = OpSelect %float %98 %float_1 %float_0"
            print "%winehua_compare_alpha = OpCompositeConstruct %v4float %97 %winehua_compare_bit %float_0 %float_1"
            print "               OpStore %o0 %winehua_compare_alpha"
            next
        }
        { print }
    ' "$HEAVEN_CAPTURE_FS_ASM" > "$HEAVEN_COMPARE_ALPHA_ASM"
    awk '
        /OpCapability DenormFlushToZero$/ { next }
        /OpCapability SignedZeroInfNanPreserve$/ { next }
        /OpExtension "SPV_KHR_float_controls"$/ { next }
        /OpExecutionMode %main DenormFlushToZero 32$/ { next }
        /OpExecutionMode %main SignedZeroInfNanPreserve 32$/ { next }
        { print }
    ' "$HEAVEN_CAPTURE_FS_ASM" > "$HEAVEN_NO_FLOAT_ASM"
    awk '
        /OpCapability DenormFlushToZero$/ { next }
        /OpCapability SignedZeroInfNanPreserve$/ { next }
        /OpExtension "SPV_KHR_float_controls"$/ { next }
        /OpExecutionMode %main DenormFlushToZero 32$/ { next }
        /OpExecutionMode %main SignedZeroInfNanPreserve 32$/ { next }
        /OpStore %ps_kill %110$/ {
            print "               OpStore %ps_kill %false"
            next
        }
        /OpStore %o0 %546$/ {
            print "%winehua_compare_bit = OpSelect %float %98 %float_1 %float_0"
            print "%winehua_compare_alpha = OpCompositeConstruct %v4float %97 %winehua_compare_bit %float_0 %float_1"
            print "               OpStore %o0 %winehua_compare_alpha"
            next
        }
        { print }
    ' "$HEAVEN_CAPTURE_FS_ASM" > "$HEAVEN_COMPARE_ALPHA_NO_FLOAT_ASM"
    # Maleoon reports shaderDemoteToHelperInvocation=false and intermittently
    # executes the captured shader's final OpKill when its condition is false.
    # Preserve DXVK Legacy's deferred-discard contract by converting the one
    # captured discard bit to fragment coverage: 0 discards all samples and
    # -1 preserves them. This exact-replay A/B is diagnostic until it passes
    # repeatedly and the behavior is qualified for D3D11 depth/MSAA semantics.
    awk '
        /OpEntryPoint Fragment %main "main"/ {
            print $0 " %winehua_sample_mask"
            next
        }
        /OpName %ps_frag_coord "ps_frag_coord"$/ {
            print
            print "               OpName %winehua_sample_mask \"winehua_sample_mask\""
            next
        }
        /OpDecorate %o0 Location 0$/ {
            print
            print "               OpDecorate %winehua_sample_mask BuiltIn SampleMask"
            next
        }
        /%uint_1 = OpConstant %uint 1$/ {
            print
            print "%winehua_sample_mask_array = OpTypeArray %int %uint_1"
            print "%winehua_sample_mask_ptr = OpTypePointer Output %winehua_sample_mask_array"
            print "%winehua_sample_mask_value_ptr = OpTypePointer Output %int"
            print "%winehua_mask_keep = OpConstant %int -1"
            next
        }
        /%ps_kill = OpVariable %_ptr_Private_bool Private %false$/ {
            print
            print "%winehua_sample_mask = OpVariable %winehua_sample_mask_ptr Output"
            next
        }
        /%98 = OpFOrdGreaterThanEqual %bool %95 %97$/ {
            print
            print "%winehua_mask_value = OpSelect %int %98 %int_0 %winehua_mask_keep"
            print "%winehua_mask_element = OpAccessChain %winehua_sample_mask_value_ptr %winehua_sample_mask %uint_0"
            print "               OpStore %winehua_mask_element %winehua_mask_value"
            next
        }
        /OpStore %ps_kill %110$/ {
            print "               OpStore %ps_kill %false"
            next
        }
        /OpKill$/ {
            print "               OpBranch %570"
            next
        }
        { print }
    ' "$HEAVEN_CAPTURE_FS_ASM" > "$HEAVEN_SAMPLE_MASK_DISCARD_ASM"
    # Keep the SampleMask output interface and control-flow shape identical to
    # the discard replacement, but force full coverage. This distinguishes a
    # broken SampleMask output path from a broken dynamically selected mask.
    awk '
        /%winehua_mask_value = OpSelect %int %98 %int_0 %winehua_mask_keep$/ {
            print "%winehua_mask_value = OpCopyObject %int %winehua_mask_keep"
            next
        }
        { print }
    ' "$HEAVEN_SAMPLE_MASK_DISCARD_ASM" > "$HEAVEN_SAMPLE_MASK_KEEP_ASM"
    # Return the real alpha comparison directly from ps_main and branch on the
    # function result in the entry point. This removes the cross-function
    # Private ps_kill state while retaining the original OpKill semantics.
    awk '
        /OpName %ps_kill "ps_kill"$/ { next }
        /%bool = OpTypeBool$/ {
            print
            print "%winehua_bool_function = OpTypeFunction %bool"
            next
        }
        /%ps_kill = OpVariable %_ptr_Private_bool Private %false$/ { next }
        /%ps_main = OpFunction %void None %13$/ {
            print "    %ps_main = OpFunction %bool None %winehua_bool_function"
            in_ps_main = 1
            next
        }
        in_ps_main && /%109 = OpLoad %bool %ps_kill$/ { next }
        in_ps_main && /%110 = OpLogicalOr %bool %109 %108$/ { next }
        in_ps_main && /OpStore %ps_kill %110$/ { next }
        in_ps_main && /OpReturn$/ {
            print "               OpReturnValue %98"
            next
        }
        in_ps_main && /OpFunctionEnd$/ {
            in_ps_main = 0
            print
            next
        }
        /%568 = OpFunctionCall %void %ps_main$/ {
            print "        %568 = OpFunctionCall %bool %ps_main"
            next
        }
        /%571 = OpLoad %bool %ps_kill$/ { next }
        /OpBranchConditional %571 %569 %570$/ {
            print "               OpBranchConditional %568 %569 %570"
            next
        }
        { print }
    ' "$HEAVEN_CAPTURE_FS_ASM" > "$HEAVEN_RETURN_COMPARE_ASM"
    # Replace DXVK's final deferred OpKill with a SampleMask write driven by
    # the same accumulated ps_kill value. This preserves all earlier discard
    # sites and tests the shape a general Legacy translator quirk would use.
    awk '
        /OpEntryPoint Fragment %main "main"/ {
            print $0 " %winehua_sample_mask"
            next
        }
        /OpName %ps_frag_coord "ps_frag_coord"$/ {
            print
            print "               OpName %winehua_sample_mask \"winehua_sample_mask\""
            next
        }
        /OpDecorate %o0 Location 0$/ {
            print
            print "               OpDecorate %winehua_sample_mask BuiltIn SampleMask"
            next
        }
        /%uint_1 = OpConstant %uint 1$/ {
            print
            print "%winehua_sample_mask_array = OpTypeArray %int %uint_1"
            print "%winehua_sample_mask_ptr = OpTypePointer Output %winehua_sample_mask_array"
            print "%winehua_sample_mask_value_ptr = OpTypePointer Output %int"
            print "%winehua_mask_keep = OpConstant %int -1"
            next
        }
        /%ps_kill = OpVariable %_ptr_Private_bool Private %false$/ {
            print
            print "%winehua_sample_mask = OpVariable %winehua_sample_mask_ptr Output"
            next
        }
        /%547 = OpLabel$/ {
            print
            print "               OpStore %ps_kill %false"
            next
        }
        /%571 = OpLoad %bool %ps_kill$/ {
            print
            print "%winehua_mask_value = OpSelect %int %571 %int_0 %winehua_mask_keep"
            print "%winehua_mask_element = OpAccessChain %winehua_sample_mask_value_ptr %winehua_sample_mask %uint_0"
            print "               OpStore %winehua_mask_element %winehua_mask_value"
            next
        }
        /OpSelectionMerge %570 None$/ { next }
        /OpBranchConditional %571 %569 %570$/ {
            print "               OpBranch %570"
            next
        }
        /%569 = OpLabel$/ { next }
        /OpKill$/ { next }
        { print }
    ' "$HEAVEN_CAPTURE_FS_ASM" > "$HEAVEN_SAMPLE_MASK_FINAL_SELECT_ASM"
    # Use structured control flow and constant stores instead of OpSelect for
    # the final mask. This isolates Maleoon's bool-to-int selection lowering.
    awk '
        /OpEntryPoint Fragment %main "main"/ {
            print $0 " %winehua_sample_mask"
            next
        }
        /OpName %ps_frag_coord "ps_frag_coord"$/ {
            print
            print "               OpName %winehua_sample_mask \"winehua_sample_mask\""
            next
        }
        /OpDecorate %o0 Location 0$/ {
            print
            print "               OpDecorate %winehua_sample_mask BuiltIn SampleMask"
            next
        }
        /%uint_1 = OpConstant %uint 1$/ {
            print
            print "%winehua_sample_mask_array = OpTypeArray %int %uint_1"
            print "%winehua_sample_mask_ptr = OpTypePointer Output %winehua_sample_mask_array"
            print "%winehua_sample_mask_value_ptr = OpTypePointer Output %int"
            print "%winehua_mask_keep = OpConstant %int -1"
            next
        }
        /%ps_kill = OpVariable %_ptr_Private_bool Private %false$/ {
            print
            print "%winehua_sample_mask = OpVariable %winehua_sample_mask_ptr Output"
            next
        }
        /%547 = OpLabel$/ {
            print
            print "               OpStore %ps_kill %false"
            next
        }
        /%571 = OpLoad %bool %ps_kill$/ {
            print
            print "               OpSelectionMerge %winehua_mask_merge None"
            print "               OpBranchConditional %571 %winehua_mask_discard %winehua_mask_keep_block"
            print "%winehua_mask_discard = OpLabel"
            print "%winehua_mask_discard_element = OpAccessChain %winehua_sample_mask_value_ptr %winehua_sample_mask %uint_0"
            print "               OpStore %winehua_mask_discard_element %int_0"
            print "               OpBranch %winehua_mask_merge"
            print "%winehua_mask_keep_block = OpLabel"
            print "%winehua_mask_keep_element = OpAccessChain %winehua_sample_mask_value_ptr %winehua_sample_mask %uint_0"
            print "               OpStore %winehua_mask_keep_element %winehua_mask_keep"
            print "               OpBranch %winehua_mask_merge"
            print "%winehua_mask_merge = OpLabel"
            next
        }
        /OpSelectionMerge %570 None$/ { next }
        /OpBranchConditional %571 %569 %570$/ {
            print "               OpBranch %570"
            next
        }
        /%569 = OpLabel$/ { next }
        /OpKill$/ { next }
        { print }
    ' "$HEAVEN_CAPTURE_FS_ASM" > "$HEAVEN_SAMPLE_MASK_FINAL_BRANCH_ASM"
    # VK_KHR_shader_terminate_invocation provides the exact fragment
    # termination semantic through a different Host compiler opcode.
    awk '
        /OpExtension "SPV_KHR_float_controls"$/ {
            print
            print "               OpExtension \"SPV_KHR_terminate_invocation\""
            next
        }
        /OpKill$/ {
            print "               OpTerminateInvocation"
            next
        }
        { print }
    ' "$HEAVEN_CAPTURE_FS_ASM" > "$HEAVEN_TERMINATE_INVOCATION_ASM"
    # Generate the discard bool directly from the ordered alpha comparison.
    # The skipped block is DXVK Legacy's uint-mask -> float -> uint -> bool
    # round trip; no later instruction observes its temporary r0 value.
    awk '
        /%101 = OpSelect %uint %98 %uint_4294967295 %uint_0$/ {
            print "        %109 = OpLoad %bool %ps_kill"
            print "        %110 = OpLogicalOr %bool %109 %98"
            print "               OpStore %ps_kill %110"
            skip_discard_round_trip = 1
            next
        }
        skip_discard_round_trip {
            if (/OpStore %ps_kill %110$/) skip_discard_round_trip = 0
            next
        }
        { print }
    ' "$HEAVEN_CAPTURE_FS_ASM" > "$HEAVEN_KILL_DIRECT_CLEAN_ASM"
    # Apply the same removal to the function-return experiment so the return
    # value is the only remaining representation of the discard decision.
    awk '
        /%101 = OpSelect %uint %98 %uint_4294967295 %uint_0$/ {
            skip_discard_round_trip = 1
            next
        }
        skip_discard_round_trip {
            if (/%108 = OpINotEqual %bool %107 %uint_0$/)
                skip_discard_round_trip = 0
            next
        }
        { print }
    ' "$HEAVEN_RETURN_COMPARE_ASM" > "$HEAVEN_RETURN_COMPARE_CLEAN_ASM"
    # Keep only compare -> SampleMask for the coverage replacement. Remove the
    # dead Private kill state and its final no-op control-flow diamond.
    awk '
        /OpName %ps_kill "ps_kill"$/ { next }
        /%ps_kill = OpVariable %_ptr_Private_bool Private %false$/ { next }
        /%101 = OpSelect %uint %98 %uint_4294967295 %uint_0$/ {
            skip_discard_round_trip = 1
            next
        }
        skip_discard_round_trip {
            if (/OpStore %ps_kill %false$/) skip_discard_round_trip = 0
            next
        }
        /%571 = OpLoad %bool %ps_kill$/ { next }
        /OpSelectionMerge %570 None$/ { next }
        /OpBranchConditional %571 %569 %570$/ {
            print "               OpBranch %570"
            next
        }
        /%569 = OpLabel$/ {
            skip_dead_kill_block = 1
            next
        }
        skip_dead_kill_block {
            if (/%570 = OpLabel$/) {
                skip_dead_kill_block = 0
                print
            }
            next
        }
        { print }
    ' "$HEAVEN_SAMPLE_MASK_DISCARD_ASM" > "$HEAVEN_SAMPLE_MASK_DIRECT_CLEAN_ASM"
    spirv-as --target-env vulkan1.1 "$HEAVEN_NO_KILL_ASM" \
        -o "$HEAVEN_EXACT_OUTPUT/heaven_exact_fs_no_kill.spv"
    spirv-as --target-env vulkan1.1 "$HEAVEN_FORCE_OPAQUE_ASM" \
        -o "$HEAVEN_EXACT_OUTPUT/heaven_exact_fs_force_opaque.spv"
    spirv-as --target-env vulkan1.1 "$HEAVEN_T0_IMPLICIT_ASM" \
        -o "$HEAVEN_EXACT_OUTPUT/heaven_exact_fs_t0_implicit.spv"
    spirv-as --target-env vulkan1.1 "$HEAVEN_T0_LOD0_ASM" \
        -o "$HEAVEN_EXACT_OUTPUT/heaven_exact_fs_t0_lod0.spv"
    spirv-as --target-env vulkan1.1 "$HEAVEN_EXPLICIT_KILL_INIT_ASM" \
        -o "$HEAVEN_EXACT_OUTPUT/heaven_exact_fs_explicit_kill_init.spv"
    spirv-as --target-env vulkan1.1 "$HEAVEN_KILL_DIRECT_BIT_ASM" \
        -o "$HEAVEN_EXACT_OUTPUT/heaven_exact_fs_kill_direct_bit.spv"
    spirv-as --target-env vulkan1.1 "$HEAVEN_KILL_DIRECT_COMPARE_ASM" \
        -o "$HEAVEN_EXACT_OUTPUT/heaven_exact_fs_kill_direct_compare.spv"
    spirv-as --target-env vulkan1.1 "$HEAVEN_KILL_RECOMPARE_ASM" \
        -o "$HEAVEN_EXACT_OUTPUT/heaven_exact_fs_kill_recompare.spv"
    spirv-as --target-env vulkan1.1 "$HEAVEN_LOCAL_KILL_ASM" \
        -o "$HEAVEN_EXACT_OUTPUT/heaven_exact_fs_local_kill.spv"
    spirv-as --target-env vulkan1.1 "$HEAVEN_COMPARE_COLOR_ASM" \
        -o "$HEAVEN_EXACT_OUTPUT/heaven_exact_fs_compare_color.spv"
    spirv-as --target-env vulkan1.1 "$HEAVEN_INLINE_KILL_ASM" \
        -o "$HEAVEN_EXACT_OUTPUT/heaven_exact_fs_inline_kill.spv"
    spirv-as --target-env vulkan1.1 "$HEAVEN_UINT_KILL_ASM" \
        -o "$HEAVEN_EXACT_OUTPUT/heaven_exact_fs_uint_kill.spv"
    spirv-as --target-env vulkan1.1 "$HEAVEN_COMPARE_ALPHA_ASM" \
        -o "$HEAVEN_EXACT_OUTPUT/heaven_exact_fs_compare_alpha.spv"
    spirv-as --target-env vulkan1.1 "$HEAVEN_NO_FLOAT_ASM" \
        -o "$HEAVEN_EXACT_OUTPUT/heaven_exact_fs_no_float.spv"
    spirv-as --target-env vulkan1.1 "$HEAVEN_COMPARE_ALPHA_NO_FLOAT_ASM" \
        -o "$HEAVEN_EXACT_OUTPUT/heaven_exact_fs_compare_alpha_no_float.spv"
    spirv-as --target-env vulkan1.1 "$HEAVEN_SAMPLE_MASK_DISCARD_ASM" \
        -o "$HEAVEN_EXACT_OUTPUT/heaven_exact_fs_sample_mask_discard.spv"
    spirv-as --target-env vulkan1.1 "$HEAVEN_SAMPLE_MASK_KEEP_ASM" \
        -o "$HEAVEN_EXACT_OUTPUT/heaven_exact_fs_sample_mask_keep.spv"
    spirv-as --target-env vulkan1.1 "$HEAVEN_RETURN_COMPARE_ASM" \
        -o "$HEAVEN_EXACT_OUTPUT/heaven_exact_fs_return_compare.spv"
    spirv-opt --inline-entry-points-exhaustive --eliminate-dead-functions \
        "$HEAVEN_EXACT_INPUT/FS_844c52a11b923f49d2afee97a8d6119eef8efeb3.remapped-d5aa1ffafa7f5fa1.spv" \
        -o "$HEAVEN_EXACT_OUTPUT/heaven_exact_fs_inlined.spv"
    spirv-opt --inline-entry-points-exhaustive --eliminate-dead-functions \
        --private-to-local \
        "$HEAVEN_EXACT_INPUT/FS_844c52a11b923f49d2afee97a8d6119eef8efeb3.remapped-d5aa1ffafa7f5fa1.spv" \
        -o "$HEAVEN_EXACT_OUTPUT/heaven_exact_fs_inlined_local.spv"
    spirv-as --target-env vulkan1.1 "$HEAVEN_SAMPLE_MASK_FINAL_SELECT_ASM" \
        -o "$HEAVEN_EXACT_OUTPUT/heaven_exact_fs_sample_mask_final_select.spv"
    spirv-as --target-env vulkan1.1 "$HEAVEN_SAMPLE_MASK_FINAL_BRANCH_ASM" \
        -o "$HEAVEN_EXACT_OUTPUT/heaven_exact_fs_sample_mask_final_branch.spv"
    spirv-as --target-env vulkan1.1 "$HEAVEN_TERMINATE_INVOCATION_ASM" \
        -o "$HEAVEN_EXACT_OUTPUT/heaven_exact_fs_terminate_invocation.spv"
    spirv-as --target-env vulkan1.1 "$HEAVEN_KILL_DIRECT_CLEAN_ASM" \
        -o "$HEAVEN_EXACT_OUTPUT/heaven_exact_fs_kill_direct_clean.spv"
    spirv-as --target-env vulkan1.1 "$HEAVEN_RETURN_COMPARE_CLEAN_ASM" \
        -o "$HEAVEN_EXACT_OUTPUT/heaven_exact_fs_return_compare_clean.spv"
    spirv-as --target-env vulkan1.1 "$HEAVEN_SAMPLE_MASK_DIRECT_CLEAN_ASM" \
        -o "$HEAVEN_EXACT_OUTPUT/heaven_exact_fs_sample_mask_direct_clean.spv"
    # Omap0 (SpecId 1216) controls DXVK's final output-component mapping. Bake
    # its captured 0x3210 value to isolate Guest->Host specialization transport
    # and Host specialization compilation from the rest of each shader.
    spirv-opt --set-spec-const-default-value "1216:12816" --freeze-spec-const \
        "$HEAVEN_EXACT_INPUT/FS_844c52a11b923f49d2afee97a8d6119eef8efeb3.remapped-d5aa1ffafa7f5fa1.spv" \
        -o "$HEAVEN_EXACT_OUTPUT/heaven_exact_fs_omap_frozen.spv"
    spirv-opt --set-spec-const-default-value "1216:12816" --freeze-spec-const \
        "$HEAVEN_EXACT_OUTPUT/heaven_exact_fs_no_kill.spv" \
        -o "$HEAVEN_EXACT_OUTPUT/heaven_exact_fs_no_kill_omap_frozen.spv"
    spirv-opt --set-spec-const-default-value "1216:12816" --freeze-spec-const \
        "$HEAVEN_EXACT_OUTPUT/heaven_exact_fs_kill_direct_clean.spv" \
        -o "$HEAVEN_EXACT_OUTPUT/heaven_exact_fs_kill_direct_clean_omap_frozen.spv"
    spirv-opt --set-spec-const-default-value "1216:12816" --freeze-spec-const \
        "$HEAVEN_EXACT_OUTPUT/heaven_exact_fs_return_compare_clean.spv" \
        -o "$HEAVEN_EXACT_OUTPUT/heaven_exact_fs_return_compare_clean_omap_frozen.spv"
    spirv-opt --set-spec-const-default-value "1216:12816" --freeze-spec-const \
        "$HEAVEN_EXACT_OUTPUT/heaven_exact_fs_sample_mask_direct_clean.spv" \
        -o "$HEAVEN_EXACT_OUTPUT/heaven_exact_fs_sample_mask_direct_clean_omap_frozen.spv"
    spirv-val --target-env vulkan1.1 \
        "$HEAVEN_EXACT_OUTPUT/heaven_exact_fs_no_kill.spv"
    spirv-val --target-env vulkan1.1 \
        "$HEAVEN_EXACT_OUTPUT/heaven_exact_fs_force_opaque.spv"
    spirv-val --target-env vulkan1.1 \
        "$HEAVEN_EXACT_OUTPUT/heaven_exact_fs_t0_implicit.spv"
    spirv-val --target-env vulkan1.1 \
        "$HEAVEN_EXACT_OUTPUT/heaven_exact_fs_t0_lod0.spv"
    spirv-val --target-env vulkan1.1 \
        "$HEAVEN_EXACT_OUTPUT/heaven_exact_fs_explicit_kill_init.spv"
    spirv-val --target-env vulkan1.1 \
        "$HEAVEN_EXACT_OUTPUT/heaven_exact_fs_kill_direct_bit.spv"
    spirv-val --target-env vulkan1.1 \
        "$HEAVEN_EXACT_OUTPUT/heaven_exact_fs_kill_direct_compare.spv"
    spirv-val --target-env vulkan1.1 \
        "$HEAVEN_EXACT_OUTPUT/heaven_exact_fs_kill_recompare.spv"
    spirv-val --target-env vulkan1.1 \
        "$HEAVEN_EXACT_OUTPUT/heaven_exact_fs_local_kill.spv"
    spirv-val --target-env vulkan1.1 \
        "$HEAVEN_EXACT_OUTPUT/heaven_exact_fs_compare_color.spv"
    spirv-val --target-env vulkan1.1 \
        "$HEAVEN_EXACT_OUTPUT/heaven_exact_fs_inline_kill.spv"
    spirv-val --target-env vulkan1.1 \
        "$HEAVEN_EXACT_OUTPUT/heaven_exact_fs_uint_kill.spv"
    spirv-val --target-env vulkan1.1 \
        "$HEAVEN_EXACT_OUTPUT/heaven_exact_fs_compare_alpha.spv"
    spirv-val --target-env vulkan1.1 \
        "$HEAVEN_EXACT_OUTPUT/heaven_exact_fs_no_float.spv"
    spirv-val --target-env vulkan1.1 \
        "$HEAVEN_EXACT_OUTPUT/heaven_exact_fs_compare_alpha_no_float.spv"
    spirv-val --target-env vulkan1.1 \
        "$HEAVEN_EXACT_OUTPUT/heaven_exact_fs_sample_mask_discard.spv"
    spirv-val --target-env vulkan1.1 \
        "$HEAVEN_EXACT_OUTPUT/heaven_exact_fs_sample_mask_keep.spv"
    spirv-val --target-env vulkan1.1 \
        "$HEAVEN_EXACT_OUTPUT/heaven_exact_fs_return_compare.spv"
    spirv-val --target-env vulkan1.1 \
        "$HEAVEN_EXACT_OUTPUT/heaven_exact_fs_inlined.spv"
    spirv-val --target-env vulkan1.1 \
        "$HEAVEN_EXACT_OUTPUT/heaven_exact_fs_inlined_local.spv"
    spirv-val --target-env vulkan1.1 \
        "$HEAVEN_EXACT_OUTPUT/heaven_exact_fs_sample_mask_final_select.spv"
    spirv-val --target-env vulkan1.1 \
        "$HEAVEN_EXACT_OUTPUT/heaven_exact_fs_sample_mask_final_branch.spv"
    spirv-val --target-env vulkan1.1 \
        "$HEAVEN_EXACT_OUTPUT/heaven_exact_fs_terminate_invocation.spv"
    spirv-val --target-env vulkan1.1 \
        "$HEAVEN_EXACT_OUTPUT/heaven_exact_fs_kill_direct_clean.spv"
    spirv-val --target-env vulkan1.1 \
        "$HEAVEN_EXACT_OUTPUT/heaven_exact_fs_return_compare_clean.spv"
    spirv-val --target-env vulkan1.1 \
        "$HEAVEN_EXACT_OUTPUT/heaven_exact_fs_sample_mask_direct_clean.spv"
    spirv-val --target-env vulkan1.1 \
        "$HEAVEN_EXACT_OUTPUT/heaven_exact_fs_omap_frozen.spv"
    spirv-val --target-env vulkan1.1 \
        "$HEAVEN_EXACT_OUTPUT/heaven_exact_fs_no_kill_omap_frozen.spv"
    spirv-val --target-env vulkan1.1 \
        "$HEAVEN_EXACT_OUTPUT/heaven_exact_fs_kill_direct_clean_omap_frozen.spv"
    spirv-val --target-env vulkan1.1 \
        "$HEAVEN_EXACT_OUTPUT/heaven_exact_fs_return_compare_clean_omap_frozen.spv"
    spirv-val --target-env vulkan1.1 \
        "$HEAVEN_EXACT_OUTPUT/heaven_exact_fs_sample_mask_direct_clean_omap_frozen.spv"
    spirv-val --target-env vulkan1.1 \
        "$HEAVEN_EXACT_OUTPUT/heaven_exact_fs_ccp.spv"
fi
# 鸿蒙沙箱不支持 exec: guest 程序编译为 .so 共享库 (不再是 -pie 可执行),
# 统一导出入口 winehua_guest_program_main (main 经 -Dmain=... 重命名),
# wine_child.cpp guestElfMode 以 dlopen + dlsym 加载 (与 hostElfMode 同模式)。
"$CLANG" --target="$TARGET" --sysroot="$SYSROOT" \
    -std=c11 -O2 -shared -fPIC -fno-emulated-tls \
    -Dmain=winehua_guest_program_main \
    -I"$HEADERS_INSTALL/include" \
    "$ROOT/smoke/guest_vulkan_smoke.c" \
    -L"$LOADER_INSTALL/lib" -Wl,-rpath,'$ORIGIN/../lib:$ORIGIN' \
    -Wl,--enable-new-dtags -lvulkan -ldl -lpthread \
    -o "$OUTPUT_ROOT/bin/libwinehua_guest_vulkan_smoke.so"
"$CLANG" --target="$TARGET" --sysroot="$SYSROOT" \
    -std=c11 -O2 -shared -fPIC -fno-emulated-tls \
    -Dmain=winehua_guest_program_main \
    -I"$HEADERS_INSTALL/include" \
    "$ROOT/smoke/venus_sampled_image_probe.c" \
    -L"$LOADER_INSTALL/lib" -Wl,-rpath,'$ORIGIN/../lib:$ORIGIN' \
    -Wl,--enable-new-dtags -lvulkan -ldl -lpthread \
    -o "$OUTPUT_ROOT/bin/libvenus_sampled_image_probe.so"
"$CLANG" --target="$TARGET" --sysroot="$SYSROOT" \
    -std=c11 -O2 -shared -fPIC -fno-emulated-tls \
    -Dmain=winehua_guest_program_main \
    -I"$HEADERS_INSTALL/include" \
    "$ROOT/smoke/venus_spirv_replay.c" \
    -L"$LOADER_INSTALL/lib" -Wl,-rpath,'$ORIGIN/../lib:$ORIGIN' \
    -Wl,--enable-new-dtags -lvulkan -ldl -lpthread \
    -o "$OUTPUT_ROOT/bin/libvenus_spirv_replay.so"
"$CLANG" --target="$TARGET" --sysroot="$SYSROOT" \
    -std=c11 -O2 -shared -fPIC -fno-emulated-tls \
    -Dmain=winehua_guest_program_main \
    -I"$HEADERS_INSTALL/include" \
    "$ROOT/smoke/venus_heaven_material_replay.c" \
    -L"$LOADER_INSTALL/lib" -Wl,-rpath,'$ORIGIN/../lib:$ORIGIN' \
    -Wl,--enable-new-dtags -lvulkan -ldl -lpthread \
    -o "$OUTPUT_ROOT/bin/libvenus_heaven_material_replay.so"

# Optional exact replay of the first Heaven pass-2 material draw. This is a
# generated diagnostic payload and is only staged when a local capture exists;
# it never changes the product Vulkan path.
HEAVEN_DRAW0_INPUT="$ROOT/build/diagnostics/heaven-draw0-capture"
HEAVEN_DRAW0_OUTPUT="$HEAVEN_OUTPUT/heaven_draw0"
if [ -f "$HEAVEN_DRAW0_INPUT/frame-180-pass-2-draw-0-index.bin" ] &&
   [ -f "$HEAVEN_DRAW0_INPUT/FS_56289d3e1ccd04a77c3d954c5ea8fe76a545a831.remapped.spv" ] &&
   [ -f "$HEAVEN_DRAW0_INPUT/VS_809ef7d3d23ff811f90c51be8d0cfddf6994cdb0.remapped.spv" ]; then
    mkdir -p "$HEAVEN_DRAW0_OUTPUT"
    find "$HEAVEN_DRAW0_INPUT" -maxdepth 1 -type f \
        \( -name 'frame-180-pass-2-*' -o -name 'frame-180-geometry.jsonl' \) \
        -exec cp -L {} "$HEAVEN_DRAW0_OUTPUT/" \;
    cp -L "$HEAVEN_DRAW0_INPUT/FS_56289d3e1ccd04a77c3d954c5ea8fe76a545a831.remapped.spv" \
        "$HEAVEN_DRAW0_OUTPUT/FS_56289d3e1ccd04a77c3d954c5ea8fe76a545a831.remapped.spv"
    cp -L "$HEAVEN_DRAW0_INPUT/VS_809ef7d3d23ff811f90c51be8d0cfddf6994cdb0.remapped.spv" \
        "$HEAVEN_DRAW0_OUTPUT/VS_809ef7d3d23ff811f90c51be8d0cfddf6994cdb0.remapped.spv"
fi

# Exact replay of the selected pass-2 draw where the current scene first
# diverges. Keep it separate from draw0 so both captures remain reproducible.
HEAVEN_DRAW170_INPUT="$ROOT/build/diagnostics/heaven-draw170-capture"
HEAVEN_DRAW170_OUTPUT="$HEAVEN_OUTPUT/heaven_draw170"
HEAVEN_DRAW170_VS="VS_809ef7d3d23ff811f90c51be8d0cfddf6994cdb0.remapped-3d526c03b048bdf1.spv"
HEAVEN_DRAW170_FS="FS_e47a0b705fa8d6f25bab50a322b14606f914dac6.remapped-6f2ed2660be66fad.spv"
if [ -f "$HEAVEN_DRAW170_INPUT/frame-180-pass-2-draw-170-index.bin" ] &&
   [ -f "$HEAVEN_DRAW170_INPUT/$HEAVEN_DRAW170_FS" ] &&
   [ -f "$HEAVEN_DRAW170_INPUT/$HEAVEN_DRAW170_VS" ]; then
    mkdir -p "$HEAVEN_DRAW170_OUTPUT"
    find "$HEAVEN_DRAW170_INPUT" -maxdepth 1 -type f \
        \( -name 'frame-180-pass-2-*' -o -name 'frame-180-geometry.jsonl' \) \
        -exec cp -L {} "$HEAVEN_DRAW170_OUTPUT/" \;
    cp -L "$HEAVEN_DRAW170_INPUT/$HEAVEN_DRAW170_FS" \
        "$HEAVEN_DRAW170_OUTPUT/$HEAVEN_DRAW170_FS"
    cp -L "$HEAVEN_DRAW170_INPUT/$HEAVEN_DRAW170_VS" \
        "$HEAVEN_DRAW170_OUTPUT/$HEAVEN_DRAW170_VS"
fi

# Exact replay of the additive pass-2 FS_f647 material. This capture exercises
# seven sampled-image bindings, a D24 comparison source, an image alias, and the
# EQUAL/depth-write-off pipeline state used by the real draw.
HEAVEN_F647_INPUT="$ROOT/build/diagnostics/heaven-f647-capture"
HEAVEN_F647_OUTPUT="$HEAVEN_OUTPUT/heaven_f647"
HEAVEN_F647_VS="VS_fa0f746828dc66ef12b425928c234654b310a37e.remapped-1eded4254f7290.spv"
HEAVEN_F647_FS="FS_f64724cbe909fa7080a3f266cec16847ec90bb92.remapped-4a2c265ca77b091e.spv"
if [ -f "$HEAVEN_F647_INPUT/frame-180-pass-2-draw-390-index.bin" ] &&
   [ -f "$HEAVEN_F647_INPUT/$HEAVEN_F647_FS" ] &&
   [ -f "$HEAVEN_F647_INPUT/$HEAVEN_F647_VS" ]; then
    mkdir -p "$HEAVEN_F647_OUTPUT"
    find "$HEAVEN_F647_INPUT" -maxdepth 1 -type f \
        \( -name 'frame-180-pass-2-*' -o -name 'frame-180-geometry.jsonl' \) \
        -exec cp -L {} "$HEAVEN_F647_OUTPUT/" \;
    cp -L "$HEAVEN_F647_INPUT/$HEAVEN_F647_FS" \
        "$HEAVEN_F647_OUTPUT/$HEAVEN_F647_FS"
    cp -L "$HEAVEN_F647_INPUT/$HEAVEN_F647_VS" \
        "$HEAVEN_F647_OUTPUT/$HEAVEN_F647_VS"
fi

# Exact frame-220 G-buffer and HDR/material depth pair. These profiles use the
# captured vertex/index/UBO inputs with a diagnostic fragment shader that writes
# gl_FragCoord.z/w, allowing Lavapipe and Maleoon to be compared without
# changing the DXVK product path.
HEAVEN_MATERIAL_DEPTH_INPUT="$ROOT/build/diagnostics/heaven-material-depth"
HEAVEN_MATERIAL_DEPTH_OUTPUT="$HEAVEN_OUTPUT/heaven_material_depth"
if [ -f "$HEAVEN_MATERIAL_DEPTH_INPUT/depth.frag.spv" ] &&
   [ -f "$HEAVEN_MATERIAL_DEPTH_INPUT/pass0/vs.spv" ] &&
   [ -f "$HEAVEN_MATERIAL_DEPTH_INPUT/pass3/vs.spv" ] &&
   [ -f "$HEAVEN_MATERIAL_DEPTH_INPUT/pass0/frame-180-pass-2-draw-2-index.bin" ] &&
   [ -f "$HEAVEN_MATERIAL_DEPTH_INPUT/pass3/frame-180-pass-2-draw-2-index.bin" ]; then
    mkdir -p "$HEAVEN_MATERIAL_DEPTH_OUTPUT"
    cp -L "$HEAVEN_MATERIAL_DEPTH_INPUT/depth.frag.spv" \
        "$HEAVEN_MATERIAL_DEPTH_OUTPUT/depth.frag.spv"
    for material_depth_pass in pass0 pass3; do
        mkdir -p "$HEAVEN_MATERIAL_DEPTH_OUTPUT/$material_depth_pass"
        find "$HEAVEN_MATERIAL_DEPTH_INPUT/$material_depth_pass" -maxdepth 1 -type f \
            -exec cp -L {} "$HEAVEN_MATERIAL_DEPTH_OUTPUT/$material_depth_pass/" \;
        spirv-dis "$HEAVEN_MATERIAL_DEPTH_INPUT/$material_depth_pass/vs.spv" \
            -o "$HEAVEN_MATERIAL_DEPTH_OUTPUT/$material_depth_pass/vs-no-float-controls.spvasm"
        awk '
            /OpCapability DenormFlushToZero$/ { next }
            /OpCapability SignedZeroInfNanPreserve$/ { next }
            /OpExtension "SPV_KHR_float_controls"$/ { next }
            /OpExecutionMode .* DenormFlushToZero 32$/ { next }
            /OpExecutionMode .* SignedZeroInfNanPreserve 32$/ { next }
            { print }
        ' "$HEAVEN_MATERIAL_DEPTH_OUTPUT/$material_depth_pass/vs-no-float-controls.spvasm" \
            > "$HEAVEN_MATERIAL_DEPTH_OUTPUT/$material_depth_pass/vs-no-float-controls.tmp.spvasm"
        mv "$HEAVEN_MATERIAL_DEPTH_OUTPUT/$material_depth_pass/vs-no-float-controls.tmp.spvasm" \
            "$HEAVEN_MATERIAL_DEPTH_OUTPUT/$material_depth_pass/vs-no-float-controls.spvasm"
        spirv-as --target-env vulkan1.1 \
            "$HEAVEN_MATERIAL_DEPTH_OUTPUT/$material_depth_pass/vs-no-float-controls.spvasm" \
            -o "$HEAVEN_MATERIAL_DEPTH_OUTPUT/$material_depth_pass/vs-no-float-controls.spv"
        spirv-val --target-env vulkan1.1 \
            "$HEAVEN_MATERIAL_DEPTH_OUTPUT/$material_depth_pass/vs-no-float-controls.spv"
    done
fi

loader_sha="$(sha256sum "$OUTPUT_ROOT/lib/libvulkan.so.1" | awk '{print $1}')"
icd_sha="$(sha256sum "$OUTPUT_ROOT/lib/libvulkan_virtio.so" | awk '{print $1}')"
mesa_commit="$(git -c safe.directory="$ROOT/thirdparty/mesa" -C "$ROOT/thirdparty/mesa" rev-parse HEAD)"
smoke_sha="$(sha256sum "$OUTPUT_ROOT/bin/libwinehua_guest_vulkan_smoke.so" | awk '{print $1}')"
probe_sha="$(sha256sum "$OUTPUT_ROOT/bin/libvenus_sampled_image_probe.so" | awk '{print $1}')"
replay_sha="$(sha256sum "$OUTPUT_ROOT/bin/libvenus_spirv_replay.so" | awk '{print $1}')"
heaven_replay_sha="$(sha256sum "$OUTPUT_ROOT/bin/libvenus_heaven_material_replay.so" | awk '{print $1}')"
cat > "$OUTPUT_ROOT/manifest.json" <<EOF
{
  "schemaVersion": 1,
  "runtimeVersion": "phase2-venus-b1-v1",
  "architecture": "$TARGET",
  "loaderVersion": "$LOADER_TAG",
  "loaderCommit": "$LOADER_COMMIT",
  "headersVersion": "$HEADERS_TAG",
  "headersCommit": "$HEADERS_COMMIT",
  "guestMesaVersion": "$(cat "$ROOT/thirdparty/mesa/VERSION")",
  "guestMesaCommit": "$mesa_commit",
  "transportRequirements": {
    "remoteMemoryShadow": true,
    "multiRing": false,
    "fenceFeedback": false,
    "queryFeedback": false,
    "semaphoreFeedback": true,
    "modernRequiresSynchronousTimelineQueries": true
  },
  "files": {
    "bin/libwinehua_guest_vulkan_smoke.so": "$smoke_sha",
    "bin/libvenus_sampled_image_probe.so": "$probe_sha",
    "bin/libvenus_spirv_replay.so": "$replay_sha",
    "bin/libvenus_heaven_material_replay.so": "$heaven_replay_sha",
    "lib/libvulkan.so.1": "$loader_sha",
    "lib/libvulkan_virtio.so": "$icd_sha"
  }
}
EOF

cat > "$OUTPUT_ROOT/BUILD_INFO.txt" <<EOF
arch=$TARGET
loader_tag=$LOADER_TAG
loader_commit=$LOADER_COMMIT
headers_tag=$HEADERS_TAG
headers_commit=$HEADERS_COMMIT
mesa_commit=$mesa_commit
built_at=$(date -u +%Y-%m-%dT%H:%M:%SZ)
EOF

file "$OUTPUT_ROOT/bin/libwinehua_guest_vulkan_smoke.so" \
    "$OUTPUT_ROOT/bin/libvenus_heaven_material_replay.so" "$OUTPUT_ROOT/lib/libvulkan.so.1" \
    "$OUTPUT_ROOT/lib/libvulkan_virtio.so"
log "guest Vulkan runtime ready: $OUTPUT_ROOT"
