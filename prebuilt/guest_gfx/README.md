Place the guest 3D receiver runtime bundle here per arch:

- `prebuilt/guest_gfx/x86_64/`
- `prebuilt/guest_gfx/arm64-v8a/`

The assembly step copies that directory into the HNP runtime as `wine/bin/guest_gfx/`.

Required file:

- `winehua-guest-gfx.env`

Recommended entrypoint:

- `bash scripts/build_ohos_guest_gfx.sh --platform wayland --mode virpipe`
- or, if you already have an install tree:
  `bash scripts/build_guest_gfx.sh --install-root <ohos-mesa-install> --mode virpipe`

Suggested layout:

```text
prebuilt/guest_gfx/x86_64/
  winehua-guest-gfx.env
  BUILD_INFO.txt
  lib/
    libEGL.so
    libGLESv2.so
    dri/
      virtio_gpu_dri.so
      zink_dri.so
      ...
```

`BuildWineEnv()` automatically prepends `guest_gfx/lib` to the runtime library path.
Use `winehua-guest-gfx.env` for the remaining receiver-side variables, for example:

```sh
WINEHUA_GUEST_GFX_MODE=mesa-virpipe
WINEHUA_GUEST_GFX_PLATFORM=wayland
LIBGL_ALWAYS_SOFTWARE=1
MESA_LOADER_DRIVER_OVERRIDE=swrast
GALLIUM_DRIVER=virpipe
LIBGL_DRIVERS_PATH=$ORIGIN/lib/dri
EGL_DRIVERS_PATH=$ORIGIN/lib/egl
```

For the vtest transport, `GraphicsBroker` injects `VTEST_SOCKET_NAME` at launch time.
That keeps the bundle relocatable while still pointing Mesa's `virpipe` guest driver
at the runtime-created VirGL socket.

Preferred source roots:

- `thirdparty/mesa-ohos`
- `thirdparty/libdrm-ohos`

`scripts/build_ohos_guest_gfx.sh` now prefers those managed child repos first.
If they are missing, the script falls back to temporary clones under `tmp/`.

Fallback source fetch:

- `bash scripts/fetch_ohos_mesa.sh`
- Mesa repo: `https://gitee.com/openharmony/third_party_mesa3d.git`
- libdrm repo: `https://gitee.com/openharmony/third_party_libdrm.git`

For reproducible fallback fetches, pin exact refs:

```sh
bash scripts/fetch_ohos_mesa.sh \
  --mesa-ref <mesa-commit-or-tag> \
  --libdrm-ref <libdrm-commit-or-tag>
```

`BUILD_INFO.txt` is generated during packaging and records:

- bundle arch / mode / platform / build timestamp
- Mesa source root + Git HEAD, when available
- libdrm source root + Git HEAD, when available

Current receiver-side finding:

- Wine's OpenGL path in this tree uses `winewayland.drv`, which initializes
  `EGL_PLATFORM_WAYLAND_KHR`.
- That means the practical Step 1 guest bundle should be built for the Wayland
  EGL platform first.
- `platform_ohos.c` is still relevant for direct native OHOS EGL experiments,
  but it is not the main path for the Windows guest smoke test.

This bundle is the receiver-side compatibility layer. The goal is to let Wine's stock
`OpenGL / DX -> EGL / wined3d` path resolve against bundled Mesa-style user-space drivers
instead of requiring any changes in the Windows application.

Current focus:

- Step 1 OpenGL smoke: `mesa-virpipe`
- Later GLES/Vulkan/DX compatibility experiments: `mesa-zink`
