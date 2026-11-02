# PR #78 validation follow-up — November 1, 2026

The old-baseline rollup now passes the single-device core, Wine Vulkan, DXVK and
D3D12 suites. These results do not validate the separate upstream integration
branch or constitute a clean Wine/Mesa/VirGL rebuild.

## Corrections and reproduction

An initial locally linked HAP omitted staged native libraries. It installed but
failed to load `libentry.so` at startup. Copying physical native files and
cleaning Hvigor's cached library packaging restored all 28 packaged libraries.
The package verifier now rejects missing or empty runtime libraries, including
the originally omitted staged dependencies; regression fixtures cover that case.

Phone management intentionally stays foreground during ordinary startup.
The smoke harness previously launched its producer behind that page, yielding
hundreds of producer frames but no compositor display sequence. The shared
SmokeRunner path now receives the actual foreground caller context for cold,
warm-Want and UI requests, awaits the desktop root, requests its Ability, and
waits for the surface before launching tests. Normal startup opens no desktop.
Rejected foreground requests produce a failing summary with the actual error.
Runtime upgrades cannot silently turn `prefix=reuse` into a destructive reset.

The failing visual/result checks were retained unchanged. The final run passed
both the Guest result and host screenshot checks.

```text
core-core-foreground-fixed                         PASS 4/4
wine-vulkan-wine-vulkan-pr78-followup               PASS 4/4
dxvk-dxvk-pr78-6bf3abe                              PASS 3/3
d3d12-d3d12-pr78-6bf3abe                            PASS 3/3
core-core-linux-dns-rebuilt                        PASS 4/4
```

These directories live under the review worktree's `build/automation-logs/`.
D3D12 includes the 1000-frame probe and 600 frames each of gears and triangle.
No clean-prefix gate was run on this development device.

The second core run used Linux-compiled x64/x86 DNS probes, avoiding the Windows
GCC 16.2 internal compiler error. Its graphics probes still came from the
existing Wine PE build. The new core payload version is `smoke-v2-0aceb71ba65f`;
`build/linux-core-payload/build-provenance.json` records its source boundary.

## Artifact identity

The final HAP's CRC, runtime manifest, payload hash, native markers and library
inventory passed local checks; installation signature acceptance was verified by
the device. Native/ArkTS were built in this follow-up; bundled runtime dependencies
were reused.

| Artifact | SHA-256 |
|---|---|
| `6bf3abe` HAP used for DXVK/D3D12 and rebuilt-DNS core | `98227b4b76a080594604576ef701fba09f9e06c082c3609585129cea7d741746` |
| Wine payload | `d6eef4ffaecf84e45fc431d878aeaf108471b9fddb2df061b9e6fa0e4c1cf58a` |
| rebuilt-DNS core host summary | `b9cbe28d6f734eb58e5ab4fb4c36ce0620b7ecd5a8fe4a53e22ec0425a8306e4` |
| Linux host report ZIP | `2c37e389f76f7db18c3a394c4950ecef7f9b4bda278ac61a86db8edeb9f70165` |

The first complete core and Wine Vulkan runs used the same final foreground
logic before the additional reuse-upgrade protection was added.

## Host checks and remaining gates

The isolated Linux snapshot completed all 27 compile/execution stages with
ASan/UBSan where declared by the harnesses: renderer, root configuration,
bufferless composition, transfer lifetime, signal reaping, descriptor ownership,
SPIR-V qualification, ELF relocation and IME fields. Windows completed eight
JavaScript groups and the package-integrity regression. Linux had no Node, so
the JavaScript evidence is recorded separately.

Official master has advanced to `a001e6d`, 1038 commits after the original
`3eecbd7` baseline. A separate `integration/pr78-upstream` checkpoint resolves
the textual conflicts while retaining upstream display-route and input-sink
contracts. Its host JS and renderer checks are separate from the above device
runs. That checkpoint still needs the new ARM64 X11/wlroots build dependencies,
complete rebuilding, and its own device validation. Product/frontend routing to
`main-ui`, multiple devices, complete IME composition and longer real-game runs
remain unfinished. PR #78 stays Draft.

## Rebuilt DXVK candidate

Both legacy PE architectures were rebuilt in an isolated Linux workspace from
DXVK `5058927a741132fd643d20800f23e62dda432760` with the repository compatibility
patch. The provenance includes the additional Wine BC decoder header hash.
The incremental packager changes only 12 legacy DLLs and the DXVK manifest;
all other Wine payload members are preserved. Package tests and the complete
16-DLL HAP inventory/hash check passed.

| Artifact | SHA-256 |
|---|---|
| Selected HAP | `14d08b585c05b44ca4326c29fb1b8c5ec75a7a8d8294eff4498a30cf41556601` |
| Selected Wine payload | `138733418e6deb7b525606805a1fe0e9526a53567ec377e92b0904eebd899b3e` |
| x64 legacy D3D9 DLL | `724f20a5fb028707be3244ba58707310e3a2b01e861dc09d3332a69947fb272a` |
| x86 legacy D3D9 DLL | `c2c6bdf1c7fa417f8de7a6a2e68ffce7b66950a206049ffbd51be2a35e570264` |

The debug HAP installed successfully. A task-specific migration checked the
exact old runtime marker and all 13 affected paths, saved matching backups,
confirmed the engine was stopped, copied only managed DXVK files and updated
the runtime marker last. Every changed file was read back and hash-verified.
The prefix, game directories and saves were not reset.

The dedicated `dxvk_legacy_d3d9` suite remains **FAIL 1/4** in
`build/automation-logs/dxvk-d3d9-dxvk-d3d9-rebuilt-138733/`: x64 D3D9 returns
`0x8876086a` at device creation; both x86 cases report a D3D9 DLL load failure.
The same profile's x64 D3D11 case passes with 607 frames and host screenshots.
An info-level x64 repeat also fails at device creation. These are unresolved
runtime failures, not a passing D3D9 qualification. The earlier core, DXVK,
Wine Vulkan and D3D12 results used the old payload and do not validate this one.

## November 2 integration rebuild and Vulkan boundary

The integrated technical branch completed an isolated full Guest rebuild and a
canonical assembly. Repeating assembly produced the same runtime payload hash:
`48c0ec3b476948359adedf51359a76216426d1ac48424253c0b74ed55935c497`.
The imported runtime contains 4,591 verified outputs. The phone received all
2,981 staged files through an app-owned two-directory swap; the prior managed
runtime remains backed up and the existing prefix, games and saves were not
reset.

Host regression completed 35 technical stages. The integrated device candidate
passed core twice (4/4 each), DXVK (3/3), D3D12 (3/3), D3D9 (4/4), X11 GL
(1/1) and X11 DXVK (1/1). The Wine Vulkan suite passed two complete repetitions,
then failed its third repetition in the x86 full and sampled-only cases. Later
diagnostic repetitions reproduced the same `0xdeadbeef` first-compute sentinel
on either architecture.

The failure was retained and narrowed rather than retried away:

- Guest shader bytes, descriptor identities, command-buffer identity and
  `vkCmdDispatch(1,1,1)` reached the Host in matching pass/fail runs.
- The Host mapping and shared shadow both retained the sentinel on failure, so
  the final Host-to-Guest copy was not the corrupting step.
- Direct waits, status polling, disabled command batching, fence feedback
  changes, descriptor serialization, queue/device idle, an explicit
  compute-to-Host barrier, a Host-write barrier, pipeline delay and device-local
  output/readback did not eliminate the failure.
- Running the x86 full case alone passed five consecutive fresh sessions. The
  intermittent failure appears only when several short-lived Vulkan clients are
  exercised in one Host-server session. A one-second inter-test delay and an
  unconditional teardown `DeviceWaitIdle` did not make the aggregate stable.

The forced experimental host profile was removed from the product path. Session
startup again reads `winehua.perf.profile` and falls back to `baseline`; the
unqualified inline-upload coverage profile is no longer silently enabled for
every program. The rapid multi-client compute failure remains an explicit merge
boundary for the Wine Vulkan aggregate and must not be reported as resolved.
