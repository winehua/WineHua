# WineHua Phase 2 DXVK/Venus Merge Report

> 状态：已完成（2026-09-22 整理归档）。Phase 2 的合并决策报告，合并已完成。

## 1. Purpose and release decision

This document is the merge-facing summary for the WineHua Phase 2 Vulkan and
DXVK work. It records the product architecture, source ownership, automated
coverage, physical-device evidence, retained development tooling, known
limitations, merge order, and rollback boundary.

The release decision is deliberately narrow:

- Keep WineD3D/VirGL as the compatibility fallback.
- Ship the controlled DXVK Legacy 1.10.3 fork as the current DX11 path.
- Use the verified Venus BrokerPresent/private-vtest path.
- Keep SmokeRunner and exact replay diagnostics because Phase 2 remains under
  active compatibility development.
- Do not claim Modern DXVK, DirectPresent, D3D12, complete stream-output, or
  DX9-equivalent performance.

The runtime candidate accepted by continuous device inspection is:

```text
HAP SHA-256:
07013f8c8e0c02e824bd35a0db55b962e95ce5ad29bf660ec2db2c01b2e066c1

DXVK runtime source:
3618d7ead648a8d9352dde41888a77b3302529cf

Main source before merge-only documentation/submodule cleanup:
403cb77
```

The final merge-preparation build may have a different HAP hash because the
fork documentation, submodule URL, and Smoke build entry are committed after
the accepted runtime build. No rendering behavior may change during that
rebuild. Its hash and gate results must be appended before master merge.

## 2. Source repositories and ownership

All modified submodule commits are available from WineHua-owned remotes.

| Repository | Remote | Merge-preparation head | Role |
| --- | --- | --- | --- |
| WineHua | `winehua/WineHua` | `feature/render-element-completeness` | App, broker, packaging, automation |
| DXVK | `winehua/dxvk` | `774f39d` | Controlled DXVK Legacy fork and fork policy |
| Mesa | `winehua/mesa-ohos` | `ee411de9` | Guest Venus transport, ring and private present |
| virglrenderer | `winehua/virglrenderer` | `3997c9d2` | Host Venus, shadow memory, synchronization and present |
| Wine | `winehua/wine` | `d9d3773c592` | winevulkan, Win32 WSI and x86/x64 smoke programs |

DXVK fork branch policy:

```text
upstream:
  https://github.com/doitsujin/dxvk.git

WineHua fork:
  https://github.com/winehua/dxvk.git

product branch:
  dxvk-legacy-1.10.3

development branch:
  feature/render-element-completeness
```

Both DXVK branches currently point to the same reviewed source head. The main
repository gitlink pins an exact commit; it never follows upstream implicitly.

## 3. Final architecture

WineD3D fallback remains independent:

```text
Windows D3D
  -> WineD3D
  -> Guest OpenGL / Mesa virpipe
  -> VirGL/vtest
  -> Host EGL/GLES
  -> VirGL SurfaceQueue compositor
```

The Phase 2 DX11 path is:

```text
Windows D3D11
  -> WineHua DXVK Legacy 1.10.3
  -> Wine Vulkan / Win32 WSI
  -> x86_64 Vulkan loader
  -> Guest Mesa Venus
  -> vtest transport
  -> ARM64 virglrenderer Venus
  -> Harmony Vulkan
  -> Broker SurfaceQueue
  -> NativeImage consumer
  -> App XComponent compositor
```

The frame-order invariant that fixed real backward-frame publication is:

```text
DXVK final presenter-copy QueueSubmit
  -> Venus primary ring
  -> vn_ring_roundtrip(primary_ring)
  -> vn_ring_wait_all(primary_ring)
  -> private vtest present
```

This ordering is part of correctness, not optional pacing. It must not be
removed, moved to an unrelated ring, or replaced by an asynchronous publish
without a new generation-safe synchronization proof.

Guest Venus handles never cross into the App. Host presentation identifies the
real Vulkan resource by renderer-owned context/resource identity. Normal DXVK
presentation has no full-frame GPU-to-CPU readback and no CPU full-frame
upload. One Host GPU copy/composition remains in the BrokerPresent path.

## 4. Product selection and launch contract

The App exposes two product modes:

| D3D backend | Present backend | Intended use |
| --- | --- | --- |
| `wined3d` | `virgl_compositor` | Compatibility fallback and D3D9/OpenGL |
| `dxvk_legacy` | `venus_broker_present` | Current D3D11 product path |

The UI and managed `runWineProgram` API default to `dxvk_legacy`. A game Want
must explicitly carry `winehua.d3d_backend=dxvk_legacy`; omitting it at the
EntryAbility boundary intentionally resolves to WineD3D for backward
compatibility. Explorer descendants inherit the selected desktop environment,
so manually started DX11 executables load the managed DXVK overlay instead of
requiring per-game DLL copies.

Managed DLL locations are selected by PE architecture:

```text
files/wine/dxvk/legacy/x64/dxgi.dll
files/wine/dxvk/legacy/x64/d3d11.dll
files/wine/dxvk/legacy/x86/dxgi.dll
files/wine/dxvk/legacy/x86/d3d11.dll
```

The automation records the actual loaded module paths and treats WineD3D
fallback as failure for a DXVK test.

The qualified product profile is:

```text
WINEHUA_PERF_PROFILE=shadow-precise-dirty-ring-inline-upload-coverage-sort
DXVK_WINEHUA_PRECISE_SHADOW=1
DXVK_WINEHUA_FLUSH_DYNAMIC_MAPPED=1
DXVK_WINEHUA_BATCH_MAPPED_FLUSH=1
VN_WINEHUA_REMOTE_MEMORY_SYNC=1
VN_WINEHUA_STRONG_RING_BARRIER=1
BOX64_DYNAREC_WEAKBARRIER=0
```

Mapped writes are owned by the consuming DXVK command list, merged per
`VkDeviceMemory`, and flushed immediately before that list's QueueSubmit.
Unassociated writes retain synchronous flush. The rejected device-global
pending-flush design crossed resource generations and must not be restored.

## 5. Compatibility implementation

The Legacy fork contains scoped compatibility policy rather than pretending
that every Host feature exists.

### 5.1 Adapter and shader quirks

- Bool sampled-descriptor specialization is selected automatically for the
  verified Venus/Maleoon adapter family.
- Maleoon cube depth comparison uses the required padded coordinate form.
- Native CubeArray comparison instructions that hang the Host ring are lowered
  to a semantically equivalent 2D-array comparison path for the affected
  adapter.
- Unsupported dual-source blend used by the qualified Heaven path is emulated
  with a constrained two-pass implementation. Cases outside its proven state
  predicate remain unsupported rather than silently changing semantics.

Debug environment overrides remain available for A/B qualification, but normal
product selection is capability/adapter based.

### 5.2 BC texture compatibility

When native BC support is absent, BC1-BC7 uploads are decoded into equivalent
uncompressed backing images. This permits the tested D3D11 resource and sample
paths to work on the current Maleoon driver. It is not free:

- backing memory is larger than native BC;
- upload has CPU decode cost;
- future update/copy/view combinations still require regression coverage.

Native BC remains selected automatically on GPUs that expose it. ASTC/ETC2
cannot be substituted directly because their bit encoding is not DXGI BC.

### 5.3 Explicit remaining capability gaps

- Transform feedback/stream-output remains unavailable when the Host lacks the
  Vulkan feature.
- Modern DXVK capability requirements have not been qualified.
- D3D12/VKD3D is outside Phase 2.
- DirectPresent and NativeBuffer import are deferred; BrokerPresent is the
  accepted path.
- Capability-query faults are reported as UNKNOWN/UNSUPPORTED, never forged as
  PASS.

## 6. Smoke and automation retained for development

Smoke is a merge deliverable. It is intentionally retained in the development
HAP so future driver, Mesa, Wine, and DXVK changes can be tested without manual
Explorer interaction.

Managed Windows layout:

```text
C:\smoke\
  manifest.json
  x64\
    winehua_audio_smoke.exe
    winehua_graphics_smoke.exe
    winehua_vulkan_smoke.exe
    winehua_d3d11_smoke.exe
    winehua_d3d_switch_cube.exe
  x86\
    winehua_audio_smoke.exe
    winehua_graphics_smoke.exe
    winehua_vulkan_smoke.exe
    winehua_d3d11_smoke.exe
  assets\
  results\<run-id>\
```

Guest Linux and Host Vulkan probes remain under the managed graphics runtime,
not disguised as Windows executables in `C:\smoke`.

The PowerShell entry point is:

```powershell
automation\Invoke-WineHuaAutomation.ps1
```

It builds only in the canonical Docker container, validates artifact hashes and
architectures, installs with Windows HDC, launches through Want parameters,
collects JSON/log/image evidence, and returns a machine-readable exit code.

Current DXVK smoke coverage includes:

- x86 and x64 D3D11 device creation and actual DXVK DLL identity;
- sampled Load, point and linear sampling in pixel and compute shaders;
- descriptor rebinding, unbound slots and resource lifetime;
- mip levels, array layers, explicit LOD and barrier update;
- 3D texture upload, UAV-to-SRV transition and ping-pong compute;
- BC format/mip decoding and sampling;
- MSAA resolve;
- compute/UAV;
- D24S8 array, views, cube/cube-array and comparison sampling;
- MRT G-buffer, HDR RGBA16F, read-only depth and mini deferred pipeline;
- visible color/depth Cube with monotonic angle validation.

Exact SPIR-V replay, Guest Venus material replay, Host Vulkan replay and frame
identity analyzers remain development tools. Broad draw/resource capture is
disabled unless an explicit diagnostic environment variable is set.

## 7. Physical-device evidence

Final accepted runtime automation:

```text
archive:
  <本机证据目录>\winehua-logs\automation\phase2-20260727-220057

status:
  PASS

suite/prefix:
  dxvk / reuse

HAP SHA-256:
  07013f8c8e0c02e824bd35a0db55b962e95ce5ad29bf660ec2db2c01b2e066c1
```

Both x86 and x64 comprehensive tests reported:

```text
DXVK version=1.10.3
featureLevel=11.0
Vulkan=winevulkan/Venus
fallbackDetected=false
cpuReadBytes=0
cpuUploadBytes=0
perFrameDeviceWaitIdle=0
MSAA resolve=PASS
compute/UAV=PASS
3D texture=PASS
D24S8 matrix=PASS
MRT=PASS
Heaven mini pipeline=PASS
```

The visible x64 Cube completed 467 frames with `angleRegressions=0`.

Continuous Heaven inspection used the same HAP. Six stable display samples
were:

```text
23.545, 27.200, 18.540, 31.654, 30.933, 20.521 FPS
```

Range: 18.540-31.654 FPS. Median: 25.373 FPS. Archived day/night frames retained
geometry, material texture, lighting and colour. No fallback, device lost, ring
fatal, timestamp regression, or frame-angle regression was found. The user
reported that this exact version had no visual problem.

This performance is usable but is not yet demonstrated to match the equivalent
DX9 workload. Further performance work was explicitly paused for merge
preparation.

## 8. Code-review retention decisions

### 8.1 Required product code

Retain:

- managed DXVK runtime packaging and architecture-specific overlay;
- structured process launch and inherited desktop DXVK environment;
- Wine Vulkan and Win32 WSI bridge;
- Guest Venus vtest transport and primary-ring ordering;
- Host Venus shadow memory, dirty allocation tracking and GPU upload;
- private vtest present and Broker SurfaceQueue target;
- command-list-owned mapped flush batching;
- adapter-scoped shader/format compatibility;
- WineD3D/VirGL fallback.

These paths are used by the accepted runtime and covered by device evidence.

### 8.2 Required development code

Retain during active Phase 2 development:

- `SmokeRunner` and all machine-readable smoke protocols;
- x86/x64 D3D11 and Vulkan smoke executables;
- Guest/Host exact replay tools and shader assets referenced by the build;
- PowerShell build/deploy/test orchestration;
- bounded, environment-gated descriptor, command, UBO, image and frame-order
  diagnostics;
- the persistent renderer performance file used for profiling.

The product coverage-sort profile does not forward every performance summary
into hilog. Diagnostic profiles retain that forwarding when live inspection is
required.

### 8.3 Excluded local investigation artifacts

Do not commit or package:

- `automation/__pycache__/`;
- `*.orig` editor backups;
- `.dxvk_cube_fail.spv*`;
- `tmp_*.spv` and ad-hoc shader copies not referenced by a reproducible suite;
- local HAP copies and device logs.

### 8.4 Rejected runtime experiments

The following are documented but absent from the product path:

- device-global deferred mapped flush;
- full Host performance-summary removal;
- asynchronous present publication;
- release-fence removal;
- unknown-generation dirty-gap merging;
- unsafe cross-NCP NativeWindow/dma-buf pointer use.

These experiments caused colour corruption, frame rollback risk, deadlock, or
unproven lifetime behavior. Their measured FPS must not be quoted as accepted
performance.

## 9. Merge gates and known evidence gaps

Required before master merge:

1. Commit the WineHua-owned DXVK URL and all final submodule gitlinks.
2. Commit the Host Vulkan build script already referenced by `Makefile`.
3. Run `git diff --check` in the main repository and every changed submodule.
4. Run `scripts/check-submodules.sh` and prove every gitlink is reachable from
   its WineHua remote.
5. Build a fresh ARM64 HAP in `winehua-master-ext4`.
6. Verify HAP timestamp/SHA-256, embedded `wine-data.zip`, Guest x86-64, and
   Host AArch64.
7. Install with Windows HDC and require `install bundle successfully`.
8. Run final `dxvk/reuse` three times and `dxvk/clean` once.
9. Reconfirm continuous Heaven correctness on the final rebuilt HAP.

The 60-minute long-run gate remains paused by explicit user direction and must
be listed as waived/deferred in the master merge record. Earlier clean-prefix
passes prove the mechanism but do not replace a final-candidate clean run.

## 10. Merge order

Recommended order:

1. Review/merge the DXVK feature history into the controlled
   `dxvk-legacy-1.10.3` branch. It is currently a fast-forward.
2. Review/merge Mesa `feature/render-element-completeness` into `main`.
3. Review/merge virglrenderer `feature/render-element-completeness` into
   `master`.
4. Review/merge Wine `feature/render-element-completeness` into `master`.
5. Update the main WineHua gitlinks to the resulting default-branch commits if
   maintainers create merge commits instead of fast-forwards.
6. Merge WineHua `feature/render-element-completeness` into `master` only after
   the final HAP and device gates pass.

The current WineHua branch is based directly on `master` and is fast-forward
compatible as long as master does not move first.

## 11. Rollback boundary

Immutable runtime rollback artifact:

```text
<本机证据目录>\winehua-logs\performance\host-perf-forward-off-20260727-2149\
  entry-default-signed-07013f8c.hap
```

Earlier fully conservative product rollback artifact:

```text
<本机证据目录>\winehua-logs\manual\product-batch-default-20260727\
  entry-default-signed-43654ab7.hap
```

Rollback must replace the whole HAP/runtime combination. Mixing DXVK, Wine,
Mesa, virglrenderer, or Host NCP components from different candidates invalidates
the synchronization and capability evidence.

## 12. Post-merge work

The next performance investigation, when resumed, should first split Host
`buffer_record` cost into allocation iteration, dirty-range intersection,
range merging and Vulkan command recording. The accepted sample showed
`buffer_record` dominating Host prepare time, but actual GPU execution time is
not yet measured. Any optimization must remain an explicit A/B and preserve the
frame-order, resource-generation, mapped-flush and release-fence invariants
above.

Modern DXVK, DirectPresent, true NativeBuffer import/zero-copy, D3D12 and broad
game qualification remain later-stage work. They are not blockers for the
current controlled DXVK Legacy development baseline.

## 13. Final-candidate validation (2026-07-28)

The ARM64 candidate was rebuilt in `winehua-master-ext4` after aligning
`SpawnViaBroker()` with the broker's single `|__env=KEY=VALUE` protocol. The
previous `ENV:<size>` trailer was ignored by the merged broker, which left
smoke children with an empty DXVK environment and produced
`0x887a0004`; no Vulkan or DXVK implementation change was required for that
failure.

Artifact:

- signed HAP SHA-256: `c11a9249c1295ea69c3f1724d2f46dc5de947f09457ec25e7527e41a287a9c50`;
- embedded `wine-data.zip` SHA-256: `32df8e8a991e338da031e39656e1311ba6f0f98f72e04ba5ea700626322ab336` (matches source);
- Guest EGL: x86-64; host `libentry.so`: AArch64;
- Windows HDC install: `install bundle successfully` on `<设备序列号>`.

Run `manual-final-dxvk-20260728-0400` (`reuse`, `dxvk`) passed for both
`dxvk-legacy-x64` and `dxvk-legacy-x86`: feature level 11.0, DXVK 1.10.3,
Venus x86_64 loader/ICD, 60 presented frames, no fallback, no CPU full-frame
readback/upload, and zero per-frame `vkDeviceWaitIdle`. Descriptor identity,
rebind/lifetime, mip/array/explicit-LOD/barrier, BC1 emulation and sampled
compute/depth matrices passed. The 60-minute gate and final three-run/clean
matrix remain deferred per the current development-stage decision.

The checked-in one-command runner was also executed with `-SkipBuild -Suite
dxvk -Prefix reuse` on the same device and returned `Automation PASS`; the
archive is `<本机证据目录>\winehua-logs\automation\phase2-20260728-040847`.
