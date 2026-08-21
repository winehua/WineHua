# VKD3D Isolation DXVK Regression Evidence — 910 — 2026-08-05

## Scope

This report checks that the isolated VKD3D work did not regress the validated
product D3D11 path. It does not load the experiment-local VKD3D DLL and does
not enable D3D12 for ordinary launches.

- Branch: `feature/vkd3d-capability-probe`
- Device: `62T0225B10005882` (910)
- Runtime ordering fix in the HAP: `5dde3da`
- Automation fixes used for artifact capture: `bb6d340`, `84c70ed`
- Signed HAP timestamp: `2026-08-05 02:01:05.957185477 +0800`
- Signed HAP SHA-256: `1b4206216813583eaacd1692b5764ffd93c57c97276777df18718499fd41e974`
- Embedded `wine-data.zip` SHA-256: `ea2f9f9e410416dd2688469f032481b1d01cb3c93350a237cd83c06379db6be5`

The HAP embeds the assembled rawfile, the Guest EGL/Vulkan runtime is x86-64,
and Host `libentry.so` is AArch64. Commits after `5dde3da` and before this run
changed only documentation and the host-side automation runner; no HAP runtime
source changed after the recorded build.

## Clean Legacy product regression

The qualified session is:

`regression-20260805-031507`

It used the product performance profile
`shadow-precise-dirty-ring-inline-upload-coverage-sort` and the sequence:

`force-stop -> uninstall -> setmode 602 -> install -> clean prefix -> dxvk suite`

The session, Guest suite, application result, fixed-frame validation, and D3D11
coverage result are all `PASS`.

| Test | PE | DXVK | Frames | Present | Fallback | Per-frame device wait-idle | Result |
| --- | --- | --- | ---: | ---: | --- | ---: | --- |
| `dxvk-legacy-x86` | x86/WoW64 | 1.10.3 | 60 | 0 | false | 0 | PASS |
| `dxvk-legacy-x64` | x86-64 | 1.10.3 | 60 | 0 | false | 0 | PASS |
| `dxvk-cube-x64` | x86-64 | 1.10.3 | fixed frame | success | false | 0 | PASS |

Both feature-matrix runs report feature level 11.0, `cpuReadBytes=0`,
`cpuUploadBytes=0`, 60 queue submissions, 79 feature-probe GPU copies, and
1,011,796 bytes of explicit feature-probe readback. Required coverage has no
missing or submitted-only entries. It includes RGBA8 PS/CS load, point and
linear sampling, descriptor identity/rebind/unbound/lifetime, subresource
array/mip/LOD/update, 3D textures, BC emulation, compute UAV and sampled image,
MSAA resolve, D24S8/Cube/CubeArray/border cases, queries, and presentation.

The x86 and x64 fixed-frame captures independently passed
`d3d11-cube-color-depth-v1` on their first attempt. Each 1280x2832 capture has
9,906 colored samples across red, green, and blue buckets. Their identical
SHA-256 is `b0a69053c74cfef159963f136705c1b936e35ac803bb3528512b669449709cb7`.

## Qualified archive hashes

| Artifact | SHA-256 |
| --- | --- |
| `artifact.json` | `dc4ad87f1dd929bc0c67b0d7cff392c76737014eef23b2b0b7d895cafd88d3a2` |
| `automation-summary.json` | `f47a5c14e41f5009f0cdaee9dacb0cc43635ba6d7dd529080d5fdfadca768c11` |
| `host-summary.json` | `b0e5f89fdf68a87c237475fa64d8ea291002b9c2119c2b7cd7188ee4662683f7` |
| `suite-summary.json` | `87acb8a0a6e62ea3cb223bf1e37bf1ab955891317c89934ce5dde76066f1f474` |
| `wine-stderr.log` | `43d25d3a04a54bfe0f20a33c2ab0b3188ea9b38f92e44ded76731b1fab6f32e2` |
| `virgl-host.log` | `fb338774162dbabba0117f312cb66d8167a5e196bb6b57fb0557b7694f0f52d9` |
| `hilog.txt` | `4324ff935a6b7b5762adf198af98395ee16befa696c45168cf6e095ef17089b4` |

The complete archive is retained at
`/home/maple/Work/WineHua-build/.temp/automation-logs/regression-20260805-031507`.

## Separate Modern 2.6.2 observation

An additional clean uninstall/reinstall session tested the separately selected,
non-default Modern profile:

`regression-20260805-031817`

This session is `FAIL`, not a qualified regression pass. The Modern x64 cube
presented successfully, but both x86 and x64 full feature matrices returned
zero readback data, zero queue submissions, zero present frames, and
`presentResult=E_FAIL`. No WineD3D fallback was detected.

| Modern artifact | SHA-256 |
| --- | --- |
| `automation-summary.json` | `6dceaee7820a29e0e9f7af0d4e74e04eac0dc50d79edcd83d44c17a18eb9f9cf` |
| `host-summary.json` | `63a5b7d8a29958b90350c19df5173659246a4351485f0d8c1efc6847ee70dd57` |
| `suite-summary.json` | `1a7b094b44de88effd221790a2f8e25995c1513df5b65071e53f711325990e1d` |

The Modern result is recorded explicitly and remains a separate investigation.
It does not invalidate the Legacy product regression pass, but it must not be
reported as supported or silently fall back. No Modern or default runtime code
was changed as part of this VKD3D gate.

## Decision

The validated product DXVK Legacy path passes its complete x86/x64 functional,
visual, and coverage regression on the exact HAP above. Together with the
three 1,000-frame `venus_broker_present` runs, this completes the product-path
regression and isolated BrokerPresent evidence required before a limited-500K
real DX12 title investigation.

This does not qualify general VKD3D support. D3D12 remains default-off, Modern
2.6.2 remains unqualified on this run, and any title creating a resource heap
above 500,000 remains unsupported.
