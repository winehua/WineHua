# Runtime components

The ARM64 product worktree is the build and library reference for the
direct-game media runtime. Its 2026-09-09 signed native HAP has the GStreamer
core libraries and `winegstreamer`, but no loadable GStreamer plugin modules.
The Proton-OHOS package retains that core closure and completes it with the
locked plugin set in Wine's loader domain:

| Component | Version or form | Package location |
| --- | --- | --- |
| GStreamer core and support libraries | 1.24.4, including codec parsers and MPEG-TS | Native HAP libraries for native Wine, or `wine-data.zip/bin/<wine-arch>-unix` for Box64 Wine |
| GStreamer plugins | 76 plugins from the locked GStreamer 1.24.4 build | `wine-data.zip/bin/<wine-arch>-unix/gstreamer-1.0` |
| FFmpeg used by `gst-libav` | libavcodec 60, libavformat 60, libavutil 58 and companion libraries | Same loader domain as the GStreamer core |
| TLS | GnuTLS, nettle, hogweed, GMP, libtasn1 and libunistring | Same loader domain as Wine, used by schannel/WinHTTP/WinInet |
| Wine network APIs | `dnsapi`, `crypt32`, `schannel`, `winhttp` and `wininet` for primary and i386 PE architectures | `wine-data.zip/bin/*-windows` |
| Platform gate smokes | x64/i386 DNS/network and cross-bitness process/IPC probes | `wine-data.zip/smoke`, suites `platform-network` and `platform-process` |
| Desktop and input | Wayland client/EGL, xkbcommon, xkbregistry, libffi, FreeType and XKB data | Native HAP libraries plus `wine-data.zip/share/X11/xkb` |
| x86/x64 execution on ARM64 Wine | `libwow64fex.dll` and `libarm64ecfex.dll`; Box64 WoW64 remains an explicit fallback | `wine-data.zip/bin/aarch64-windows` alongside primary and i386 Wine PE DLLs |
| Wine media bridge | `winegstreamer.dll`, `mf.dll`, `mfplat.dll`, `mfreadwrite.dll`, `quartz.dll`, `devenum.dll` and `wmvcore.dll` for the primary Windows architecture and i386 | `wine-data.zip/bin/*-windows` |
| Wine Mono | 11.1.0, SHA-256 `deb0341431f8260b209fff6bc79ddcc5414b97f8e9236ab9fbdca4ce59e0a9b9`; opt-in only | `wine-data.zip/share/wine/mono` when `BUILD_WINE_MONO=1` |
| D3D11 runtime policy | DXVK 1.10.3 is the default; DXVK 2.6.2 is an explicit optional profile | `wine-data.zip/dxvk/{legacy,modern-2.6}` |
| VKD3D-Proton | Retained packaged library only; no product UI, Want, NAPI, program-launch or smoke route activates it | `wine-data.zip/vkd3d/limited-500k/x64/d3d12.dll` |
| Fonts | Wine font payload | `wine-data.zip/share/wine/fonts` |

`wine_env.cpp` supplies `GST_PLUGIN_PATH` and `GST_PLUGIN_SYSTEM_PATH` for the
per-architecture plugin directory. The build requires the GStreamer core,
common playback and container plugins, H.264 parser, and `gst-libav`; it does
not accept an archive that only contains the Unix libraries.

Wine Gecko is deliberately not in this direct-game profile, matching the
ARM64 reference package. It is not a replacement for Steam or CEF and must be
introduced only with a separate Web-runtime integration and test plan.

DXVK 1.10.3 is the default D3D11 path for direct games and Host-installed
titles. DXVK 2.6.2 remains a user-selected compatibility profile. VKD3D-Proton
stays in the payload so a later qualified port does not need a library-layout
change, but all product launch boundaries reject `vkd3d_limited_500k`; a
historical `d3d12` smoke request is rejected as well. This keeps the archived
VKD3D assets from changing DXVK performance or being selected by a persisted
preference, external Want, or per-program request.

The default direct-game HAP also excludes Wine Mono, matching the ARM64
reference. Wine's first-boot Mono installer opens a modal window and previously
blocked `wineboot` on device. `BUILD_WINE_MONO=1` creates an explicit managed
runtime experiment package with the locked MSI and `appwiz.cpl`; it is not a
supported profile until the installer has a verified device-side completion
path. The component validator accepts either manifest state and verifies the
MSI checksum whenever Mono is bundled.

The assembly step validates the unpacked staging tree and then invokes:

```sh
python3 scripts/check_runtime_components.py --payload entry/src/main/resources/rawfile/wine-data.zip
```

The HAP packaging step repeats the validation against the signed archive:

```sh
python3 scripts/check_runtime_components.py --hap entry/build/default/outputs/default/entry-default-signed.hap
```

Both checks verify the component manifest, the default DXVK 1.10.3 and optional
DXVK 2.6.2 overlays, the packaged-but-disabled VKD3D-Proton manifest, and exact 76-module plugin inventory,
the Wine Mono checksum when enabled, matching primary/i386 Media Foundation and GStreamer PE bridges, default ARM64 FEX
translators, fonts, XKB data, and the complete GStreamer/FFmpeg, desktop/input
and GnuTLS library closures visible in the HAP. The signed-HAP check also parses
each media ELF's `DT_NEEDED` entries and rejects an unresolved dependency outside
the Wine payload and packaged native libraries.

## Candidate-to-HAP mapping

HarmonyOS release packaging strips native ELF debug and section-table metadata,
so a signed HAP library's whole-file SHA-256 is intentionally different from
the matching file in `entry/libs`. `scripts/w1-verify-candidate.sh --hap <hap>`
therefore performs three provenance checks after the ordinary candidate checks:

1. `wine-data.zip` in the signed HAP has the exact SHA-256 of the rawfile
   assembled for this candidate.
2. `wineohos.so`, `win32u.so`, `winewayland.so`, `ntdll.so`, and
   `libwineserver.so` in `entry/libs/arm64-v8a` are byte-identical to their
   current Wine build outputs.
3. Each packaged native library has the same ELF program-header layout and
   normalized `PT_LOAD` contents as that build output. Only ELF section-table
   fields are normalized, because they are not mapped by the runtime loader
   and are the fields changed by release stripping.

`scripts/w1-m3-build-hap.sh` runs this validation automatically after the HAP
has been signed. A build is not a candidate for device deployment until this
step passes.
