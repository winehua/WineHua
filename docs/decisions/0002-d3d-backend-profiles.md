# WineHua DXVK Modern Upgrade Readiness

> Updated: 2026-08-10

> Purpose: record the independent DXVK and VKD3D capability decisions. DXVK
> 2.6.2 is already adapted and game-validated on Maleoon 920; this document
> must not downgrade that result because a separate VKD3D profile has stricter
> descriptor requirements.

## 1. Current product decision

DXVK and VKD3D are selected independently. On Maleoon 920, the WineHua DXVK
2.6.2 profile has passed D3D11 smoke and real D3D11 workload validation. Its
observed game compatibility is not lower than the 1.10.x path. It is not a
transport-only candidate and must not be described as one.

DXVK 1.10.3 remains the qualified fallback for devices that cannot satisfy the
DXVK 2.6.2 Vulkan 1.3 and robustness2 contract, including Maleoon 910. Selecting
VKD3D-Proton 2.6 limited-500K for D3D12 does not by itself require selecting
DXVK 1.10.3 for D3D11.

```text
DXVK 2.6.2 on qualified 920 devices
  -> Wine Vulkan
  -> x86_64 Mesa Venus
  -> virglrenderer Venus
  -> Host Harmony Vulkan
  -> Venus BrokerPresent
```

The managed 2.6.2 DLLs are the WineHua-adapted build, not an unmodified upstream
drop. Profiles are selected before process startup and retain this fallback
order:

```text
DXVK_MODERN -> DXVK_LEGACY -> WineD3D
```

Switching DLLs inside a running Wine process is invalid. A failed Modern test
is recorded as a failure even if a separate Legacy restart succeeds.

## 2. Capability evidence captured on 2026-07-30

The device identity below is the Host Vulkan device name. The Guest identity is
the corresponding Venus adapter.

| Capability | 9010 / Maleoon 910 | 9020 / Maleoon 920 | Upgrade interpretation |
| --- | --- | --- | --- |
| Host API version | 1.2.275 | 1.3.309 | Only 920 reaches the DXVK 2.x API baseline. |
| Venus Guest API version | 1.2.275 | 1.3.269 | 920 exposes Vulkan 1.3 to Wine/DXVK. |
| `VK_EXT_robustness2` | no | yes | DXVK 2.x requires robust buffer access and null descriptors. |
| `synchronization2` | yes | yes | Already available on both; Modern uses the newer submission path. |
| `dynamicRendering` | no | yes | 920 can execute the Modern render path. |
| `maintenance4` / `maintenance5` | no | yes | Useful to newer DXVK, but not sufficient alone. |
| `VK_EXT_transform_feedback` | no | yes, `geometryStreams=0` | Ordinary stream output is possible; multi-stream remains incomplete. |
| `VK_EXT_extended_dynamic_state` | no | yes | Current Legacy DXVK already enables it on 920. |
| `VK_EXT_vertex_attribute_divisor` | no | yes | Current Legacy DXVK already enables it on 920. |
| `VK_EXT_shader_demote_to_helper_invocation` | no | yes | Current Legacy DXVK already enables it on 920. |
| Descriptor indexing | Host yes, Venus no | Host yes, Venus no | Not required by DXVK 2.6.2; remains a blocker for newer descriptor-indexing-dependent profiles. |
| ETC2 / ASTC | Host yes, Venus no | Host yes, Venus no | Not a direct replacement for DXGI BC data. |
| BC1-BC7 | no | no | WineHua BC decode/emulation remains necessary. |
| Buffer device address | not qualified | Venus reports yes | Present in the current 920 transport capture; not by itself sufficient for DXVK 2.7+. |

Primary evidence:

```text
D:\MyProject\winehua-logs\automation\capabilities-pad-910-20260730\phase2-20260730-130554\capability-matrix.json
D:\MyProject\winehua-logs\automation\capabilities-phone-920-title-20260730\phase2-20260730-131706\capability-matrix.json
```

The 920 DXVK runtime log proves that the installed Legacy runtime enables
`VK_EXT_robustness2`, `VK_EXT_extended_dynamic_state`,
`VK_EXT_shader_demote_to_helper_invocation`, `VK_EXT_transform_feedback`, and
`VK_EXT_vertex_attribute_divisor`. The 910 log shows these as unavailable. This
is capability-driven behavior, not a device-name performance special case.

### DXVK 2.6.2 transport qualification, 2026-07-31

The versioned `dxvk26-requirements` smoke suite now executes the actual three
Vulkan paths relevant to a Modern Wine runtime:

```text
Guest Linux loader -> Venus ICD
Windows x86 -> winevulkan -> x86_64 loader -> Venus ICD
Windows x64 -> winevulkan -> x86_64 loader -> Venus ICD
```

All three passed on Maleoon 920. The Guest loader is `1.3.290`, the Venus
adapter is `1.3.269`, and all paths successfully created a device requesting
Vulkan 1.3 synchronization2, dynamic rendering, maintenance4, and all three
`VK_EXT_robustness2` features: robust buffer access, robust image access, and
null descriptors. This was the initial transport checkpoint; it was later
superseded by the adapted DXVK 2.6.2 D3D11 and game runs in section 5.

The captured adapter still reports these upstream D3D11 baseline gaps:

```text
textureCompressionBC      = 0, BC1 through BC7 = 0
dualSrcBlend              = 0
multiViewport             = 0
transformFeedback         = 1, geometryStreams = 0
```

The suite summary is retained under the run id
`dxvk26req-20260731-0900`. Its next gate is an unmodified DXVK 2.6.2
`D3D11CreateDevice` attempt, whose log is the authority for the exact
rejection set.

## 3. What Vulkan 1.3 changes today

Vulkan 1.3 does not by itself increase frame rate. A feature must be exposed by
Host Vulkan, passed through Venus, enabled by DXVK, and exercised by the game.

On 920, Legacy DXVK already benefits where it can from null descriptors,
extended dynamic state, shader demote, transform feedback, and vertex divisor.
These can reduce state churn or improve compatibility for particular games, but
they do not remove Wine, Box64, Venus transport, shadow-memory, or Host-driver
costs.

Modern DXVK 2.x additionally uses `vkQueueSubmit2` and dynamic rendering. This
may reduce CPU/front-end overhead and render-pass management work. It is not
expected to transform a Host-GPU-bound workload such as tessellation-heavy
Heaven. `VK_EXT_graphics_pipeline_library` is not currently exposed by the 920
Venus adapter, so its major shader-stutter benefit must not be assumed.

The BrokerPresent GPU copy is not the reason different GPUs would have the same
performance. Existing Heaven profiling places that copy at roughly 0.06-0.19
ms. Low-quality Heaven scenes retain meaningful shared front-end cost, while
tessellation-heavy scenes are dominated by Host GPU completion. A stronger Host
GPU should therefore help heavy scenes, but gains will not be linear in light
scenes and can be affected by resolution and sustained thermal limits.

## 4. Why stock DXVK 2.x cannot be dropped in

DXVK 2.0 through 2.6.x require a Vulkan 1.3 driver and
`VK_EXT_robustness2`. The 920 Guest satisfies those core requirements; the 910
Guest satisfies neither and must remain on Legacy or WineD3D.

However, upstream DXVK 2.6.2 D3D11 baseline requests these desktop features
without the WineHua fallbacks used by the Legacy fork:

```text
dualSrcBlend
multiViewport
textureCompressionBC
transformFeedback + geometryStreams
```

The current 920 Venus adapter reports:

```text
dualSrcBlend          = 0
multiViewport         = 0
textureCompressionBC  = 0
transformFeedback     = 1
geometryStreams       = 0
```

An unmodified upstream 2.0 or 2.6.x DLL is therefore expected to reject the
D3D11 feature baseline or expose an unsafe partial device. Vulkan 1.3 does not
change that conclusion.

DXVK 2.7.1 and later are not the initial target. The published 2.7 profile
requires descriptor indexing, runtime descriptor arrays, update-after-bind,
maintenance5, and buffer device address. Current 920 Venus exposes neither
descriptor indexing nor buffer device address. Re-evaluate only after a new
Guest capability capture proves all required features, limits, and extensions.

## 5. DXVK 2.6.2 qualification checkpoint, 2026-08-01

The `feature/dxvk-modern-2.6` fork now contains the WineHua compatibility
implementation required to run DXVK 2.6.2 over the current Venus transport.
It is not an unmodified upstream DLL drop. The implementation covers the
capability policy, BC decode/backing views, qualified fallback paths, mapped
memory/shadow synchronization, descriptor handling, and shader compatibility
controls needed by the established Legacy path.

Maleoon 920 qualification passed the following gates:

```text
DXVK 2.6 requirements: Guest Linux, Wine x86, Wine x64
Modern D3D11 baseline: x86 and x64
D3D switch cube: x86 and x64
Prefix gates: clean once, reuse three times
Stability gate: fixed 60-second Modern run
```

Maleoon 910 remains explicitly `UNSUPPORTED` for Modern: the Guest Venus API
is Vulkan 1.2.275, so it cannot meet the Vulkan 1.3 + robustness2 contract.
It continues to use Legacy 1.10.3, whose x86/x64/Cube regression suite passed.
The GPU diagnostics smoke records the actual guest device API, feature set,
requested profile, loaded DXVK DLL paths, and D3D11 creation result in both a
GUI report and `C:\\smoke\\results\\diagnostics`.

Modern is a game-validated, capability-gated profile on 920. It must never be
selected by pretending a Vulkan 1.2 device has Vulkan 1.3 features.

The physical-device evidence includes Heaven D3D11 sessions under
`D:\\MyProject\\winehua-logs\\automation\\heaven-modern-*`. The clean baseline
log records `WINEHUA_DXVK_PROFILE=modern-2.6`, `WINEHUA_DXVK_VERSION=2.6.2`,
and the managed `modern-2.6` DLL directories; the DXVK log reports
`DXVK: 2.6.2`. Captured frames and FPS samples prove that this was a rendered
game workload rather than device creation alone.

## 6. Required WineHua compatibility inventory

A Modern branch is a forward-port, not a DLL swap and not a blind cherry-pick
series. Before it can create a D3D11 device on current Maleoon/Venus, inventory
and port the production portions of the Legacy compatibility implementation:

1. Relaxed D3D11 capability policy that never claims unsupported native Vulkan
   features, paired with explicit emulation or `UNSUPPORTED`.
2. BC1-BC7 upload decode and uncompressed backing-image/view handling.
3. Qualified dual-source blend fallback.
4. Qualified RGBA8 SNORM render-target fallback.
5. Custom-border, sampler, format-view, depth comparison, and Cube/CubeArray
   Maleoon quirks documented in `thirdparty/dxvk/WINEHUA_FORK.md`.
6. Mapped-memory flush ownership and Venus shadow synchronization contract.
7. Bool sampled-descriptor specialization policy and its capability/adapter
   quirk controls.

Diagnostics may be redesigned for the Modern code base. Correctness fixes,
semantic emulation, runtime manifest integration, and smoke coverage must not
be omitted merely because their original commits do not apply cleanly.

## 7. Remaining Modern coverage work

1. Continue fixed-setting Legacy versus Modern A/B on additional games and
   workloads. Record DLL identity, frame percentiles, Host CPU time, completion
   wait, shadow bytes, present cost, output image, and crash status.
2. Complete a separate 60-minute stability gate and overwrite-install refresh.
3. Re-run qualification whenever the capability hash, Host driver, Venus, or
   Mesa changes.

This remaining breadth and long-duration work does not erase the existing 920
game result or justify describing 2.6.2 as less compatible than 1.10.x. A new
capability hash, Host driver update, Venus update, or Mesa update invalidates
the qualification and requires rerunning the matrix.

## 8. Known Modern workload limitation

The 920 Modern profile has a known limitation in
`Z:\\games\\dx11_test\\dx11_test\\InstancingFX11\\InstancingFX11.exe`:

- the qualified Modern base plus the isolated VKD3D swapchain bridge
  (`977a3d78`) keeps the ordinary 2.6.2 path free of the later Instancing
  experiments; the last visible workload result had vegetation but incomplete
  alpha foliage/leaf geometry;
- later experimental Modern changes were able to turn this workload into a
  black screen and, in one run, a first-present `VK_ERROR_DEVICE_LOST` followed
  by a Venus ring fatal error;
- the ordinary DXVK 2.6.2 smoke suite and other validated D3D11 workloads still
  pass on the same 920 device.

This is recorded as a workload-specific Modern/Venus/Maleoon compatibility
issue, not as a reason to downgrade the normal 920 DXVK path or to claim that
DXVK 2.6.2 is globally unsupported. The failed follow-up experiments remain
in local Git stashes and are not part of the qualified runtime.

## 9. VKD3D boundary

VKD3D is a separate D3D12 project, not a consequence of upgrading DXVK. DXVK
2.6.2 qualification proves a D3D9/10/11 translation path only. The current
VKD3D-Proton 2.6 limited-500K profile has its own capability matrix, descriptor
ceiling, smoke, demo, and sample evidence. Failure to qualify a newer VKD3D
profile does not downgrade DXVK 2.6.2.

VKD3D-Proton 2.6 exposes the legacy `IWineDXGISwapChainFactory` interface. The
Modern 2.6.2 DXGI overlay must retain the narrowly scoped `977a3d78` bridge
which queries that IID after the normal `IDXGIVkSwapChainFactory` query fails.
This is an ABI compatibility bridge, not a rendering workaround. On Maleoon
920, the same x64 1000-frame D3D12 smoke produced the following A/B evidence:

- Modern DXGI without the bridge: D3D12 device and feature queries succeeded,
  but `CreateSwapChainForHwnd` returned `DXGI_ERROR_UNSUPPORTED` and rendered
  0/1000 frames;
- Legacy 1.10.3 DXGI with the same VKD3D DLL: 1000/1000 frames passed at
  37.956 FPS, including animation, buffer-copy/readback, and fence checks;
- the qualified product combination is VKD3D-Proton 2.6 plus Modern DXGI with
  the legacy-factory bridge; it must pass the same authoritative 1000-frame
  test after every DXGI update.

## 10. Decision record

```text
920 D3D11:             WineHua DXVK 2.6.2, adapted and game-validated
910 / Vulkan 1.2:      DXVK 1.10.3 fallback
Modern DXVK 2.6.2:     capability-gated; not a transport/smoke-only candidate
910 and Vulkan 1.2:    Legacy 1.10.3 only; Modern reports UNSUPPORTED
Immediate objective:   broaden game coverage and long-run stability
DXVK 2.7+/3.x:         deferred pending Venus descriptor/BDA capability work
VKD3D:                  independent D3D12 profile and qualification
```
