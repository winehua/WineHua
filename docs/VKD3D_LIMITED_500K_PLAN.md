# VKD3D Limited 500K Adaptation Plan

> Updated: 2026-08-05

> Status: qualified limited-support product profile. VKD3D-Proton 2.6 is
> packaged and selected for default D3D12 launches after the recorded 910 gates.

## Objective

Document the qualified vkd3d-proton 2.6 limited-500K profile for devices whose Vulkan
driver supports a 500,000-entry bindless resource view array, without claiming
upstream vkd3d-proton compatibility or coupling the independently qualified DXVK
profiles to the selected VKD3D version.

The upstream-compatible profile remains unchanged: resource-view descriptor
arrays need 1,000,000 entries. The limited profile never fabricates a Vulkan feature or limit. It is a real
x64 product profile with an explicit 500,000 descriptor-heap ceiling.

## Current 920 Evidence

The four-layer audit from `regression-20260803-225528` observes these values
on Maleoon 920. The capability hash is
`bbfa91d471ea2f2d532da80c5601a4fe84bb3e81eb112838a0461176372e0e0d`.

| Layer | Aggregate descriptorIndexing | Required bindless fields | Requested 2.6 bindless device |
| --- | --- | --- | --- |
| Host Vulkan | yes | all observed | not attempted |
| Guest Venus | no | all observed | `VK_SUCCESS` |
| Wine Vulkan x64 | yes | all observed | not attempted |
| Wine PE x64 | no | all observed | `VK_SUCCESS` |

`maxPerStageUpdateAfterBindResources` is 2,000,016 and
`maxUpdateAfterBindDescriptorsInAllPools` is `UINT32_MAX`. The numeric limit
is therefore potentially useful. Every resource and sampler UAB per-stage and
per-set limit is 500,000. The aggregate `descriptorIndexing` member is false
for Guest Venus and Wine PE, but vkd3d-proton 2.6 gates the individual fields,
not that aggregate member; the requested feature chain has already produced a
real `VK_SUCCESS` device at those layers. It is not an experimental blocker.

The 2,000,016 aggregate per-stage resource limit remains a real risk. Without
a mutable-descriptor path, a root signature can require several independent
500K resource layouts and exceed that aggregate. A real root-signature and
descriptor microtest must therefore validate the exact layouts used before any
runtime integration claim.

## Profile Definitions

| Profile | Resource view limits | Sampler limits | Input attachments | Gate status |
| --- | --- | --- | --- | --- |
| Upstream 2.6 / 2.8 / 2.9 | >= 1,000,000 per-stage and per-set | >= 2,048 | Informational | May qualify for Gate B only after all normal gates pass. |
| Product 2.6 limited 500K | >= 500,000 per-stage and per-set | >= 2,048 | Informational | Qualified limited support on the recorded 910 capability hash; requests above 500,000 remain unsupported. |

The resource limits are Sampled Images, Storage Images, and Storage Buffers.
The vkd3d 2.6 bindless path creates a 1,000,000-entry resource layout and a
2,048-entry sampler layout. `InputAttachments=8` is not a D3D12 shader-visible
resource heap limit. A title that requests a 1,000,000-entry resource heap
cannot be silently truncated by the experimental profile.

## Implementation Sequence

1. Complete the four-layer audit and require a stable capability hash. This is
   complete for `bbfa91d471ea2f2d532da80c5601a4fe84bb3e81eb112838a0461176372e0e0d`.
2. Create `feature/vkd3d-capability-probe` in an isolated vkd3d-proton checkout
   from v2.6 commit `3e5aab6fb3e18f81a71b339be4cb5cdf55140980`. Do not add a
   product submodule or alter a main-repository gitlink.
3. Add a separately named 500K build profile and expose it as the qualified D3D12 default. Reduce every
   descriptor-layout, variable-count allocation, descriptor-heap maximum,
   host mapping, and reported D3D12 view-heap limit coherently. Keep sampler
   heaps at 2,048.
4. Reject a `D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV` shader-visible heap over
   500,000 with a defined D3D12 error. Do not clamp its size or rewrite its GPU
   descriptor addresses.
5. Build only the isolated x64 vkd3d DLLs. Record upstream tag, fork commit,
   DLL version, SHA-256, loader path, and default-disabled activation setting.
6. Pass a real x64 D3D12 microtest: device, queue, fence, upload/default/
   readback copy, byte-for-byte verification, descriptor writes, and a 500K
   shader-visible resource heap. Run it three times without `vkDeviceWaitIdle`,
   sleeps, or global flushes.
7. Add descriptor-heap telemetry for each game: requested capacity and highest
   descriptor index written. A game that creates a 1M heap remains unsupported
   even if its observed writes are sparse, unless a future vkd3d design can
   preserve its GPU descriptor-handle semantics.

## Safety Boundaries

- Do not modify Host, Venus, Wine, or PE probes to report larger limits or
  enabled features than they expose.
- Do not change `master`, the validated DXVK 1.10.3 fallback, or the adapted
  DXVK 2.6.2 profile. VKD3D admission must not downgrade D3D11 selection.
- Keep the default D3D12 route limited to the qualified x64 VKD3D profile; a built
  DLL alone is never sufficient evidence.
- Capability hash, Host driver, Mesa/Venus, or Wine Vulkan changes invalidate
  the audit and require Gate A again.

## Current Next Gate

The real 500K final-slot GPU descriptor microtest is complete on Maleoon 910:
three independently reinstalled runs passed with identical build identity,
post-teardown result records, and process exit status. See
`VKD3D_DESCRIPTOR_500K_910_20260804.md`.

The isolated physical-display gate is now also complete on Maleoon 910: after
correcting the VKD3D experiment's Box64/Venus command-ring ordering, three
independently reinstalled runs sustained 1,000/1,000 Guest frames and reached
Host frame 960 with zero SurfaceQueue failures. See
`VKD3D_GRAPHICS_SMOKE_910_20260805.md`.

The BDA gate is also complete on Maleoon 910 with the ordering-fixed HAP. Three
independently reinstalled runs validated non-zero GPU virtual addresses, root
SRV/UAV GPUVA compute access, BDA output readback, and byte-for-byte results
while retaining the 500K final-slot descriptor coverage. See
`VKD3D_BDA_910_20260805.md`.

The multiple-queue gate is complete on Maleoon 910. Three independently
reinstalled runs validated COPY-to-DIRECT and DIRECT-to-COPY fence dependencies,
two COPY submissions, one DIRECT submission, and a 256-byte
upload/default/readback path. See `VKD3D_MULTIQUEUE_910_20260805.md`.

The 1,000-frame physical-display evidence already exercises the isolated
`venus_broker_present` route. The remaining qualification work is to reconcile
that evidence as the BrokerPresent gate and run the full DXVK product
regression.

The Maleoon 910 regression used DXVK 1.10.3 because its Vulkan 1.2 Guest does
not meet DXVK 2.6.2 admission. That path passed a clean-prefix x86/x64 full
feature matrix, fixed-frame validation, coverage audit, and cube presentation
with no fallback or per-frame device wait-idle. See
`VKD3D_DXVK_REGRESSION_910_20260805.md`. The Modern matrix failure recorded in
that document is a 910 capability result; it must not be generalized to
Maleoon 920. The WineHua DXVK 2.6.2 profile is adapted and has separate real
D3D11 game validation on 920.

The limited-500K track has therefore completed its descriptor, Gate C, BDA,
multiple-queue, physical BrokerPresent, and validated product-path regression
checks on the recorded 910 artifacts. It may proceed to a carefully selected
real DX12 title investigation with descriptor-heap telemetry. This is formal limited support rather than unrestricted upstream support: default
D3D12 launches use VKD3D 2.6 on x64, while D3D11/DXGI use the independently
qualified DXVK profile (2.6.2 on 920, with 1.10.3 retained for 910 and other
Vulkan 1.2 devices). A heap request above 500,000 is rejected, Query Meta
remains unsupported, and any capability/driver/Mesa/Venus/Wine change
invalidates the qualification.

The product profile keeps the qualified precise mapping, direct-fence,
persistent-map synchronization, and uncoalesced ring notification behavior,
but disables their high-volume diagnostic traces. Gate C and the manual DX12
smoke opt into `shadow-precise-direct-fence` when full evidence capture is
needed.
