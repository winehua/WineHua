# WineHua Phase 2 DXVK Status Memo

> Last updated: 2026-08-01
>
> Purpose: this is the durable handoff for resuming the DXVK investigation.
> Read this file before changing DXVK, Venus present, SmokeRunner, or game launch
> code. Update it whenever a conclusion, gate result, commit, HAP, or primary
> blocker changes.

## 2026-07-30 DXVK 1.10.3 stable baseline

`dxvk_legacy` is the Phase 2 user-facing baseline. It uses DXVK 1.10.3 over
Wine Vulkan, x86_64 Venus, and the Host Vulkan broker presenter. WineD3D/VirGL
remains the explicit fallback; Modern DXVK and DirectPresent are not part of
this baseline.

The product profile is:

    shadow-precise-dirty-ring-inline-upload-coverage-sort

It keeps the required precise dirty shadow contract, remote-memory ring
publication, inline GPU upload, and coverage ordering. It is not a generic
"debug" profile and must remain the default for DXVK Legacy launches until a
replacement passes the same gates.

The tested signed HAP is:

    HAP SHA-256:
      f18e279a9e4973aae3144112319ea1cef42841fd4a7f4f5ff8d0bab96ce486db
    wine-data SHA-256:
      d56c9308858b37e53fae0c7a46a9b7780fb953ab298504a452a6ee5077e8dced
    archive:
      D:\MyProject\winehua-logs\automation\release-dxvk-1.10.3-20260730

Physical-device evidence:

- 910 tablet, `release-stable-910-20260730`: full `dxvk` suite PASS.
- 920 phone, `release-stable-920-20260730`: full `dxvk` suite PASS.
- 910 tablet, `release-logquiet-910-20260730`: full `dxvk` suite PASS with
  no periodic vtest/present/fence/blob/busy-wait log lines.
- 910 tablet, `release-timeline-fixed-910-20260730`: full `dxvk` suite PASS
  while Host `WineHuaFrameTimeline` and sampled vtest summaries are present.

The stable suite validates x86 WoW64 and x64 DXVK loading from the managed
runtime, Feature Level 11.0, Win32 present, no WineD3D fallback, BC sampling,
mips/arrays/cube views, depth/stencil, MSAA resolve, MRT, compute/UAV,
descriptor rebinding/lifetime, 3D textures, and a 675-frame cube sequence with
`angleRegressions=0`. It records zero CPU full-frame readback/upload and zero
per-frame `vkDeviceWaitIdle`.

### Product logging and diagnostics

Normal product runs set `WINEHUA_VTEST_PRESENT_PERF_SUMMARY=0`. This suppresses
periodic present, submit, fence, busy-wait, and successful blob lifecycle
records that previously wrote and flushed the Host log during normal play.
Initialization, teardown, protocol/submit/resource failures, and fence
regressions remain logged. Resource identity logging remains available under
the explicit capture profile through `WINEHUA_RESOURCE_TRACE=1`.

`shadow-precise-dirty-ring-frame-timeline` is a diagnostic-only profile. It
inherits the same Guest DXVK precise-shadow and strong-ring-barrier contract as
the product profile, then enables sparse Host frame timelines and vtest
summaries. Do not remove the SmokeRunner environment inheritance for this
profile: doing so silently changes the Guest shadow semantics and causes valid
texture/3D/UAV probes to fail.

### Release boundary

This baseline is suitable for user selection and controlled game testing. It
does not prove universal DX11 compatibility: untested games may require new
format, shader, synchronization, Box64, or Wine work. Preserve the per-game
fallback to WineD3D, collect the affected smoke/log archive before changing a
product default, and run the stable suite on both architectures after every
DXVK, Mesa, virglrenderer, Wine Vulkan, or presenter change.

## 2026-07-28 SNORM default, DLL search fix, and Crysis 3 boundary

`DXVK_WINEHUA_EMULATE_RGBA8_SNORM_RT=auto` is now part of the default
`dxvk_legacy` environment. DXVK keeps the native `RGBA8_SNORM` render-target
path when the Host device supports it; on devices such as Maleoon where the
format can be sampled but not used as a color attachment, DXVK may select its
qualified `RGBA16F` backing image. A process can still set the variable to `0`
for an exact native-path A/B. The user confirmed that this default fixes Tomb
Raider's dark rendering and also improves its frame rate. Wine child startup
logs now record the selected mode.

The current installed and regression-tested artifact is:

    HAP SHA-256:
      69c271f1afb4eb3c0520d0db1a8bf00531ed4b3fa7f3d33b820aa4b69631b22e
    wine-data SHA-256:
      17e261ff8a3753c48424c6f1619378c47e81a5b082f1bea9c7862aadc7a1b3f5
    device ntdll.so SHA-256:
      f0f0ca1bdb902df6964dd92e7888e84f762d85bce52a4c5da124ad51967155ba
    automation archive:
      D:\MyProject\winehua-logs\automation\phase2-20260728-191205

The full `dxvk + reuse prefix` gate passed for x64 and x86. It covered D3D11
feature level 11.0, texture and descriptor matrices, subresources, 3D textures,
compute/UAV, depth/stencil, BC formats, fixed-frame presentation, and the Cube
sequence. The Cube result was 521 frames, `angleRegressions=0`, and
`fallbackDetected=false`.

Crysis 3 Remastered must be launched from the real path:

    Z:\games\Remastered\Crysis3Remastered\Bin64\Crysis3Remastered.exe

Its first failure, `0xC0000135`, was a WineHua OHOS loader bug. OHOS replaced
the normal process `DllPath` with case-sensitive Unix `WINEDLLPATH` entries, so
uppercase imports such as `WINMM.dll`, `WININET.dll`, and `DINPUT8.dll` did not
resolve even though the files existed. Wine now appends
`C:\windows\system32;C:\windows` after the managed DXVK paths. This preserves
DXVK priority while restoring normal Windows system-DLL resolution. After the
fix, DXVK `dxgi.dll`, `vulkan-1.dll`, and `winevulkan.dll` all load.

The remaining terminal failure is inside the game's own `steam_api64.dll`:

    steam_api64.dll + 0x282321: EXCEPTION_ACCESS_VIOLATION
    detach + 0x1a7000: EXCEPTION_ILLEGAL_INSTRUCTION

The DLL has protected/self-modifying `WUS0` and `WUS1` sections. The same game
files run on Windows, so this is a WineHua compatibility failure rather than an
invalid-installation conclusion. `BOX64_AVX=1` and `BOX64_AVX=2` reach the same
addresses and fail identically, excluding AVX feature selection.

A controlled Box64 matrix identified the minimum condition. The WineHua
performance environment had forced `BOX64_DYNAREC_SAFEFLAGS=0`, overriding
Box64's compatibility default. `BIGBLOCK=0` and `CALLRET=0` alone still fail at
the same `steam_api64.dll + 0x282321` address. `SAFEFLAGS=1` alone preserves the
required x86 flag semantics, passes DLL initialization, creates DXVK, renders
the CryEngine splash, and reaches the Crysis 3 Remastered title screen.
`SAFEFLAGS=2` also works but is unnecessary. The product default is therefore
restored to `SAFEFLAGS=1`; explicit per-process overrides remain available for
performance diagnosis. No Steam API bypass or fake result is used.

Evidence is archived at:

    D:\MyProject\winehua-logs\crysis3-20260728

## 0. Visual correctness ledger and non-regression rule

As of 2026-07-27, the command-list-owned mapped-flush build recorded in section
36 is the first fully archived, user-confirmed `KNOWN_GOOD` Heaven baseline. Its
HAP SHA-256 is
`4cb5722fff73e2b16112a05cbc6b9d440deb818f7e6dd35a96f5ac311dd0d457`.
The user continuously confirmed correct grass, terrain, stone, and building
materials with no backward camera-angle frames. The earlier statement that no
recoverable baseline existed applied to the historical incident in sections
19-24; those candidates remain `UNKNOWN` or `REJECTED` and must not replace
this baseline.

This exposed a process failure: performance improvements, sparse screenshot
checks, and temporary visual observations were allowed to advance without
first creating an immutable correctness milestone. From now on:

1. `KNOWN_GOOD` requires an archived signed HAP, HAP/wine-data/runtime DLL
   hashes, main and all changed submodule commits, exact profile/environment,
   machine-readable logs, Cube `angleRegressions=0`, and the user's continuous
   Heaven verdict.
2. A candidate without all of that is `UNKNOWN`, even if one observation looks
   correct. User-observed rollback immediately marks it `REJECTED`.
3. No newer package may replace a `KNOWN_GOOD` device install until its archive
   and restore command have been verified.
4. Correctness and performance are separate gates. FPS, monotonic present
   serials, unique hashes, and low-rate screenshots cannot prove that camera
   motion is rollback-free.
5. Every root-cause boundary, rejected hypothesis, HAP identity, visual verdict,
   and next experiment is added here and committed before the next behavior
   change.


## Handoff scope note (2026-07-31)

The dated investigation sections originally following this point (史前调查日志、“Current blocker”、“Current conclusion”、编号章节 1-9 及 11-40)
have been archived to docs/archive/PHASE2_DXVK_STATUS_MEMO_sections_01-40.md
to keep this file as the durable handoff summary. Consult the archive for historical A/B details, rejected hypotheses, and per-date evidence.
## 10. Resume checklist and commands

First read this memo and inspect changes:

    git status --short --branch &&
    git submodule status &&
    git -C thirdparty/dxvk status --short --branch

Run the known DXVK regression suite (pure WSL, no Windows PowerShell;
see automation/README.md for prerequisites):

    python3 automation/run_regression.py --suite dxvk --prefix reuse

Run the Phase-2 entry gate (three reuse-prefix core runs and one clean-prefix
core run):

    python3 automation/run_regression.py --gate

Run the capability matrix (Host Vulkan vs Venus):

    python3 automation/run_regression.py --suite capabilities

Launch the packaged visible D3D11 cube through normal game mode (manual
visual check after the suite passes):

    hdc shell aa start -a EntryAbility -b app.hackeris.winehua
    # 桌面 Explorer 中运行 C:\smoke\x64\winehua_d3d_switch_cube.exe

Before trusting a rebuilt package:

1. Build with the local Makefile (`make NATIVE_ARCH=arm64-v8a`) and verify the
   build exit code and signed HAP timestamp.
2. Verify HAP SHA-256 and embedded wine-data payload (run_regression.py does
   this as part of every run; check `<archive>/artifact.json`).
3. Verify guest Vulkan/DRI binaries are x86-64 and host libraries are AArch64.
4. Install through the Linux hdc and require install bundle successfully.
5. Capture a physical device screenshot and archive logs.
