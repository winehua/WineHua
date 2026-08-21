# VKD3D Guest Swapchain Smoke Evidence — 910 — 2026-08-05

## Scope

This is isolated evidence for the `feature/vkd3d-capability-probe` branch. It
does not install VKD3D into the default Wine runtime, does not change the DXVK
Legacy/Modern selections, and does not enable D3D12 for ordinary launches.

Device: `62T0225B10005882` (910)

## Source and artifacts

- VKD3D-Proton runtime source: `f74c040a29688cd1e437bf089fd6548b89b00504`
- Graphics smoke source commit: `a302f9d4` (`test(vkd3d): add deterministic graphics present smoke`)
- `d3d12.dll` SHA-256: `01d74e215a3a7fb2172749f6cbd08bb746706cef3ba63c841b1444572ada8003`
- `vkd3d-graphics-smoketest.exe` SHA-256: `5632b68ae47e90b27a82400eb81d179d9ad1df23cabfdd14f8c0e0ed1d6d11ad`
- Signed HAP SHA-256: `688cf87035714bd0a9413ce829c98363b2357e9bbbc97a5c37a0b49eece68443`

The Docker build exited zero. Both payload files are x86-64 PE files. The
smoke executable import table contains only `GDI32.dll`, `KERNEL32.dll`,
`msvcrt.dll`, and `USER32.dll`; D3D12 and DXGI are loaded after `main()` with
hash-verified experiment-local payload resolution.

## Root-cause corrections

The first graphics smoke statically imported D3D12/DXGI, so VKD3D work could
begin before the first program checkpoint. The test now emits its configuration
and creates the Win32 window before dynamically resolving
`D3D12CreateDevice`, `D3D12SerializeRootSignature`, and
`CreateDXGIFactory1`.

The remaining apparent initialization hang was not a failed D3D12 device. The
stage trace proved successful creation of the device, direct queue, swapchain,
root signature, graphics PSO, command list, upload vertex buffer, RTV heap,
back buffers, fence, and event. The original demo loop rendered only when the
Win32 message queue became completely empty; continuous WineHua window traffic
starved that idle callback. The smoke now handles at most 64 messages per
iteration and then renders one frame deterministically.

No `vkDeviceWaitIdle`, sleep, global flush, or equivalent completion
compensation was introduced.

## Three clean-device results

Each run used a new experiment ID, full bundle uninstall/reinstall, a clean
Wine prefix, and the `vkd3d-500k-*` isolated runtime profile.

| Experiment | Result | Frames | Elapsed | FPS |
| --- | --- | ---: | ---: | ---: |
| `vkd3d-500k-f74c040a-graphics-r4` | PASS | 3/3 | 62687 ms | 0.048 |
| `vkd3d-500k-f74c040a-graphics-r5` | PASS | 3/3 | 62843 ms | 0.048 |
| `vkd3d-500k-f74c040a-graphics-r6` | PASS | 3/3 | 62825 ms | 0.048 |

All runs used a 640x480 `DXGI_FORMAT_B8G8R8A8_UNORM` two-buffer swapchain,
`Present(0)`, a direct D3D12 queue, per-frame fence completion, and returned
exit code zero with `status=PASS` JSON.

## Retained log hashes

| Log | SHA-256 |
| --- | --- |
| `vkd3d-graphics-r4-pass-stderr.log` | `96ab2cb6555cc0cfa66b4e65339bc76213ed719d7bc378eb323f2c8baac4f25a` |
| `vkd3d-graphics-r4-pass-virgl.log` | `fad357f25bcf2b3b8ae72cc39baac746d394914d3480235510ba8d9a9845a35a` |
| `vkd3d-graphics-r5-pass-stderr.log` | `cf37a28f5c2652be17213884e406d50f434ad3251921cd81db223a8d15812dd8` |
| `vkd3d-graphics-r5-pass-virgl.log` | `98999d5e098812dbb2df0d18333b6d36b33b618779c12c844cf9d4e0610b8b8c` |
| `vkd3d-graphics-r6-pass-stderr.log` | `44768007ee8e9e816f03c24dedae92e29c67eaa49ca60a38d25db52e425d6eab` |
| `vkd3d-graphics-r6-pass-virgl.log` | `260c0dc888e52222aa8b19035942ec00bd74a6590c282393e225d9034800758f` |

The retained logs are stored outside the repository at `D:\MyProject`.

## Gate status

The three-frame graphics construction, command submission, fence, and DXGI
swapchain API smoke is reproducible on the 910. This qualifies only the Guest
D3D12 execution path. It does not qualify physical presentation, games, or the
1000-frame physical-display gate.

A post-run Host log audit found that the Venus presenter did not attach to the
Wine window. It waited for key `182205397598275` (`surface=67`) and timed out
with `target missing` / `result=-11`, while the actual Wayland window later
created `surface=71` (`key=182205397598279`). Wine maps the Host `-EAGAIN` to
`VK_SUBOPTIMAL_KHR`, so the DXGI microtest can report `PASS` even when the Host
did not publish its frames. The earlier interpretation of `-EAGAIN` as buffer
release pacing was therefore incorrect.

The staged VKD3D experiment selected `winehua.d3d_backend=wined3d`. The UI
previously derived the Host presentation backend only from that D3D11 setting,
selecting `virgl_compositor`, although the experiment-local `d3d12.dll`
presented through Vulkan/Venus. The r7 correction routes only `vkd3d-*`
experiments to `venus_broker_present`; ordinary WineD3D, DXVK Legacy/Modern,
and default desktop launches remain unchanged.

## r7 presenter-routing result

The r7 run used a full uninstall/reinstall, a clean prefix, and the newly built
HAP (`68a94c74855c0c5c5a8f8e7657a9f14017f98a251a842fbd154f46144753422d`).
The launch log records `d3d=wined3d present=venus_broker_present`. The Host
initially observed the expected startup race, then attached the same requested
key after 42,364 microseconds and created a 640x480 FIFO swapchain successfully.

The SurfaceQueue consumer published frame 1 with one signal and zero failures.
The Guest completed 3/3 frames in 1,971 ms (`fps=1.522`) and exited with code
zero. This proves that the corrected route reaches a real Host target; it does
not yet prove three independently displayed frames or the continuous 1000-frame
gate.

Retained r7 logs outside the repository have SHA-256 hashes
`9b2871443667a2b984941903a188ee946f7932ac1012cf99c474c391b501bf0e`
(Host) and `3e91e901decba8c747a72be99d54268ac2e6fccf1838b0aeda139ecc55605be3`
(Guest stderr).

Physical presentation requires a matching Host target, successful presenter
attachment/swapchain setup, `vk_present ret=0`, and observable frame publication.
The 1000-frame gate remains blocked until those conditions pass without sleep,
`vkDeviceWaitIdle`, or global-flush compensation.

## r8 120-frame physical-display result

A clean uninstall/reinstall run kept the same target attached for 120 frames.
The SurfaceQueue evidence reached `frame=120`, `signals=120`, and `failures=0`.
The Guest result was 120/120 in 3,416 ms (`fps=35.129`) with exit code zero.
No sleep, `vkDeviceWaitIdle`, or global-flush compensation was added.

Retained r8 logs outside the repository have SHA-256 hashes
`a20445682d127b418ddd32ca4bc458d359c677ad9d52f9e67cf5f319432686a8`
(Host) and `0a70c655cdf6752e90aab9767d722c50390fe93a352c4648dee6723bb4bb69c1`
(Guest stderr).

## r9 ordering failure and root cause

The first 1,000-frame attempt failed after Guest frame 145. The Host decoder
reported `vkCmdBindDescriptorSets resulted in CS error`, entered its fatal
state, and the Guest aborted its Venus ring submission. The retained r9 Host
and Guest logs have SHA-256 hashes
`887c2c1291eb03e5e081c209acd1be41160740c765210b5b8e55708b35626456`
and `3ae4f3ffab48b64fb9959d0d279ff743cf260e67273c9606208adc4c4c6979d9`.

The failure was initially correlated with the 128 KiB Venus ring size, but the
shared ring uses one SHM fd on both sides and its normal wrap implementation is
unchanged. The decisive difference was in the process environment: the stable
DXVK/Venus path uses `BOX64_DYNAREC_WEAKBARRIER=0` and
`VN_WINEHUA_STRONG_RING_BARRIER=1`, while the VKD3D experiment recorded the
default weak-barrier value `2` and no strong publication barrier. The
experiment deliberately keeps the product D3D11 backend as WineD3D, so it had
bypassed the DXVK-only transport defaults even though its isolated D3D12 DLL
was issuing Vulkan commands through Venus.

Commit `5dde3da` applies those two ordering requirements in
`capabilityProbeEnvironment()`, which is used by the PE Vulkan probe and
isolated VKD3D experiments. It does not alter ordinary WineD3D, DXVK Legacy,
DXVK Modern, or default desktop launches. No wait, sleep, or flush compensation
was added.

## r10 ordering-fix boundary result

The fixed HAP has SHA-256
`1b4206216813583eaacd1692b5764ffd93c57c97276777df18718499fd41e974`.
Its embedded `wine-data.zip` remains
`ea2f9f9e410416dd2688469f032481b1d01cb3c93350a237cd83c06379db6be5`;
the Guest Vulkan loader is x86-64 and Host `libentry.so` is AArch64.

After a full uninstall/reinstall and clean prefix, r10 recorded both corrected
environment values, two `WineHua strong ring publish barrier enabled` records,
and completed 200/200 frames in 4,467 ms with exit code zero. The physical
SurfaceQueue reached frame 120 with 120 signals and zero failures. Retained r10
Host and Guest logs have SHA-256 hashes
`b80febffc700e414cda12a45bd06bbca2d2b22988e77afe52ea69310bfbea0ea`
and `487070e98872c07765f6f3def6bddf73a2d44eeeabfeeff6ed2b5784921ae42e`.

## Three clean 1,000-frame physical-display results

Each qualifying run used the fixed HAP, a unique experiment ID, full bundle
uninstall/reinstall, and a clean Wine prefix. The Guest emitted frame 1,000,
wrote `status=PASS`, and exited zero. The Host SurfaceQueue reached at least
frame 960 with zero failures in every run.

| Experiment | Guest result | Elapsed | FPS | Host frame 960 |
| --- | --- | ---: | ---: | --- |
| `vkd3d-500k-f74c040a-graphics-r11-1000f` | PASS 1000/1000 | 14,405 ms | 69.420 | signals=961, failures=0 |
| `vkd3d-500k-f74c040a-graphics-r12-1000f` | PASS 1000/1000 | 14,842 ms | 67.376 | signals=963, failures=0 |
| `vkd3d-500k-f74c040a-graphics-r13-1000f` | PASS 1000/1000 | 14,946 ms | 66.908 | signals=963, failures=0 |

The asynchronous release signal counter can be ahead of the published-frame
counter while queued buffers retire; no signal operation failed. None of the
three runs contains a command-stream decoder error, fatal ring abort, sleep,
`vkDeviceWaitIdle`, or global-flush workaround.

| Log | SHA-256 |
| --- | --- |
| `vkd3d-graphics-r11-1000f-host.log` | `7bb58f4bb0f20b4875311ff17e8e888d304d4dc2f79b58cb1a50b6ec212f76dd` |
| `vkd3d-graphics-r11-1000f-stderr.log` | `ccb99419a2e19326174d8651a15ef0bfae87c2769c4bf352ea3b752b8ddf345a` |
| `vkd3d-graphics-r12-1000f-host.log` | `dbcde49adca2a59d8ebef2d707f20671195ff3b7f810497b1193eff963b7b9d1` |
| `vkd3d-graphics-r12-1000f-stderr.log` | `55eed356cc08e296ed36763c2f7aa03e883ecedb77046645b037c28c29cd03bd` |
| `vkd3d-graphics-r13-1000f-host.log` | `f58467ccd6c8d411c47947845f7d1b3d6db09879463e4639457393a6cada8b1d` |
| `vkd3d-graphics-r13-1000f-stderr.log` | `7a2c8149fce935f2af756f50622edb0f12717f33c234e213b7d3f0fcd4fc3f92` |

This completes the isolated 1,000-frame physical-display gate on the recorded
910 device and exact runtime artifacts. D3D12 remains default-off. BDA,
multiple queues, BrokerPresent, and the full DXVK regression remain mandatory
before any real DX12 game qualification.

## User-facing manual button qualification

The packaged, default-off `VKD3D 2.6 (500K)` button was exercised after a full
bundle uninstall/reinstall on the same 910 device. Two launch-boundary defects
were corrected without changing the default DXVK runtime:

- managed-window mode no longer implicitly starts the full core smoke suite;
  only an explicit `smokeRequest.mode=smoke` request may do so;
- the packaged `d3d12.dll` is copied only into the app-owned clean
  `.wine-smoke/drive_c/smoke/x64` directory, beside the standalone PE where
  Win32 `LoadLibrary("d3d12.dll")` searches. The normal `.wine`, DXVK overlays,
  and user game directories are not modified.

The final signed HAP (`SHA-256
ede5fa0888c2e503ecd4ecaa76d467d79b583b99a31443586cbbc40e19493e3c`)
embedded the exact assembled `wine-data.zip` (`SHA-256
688ca5c69e30b3880750c598b1cb8b0e2449392791ad1ac542511987a6e047c0`).
The staged and runtime `d3d12.dll` both matched
`c0fc7447f6b298db02330e89f48053806a13349f05eaac01af4e0d3b2d1a6149`;
the x64 smoke executable matched
`cba49085ffcb6e2e98380977253ed0a06863ddc2b671e998e294af3683478dc5`.

The manual 1,000-frame run completed in 14,592 ms at 68.531 FPS and exited
zero. Guest checkpoints reached frames 1, 120, 960, and 1,000. The physical
SurfaceQueue reached frame 960 with 961 release signals and zero failures; the
presenter attached the requested target after 15,757 microseconds. No decoder
error, fatal ring abort, device loss, or failed SurfaceQueue release was found.
After the experiment, a cold application restart selected
`WINEHUA_D3D_BACKEND=dxvk_legacy` and restored the normal blue Wine desktop.

Retained evidence outside the repository:

| Evidence | SHA-256 |
| --- | --- |
| `vkd3d-manual-ui-910-20260805-host.log` | `490b2bdfdcb22c77583a739a2bf71f739e6c0fd572317f8d0707a90c683f6900` |
| `vkd3d-manual-ui-910-20260805-stderr.log` | `7fab1141d630627caad39a02893834a971b06aca807b9380a7881c43b41d4c15` |
| `vkd3d-manual-ui-910-20260805-hilog.log` | `9c4ba3c51cf90ae5baef934b8f60d2ca17ea0a3fb353458f5771762854155f2f` |
| `vkd3d-manual-ui-910-20260805.jpeg` | `27e7001ca8a04f6dcf9ab20efdab74dc80158e8ca6f36caf3d79e4eec6657f3f` |

## User-visible result reporting qualification

The manual button now passes explicit `--result` and `--checkpoint` paths to
the isolated graphics smoke. The ArkTS UI monitors those files and reports the
current D3D12 stage and frame count while running. It retains the authoritative
PASS/FAIL result, frames, elapsed time, FPS, and failure stage after the PE
exits instead of allowing the generic process callback to replace the result
with `Ready`.

The reporting change was rebuilt and tested after another full
force-stop/uninstall/install cycle on the same 910. The signed HAP was newer
than the source and had SHA-256
`cb6b2413d35add3a2c658e1865a7e5ab8af68144bdec7edc0d9dbd9fee707f6e`.
Its embedded and assembled `wine-data.zip` both matched
`688ca5c69e30b3880750c598b1cb8b0e2449392791ad1ac542511987a6e047c0`;
Guest EGL remained x86-64 and Host `libentry.so` remained AArch64.

Before entering the experiment, the normal DXVK Legacy prefix reached the
blue Wine desktop. The opt-in run then generated this authoritative result:

```json
{"status":"PASS","stage":"complete","frames":1000,"target_frames":1000,"elapsed_ms":15318,"fps":65.283,"width":640,"height":480,"sync_interval":0}
```

The UI displayed `PASS: 1000/1000`, `15.318 s`, and `65.283 FPS` after
completion. The PE exited zero. The packaged and isolated runtime copies of
`d3d12.dll` both matched
`c0fc7447f6b298db02330e89f48053806a13349f05eaac01af4e0d3b2d1a6149`;
the smoke executable matched
`cba49085ffcb6e2e98380977253ed0a06863ddc2b671e998e294af3683478dc5`.
The retained Host and stderr logs contained no device loss, decoder error,
fatal ring abort, release failure, or `vkDeviceWaitIdle` record. A cold restart
again restored the normal blue DXVK Legacy desktop.

Retained evidence outside the repository:

| Evidence | SHA-256 |
| --- | --- |
| `vkd3d-ui-result-910-20260805-host.log` | `47d6a3e6908ffff09949b2aa148b9b46cbda29a483f8b7af7646d11474bd4bd2` |
| `vkd3d-ui-result-910-20260805-stderr.log` | `63b9fc1ed50edb9d7e7ac013aba36ffd58ab33ccc1e363359461529cf9f3a2e0` |
| `vkd3d-ui-result-910-20260805.json` | `fc50ba8741fc6433a43e7616c7d4106b81af16542dcc0c9e867b59b637036621` |
| `vkd3d-ui-result-910-20260805-checkpoint.json` | `dbdf1d85b54d180d008c5a66bcb924289bad1fad37c69d0016a1924d443ec40a` |
| `winehua-vkd3d-after-result-910-20260805.jpeg` | `025ed97b59c93462f00bdf10de10f91bd9090b92c6c2d26e46331038d35ec9e9` |
