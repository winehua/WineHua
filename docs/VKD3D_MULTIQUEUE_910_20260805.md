# VKD3D Multiple Queue Evidence — 910 — 2026-08-05

## Scope

This is isolated evidence for the `feature/vkd3d-capability-probe` branch. It
does not package VKD3D into the default Wine runtime, alter the validated DXVK
profiles, or enable D3D12 for ordinary application launches.

- Device: `62T0225B10005882` (Maleoon 910)
- Main runtime ordering fix: `5dde3da`
- VKD3D-Proton source: `5dd42d711dd069929d4666c51dc63e7d1e6a9dc6`
- VKD3D-Proton version: `v2.6-4-g5dd42d71`
- VKD3D build ID: `5dd42d711dd06990`
- Signed HAP SHA-256: `1b4206216813583eaacd1692b5764ffd93c57c97276777df18718499fd41e974`
- Embedded `wine-data.zip` SHA-256: `ea2f9f9e410416dd2688469f032481b1d01cb3c93350a237cd83c06379db6be5`
- `d3d12.dll` SHA-256: `f1f35953a4ac6afe5b8a3cd3f6d56bec1affd82e57427a16b7920b6387bee208`
- Microtest SHA-256: `b1d556f5b9f024152e6af820b91c43df9014cc2e5b6d4e7c32b17c6dad22445f`

The cross build ran inside `winehua-master-ext4` and exited zero. Both payload
files are x86-64 PE files. The microtest imports only `KERNEL32.dll` and
`msvcrt.dll`; it loads the hash-verified experiment-local `d3d12.dll` after
`main()` starts.

The HAP was built at `2026-08-05 02:01:05.957185477 +0800`. Its embedded
`wine-data.zip` matches the assembled rawfile. The Guest Vulkan loader is
x86-64 and Host `libentry.so` is AArch64.

## Queue and resource flow

The microtest creates separate D3D12 COPY and DIRECT command queue objects and
three command lists. The resource path is:

`upload -> COPY queue -> default A -> DIRECT queue -> default B -> COPY queue -> readback`

Submission order is enforced entirely on the GPU timeline:

1. COPY submits upload to default A, then signals shared fence value 1.
2. DIRECT waits for value 1, transitions/copies default A to default B, then
   signals value 2.
3. COPY waits for value 2, copies default B to readback, then signals value 3.
4. The CPU waits for value 3 with a D3D12 fence event and compares all 256
   deterministic bytes.

This verifies two cross-queue dependencies, two COPY submissions, one DIRECT
submission, upload/default/readback resources, transitions, and byte-accurate
completion. It proves the D3D12 logical multiple-queue path; it does not claim
that the driver maps those queue objects to distinct physical engines.

The source contains no sleep, `vkQueueWaitIdle`, `vkDeviceWaitIdle`, or global
flush compensation. The CPU event has a finite failure timeout and is used
only after the final GPU signal.

## Qualified runs

Every run used a unique experiment ID and the following clean-device sequence:

`force-stop -> uninstall -> install -> setmode 602 -> clean prefix -> launch`

The staged artifacts were re-hashed before import. All runs recorded
`BOX64_DYNAREC_WEAKBARRIER=0` and
`VN_WINEHUA_STRONG_RING_BARRIER=1`.

| Experiment | COPY submits | DIRECT submits | Cross-queue waits | Bytes | Final |
| --- | ---: | ---: | ---: | ---: | --- |
| `vkd3d-500k-5dd42d71-multiqueue-r17` | 2 | 1 | 2 | 256 | `PASS/complete/result_written=1`, exit 0 |
| `vkd3d-500k-5dd42d71-multiqueue-r18` | 2 | 1 | 2 | 256 | `PASS/complete/result_written=1`, exit 0 |
| `vkd3d-500k-5dd42d71-multiqueue-r19` | 2 | 1 | 2 | 256 | `PASS/complete/result_written=1`, exit 0 |

The application hilog observed the persisted result JSON in its private Wine
prefix. The test emits its final record only after object teardown,
`FreeLibrary(d3d12.dll)`, and successful result-file persistence.

## Retained log hashes

The accessible Host and Wine logs are retained outside the repository at
`D:\MyProject`.

| Log | SHA-256 |
| --- | --- |
| `vkd3d-multiqueue-r17-host.log` | `db0272baf7600704dcf55a820abcd6c6d23c87e2932238ebf5875271646e7ecc` |
| `vkd3d-multiqueue-r17-stderr.log` | `07baf701ff5580a8d082e552144589a2451911470231f05370f340da0c7c6424` |
| `vkd3d-multiqueue-r18-host.log` | `81930c585fd2bb3ecc9be7eff07434d507d0fa09b21dcf0b233092dae255a1ea` |
| `vkd3d-multiqueue-r18-stderr.log` | `5a3b746799ab9b3faa7ec9e36dc318609a15d095f933da5ddfff98fe3e851b77` |
| `vkd3d-multiqueue-r19-host.log` | `0b95a32553727532bbdc0e1757593d4a2356936d36a48ec0c775ce0bcb7e98d8` |
| `vkd3d-multiqueue-r19-stderr.log` | `00378d172e5134c267740985a287687408955519a19edaecfcc079e8beed85bd` |

None of the six logs contains a command-stream decoder fatal state, Venus ring
abort, or device-lost record.

## Decision

The isolated VKD3D-Proton 2.6 limited-500K profile passes the multiple-queue
follow-up gate three consecutive times on this 910 device for the exact
artifacts above. This is not general D3D12 or game qualification.

D3D12 remains default-off. The already completed 1,000-frame physical
BrokerPresent evidence and a full DXVK product regression must be reconciled
before any real DX12 game test.
