# Steam win64 Stage 1: device continuation (2026-09-19 evening)

This is a new on-device investigation. It supersedes the causal claims in the
September 19 handoff where the observations below disagree. Stage 1 has **not**
passed. The raw screenshots and one Steam assert dump are local only, under
`F:\WineHua\device-evidence-20260919-steam-g1\`; do not publish them without
reviewing personal content. Steam login data and full command lines were not
copied into this report.

## Identity and scope

- Device: connected USB tablet, model `MLR-AL10`; bundle `app.hackeris.winehua`.
  Each run used the existing `default` container and prefix, with
  `-cef-force-gpu -no-cef-sandbox`. The app's CEF software override was unset;
  its `WINEDEBUG` summary reported `-all`.
- Main source: `32efc9c709d19a973942f967e96cb062bd84f2f7`, tree
  `d400ed3ccf493900572a15e09c6098f172d71fa7`. Build source is
  `thirdparty/wine-valve` at `f90646a3c5a6a54de6da03eebb24f356145c82f4`,
  tree `ca5bf4a5820e2163f292d71d0839101761df3c3b`. The latter tree is
  byte-identical to CI Wine commit `c0cc03e377efe88763892496a45016dfc2946fc5`.
  Local source changes made during these tests are uncommitted.
- FEX gitlink: `86ff33bbe299cd8959a6610198c169b67ec419db` plus the patches
  listed in `scripts/build_fex.sh`. The registered `thirdparty/fex` is absent
  locally; the build uses `build/fex-src`, which is not an independent Git
  checkout. The unpacked device `libarm64ecfex.dll` matches the staged file:
  SHA-256 `9679bed365e9f7a9ba5801a4e5ccc26758b7efa50a61ee62a7cd1b39b2956d34`.
- The same Windows Steam file set was observed before the first run:
  `steam.exe` SHA-256 `48ea0576865d2dfda26001b7b210c3f7559d46e647424cd11704eb1ea842fe5a`;
  `steamclient64.dll` `caba4826aa3501039d095aee1843a6bfb270fb43a3ab4455b2d6733223579fee`;
  `cef.win64/steamwebhelper.exe` `f9ee1d1cc0f06fad16c9a277136a2e1b00b6b668901e28454a7dd8af29afde0d`;
  `cef.win64/libcef.dll` `c85bf94462b3c79baeac63823db52d15e116eead71ea414e5d3a112c59e434d3`.
  `transport_steamui.txt` reports client version `1788652215`.
- `git status` of the main repository needs `--ignore-submodules=all` here:
  the `thirdparty/dxvk` Git metadata is missing. Nothing was reset, cleaned,
  or replaced to hide that limitation.
- F/G used the same signed HAP SHA-256
  `de5217dd9896a881fcd067555ec75cb73584528c177ddb44500369246c6150d2`.
  Its payload SHA-256 was
  `7b877093042b4728bce62f44757aabb9ab7fdb732c830306c0bd221b6d5b8ccc`;
  the device runtime manifest matched after a game Want initiated the managed
  refresh, with the existing prefix retained. The packaged native `ntdll.so`
  SHA-256 was `7ec8d20d95e2ca44b1c3a5bf7825d6e59f05bd3f4e6caa01bdf5f7a170848f22`.
  F's `[wait-object]` output confirms that the instrumented native path ran.
  Both rounds retained `steam.exe` SHA-256 `48ea0576...e5a` and
  `steamwebhelper.exe` SHA-256 `f9ee1d1c...de0d`. Starting the App without a
  game Want initially left the previous payload installed behind its upgrade
  prompt; no run was counted against that stale runtime.

## On-device runs

All times below are device-local. Each run began only after `aa force-stop`
and a check for remaining Wine child processes. Log byte offsets were saved
immediately before launching Steam; the stage markers below come only from
those new byte ranges. The observation window ran at least 180 seconds after
the new `CreateResponse`/`AfterCreated` pair.

| Run | HAP SHA-256 prefix / unpacked payload SHA-256 prefix | Variable | New Stage 1 markers | Result |
| --- | --- | --- | --- | --- |
| A, 21:29 | `e00d01cf` / `df6fca4f` | Original diagnostics | `CreateResponse` twice at 21:29:17/23; `AfterCreated` twice | `TIMEOUT_STAGE_1` at 21:32:19 |
| B, 21:35 | `5636c965` / `1edf258d` | New `WINEHUA_DIAG_QUIET=1` | Both markers at 21:35:45; child logs confirm early-fault logger off | `TIMEOUT_STAGE_1` at 21:38:52 |
| C, 21:42 | `8cf03e6b` / `4a35b8f9` | Quiet plus paired `WINEHUA_WAIT_TRACE=1` | Both markers at 21:42:08; browser host PID 23053, Steam host PID 22791 | `TIMEOUT_STAGE_1` at 21:45:09 |
| D, 21:47 | `8cf03e6b` / `4a35b8f9` | Same HAP, quiet, wait trace off | Both markers at 21:47:34/35 | `TIMEOUT_STAGE_1` at 21:50:47 |
| E, 21:54 | `babee72e` / `fb0e87b3` | Quiet plus sampled wait trace | `CreateResponse` and `AfterCreated` at 21:54:49; browser host PID 29193, Steam host PID 29080 | `TIMEOUT_STAGE_1` after 22:00:47 |
| F, 22:10 | `de5217dd` / `7b877093` | Quiet, wait trace and new `WINEHUA_WAIT_OBJECT_TRACE=1` | Paired markers at 22:10:27, 22:10:35 and 22:10:46 across three browsers; two exited with SIGSEGV, third host PID 36150 remained | `TIMEOUT_STAGE_1` at 22:14:08 |
| G, 22:15 | `de5217dd` / `7b877093` | Same HAP, quiet, both wait probes off | Paired markers at 22:16:06; browser host PID 38598 remained, with no browser exit recorded | `TIMEOUT_STAGE_1` at 22:19:25 |

For A-G, the new ranges have no `BrowserReady: handle:65536`, `SetName:
SP Shared JS Context`, or renderer creation. A/B/C screenshots show the blue
Wine desktop rather than HTML content. D's screenshot contains a personal
notification and must not be used as a shareable artifact. The same Steam
profile was retained but **not restored from one fixed snapshot** between
runs, so this is exploratory evidence, not a controlled multi-run pass rate.

## Wait and lifecycle findings

In C, the new wait probe recorded enter/exit with a per-process call ID,
NTSTATUS, elapsed monotonic ticks and the selected backend for single,
multiple and signal-and-wait NT calls with either representation of an
infinite timeout. For Steam PID 22791/TID 22791, the first waits on handles
`0x40` and `0x7c` returned `STATUS_SUCCESS` through `server` in 708 and
1,375 100-ns ticks respectively. The browser main thread also had successful
short waits. This disproves the earlier inference that repeated *entry-only*
lines prove a permanent wait on `0x40`/`0x7c`. It does **not** prove the
BrowserReady notification was delivered: handles have not been associated
with object generations or the Steam protocol.

At a C snapshot, 3,910 wait entries had 3,853 exits; 57 calls were open at
that instant across processes. Most observed exits were `backend=server` and
`status=0`; nonzero small status values also occur as successful wait-any
indices, so they cannot be called errors by number alone. Steam PID 22791
reached the first implementation's 2,048-call cap, creating a blind interval.
The final code samples one out of each 128 later calls and announces the
coverage change; E verified the notice and Steam PID 29080's paired entries
at IDs 3968 through 5504. E's browser `CrBrowserMain` (host TID 29324)
repeatedly entered and returned successfully from waits on its own process
handle `0xa8`; call ID 244 entered and had no exit before the 22:00 capture.
The Steam main thread continued to return from its sampled server waits.
The E session was then ended with `aa force-stop`; no Wine child processes
remained. Finite waits,
CEF-internal futexes, keyed events, and object identity are still outside
this trace.

In F, the new read-only object probe identified browser `CrBrowserMain`'s
handle `0xa8` within browser PID 36150 as an **unnamed auto-reset Event**
(`NtQueryObject` type/name success, `NtQueryEvent` type 1, state 0).
Its server-backed wait ID 134 returned `STATUS_SUCCESS` after about 0.73 s.
The same process's main thread subsequently called `NtSetEvent(0xa8)` with
success; an immediate post-call query still read state 0, consistent with an
auto-reset event being consumed. `CrBrowserMain` then entered wait ID 184 on
`0xa8`; it had no paired return by 22:14:08, and the targeted trace recorded
no further Set/Reset for that handle. A different F browser PID 35942 showed
the same pattern before its later SIGSEGV. Queries and Set/Reset records are
from **within each browser process**; they do not identify an object in the
Steam host, prove that a task should have been queued, or prove the callback
following `AfterCreated` returned.

The optional query adds server calls and changes timing. Two initial F browsers
exited with SIGSEGV after 8.9 and 6.4 seconds; G, on the identical HAP with
both wait probes off, retained its first browser beyond 180 seconds and still
failed Stage 1. One F/G pair is insufficient to attribute those crashes to
the probe or to claim the quiet baseline never crashes. The event evidence
rules out only the simple claim that *the observed* `0xa8` wait never received
any signal: one wait did return successfully, and the later wait had no
observed producer. The absence of a new signal may be normal idle message
loop behavior. No protocol-level producer/consumer link has been established.

Steam's `cef_log.txt` in F/G repeats `WSALookupServiceBegin failed with: 8`.
The same error appears in the surviving September 18 14:52 CEF log alongside
an earlier renderer run; it is not a new distinguishing Stage 1 failure.
No matching renderer child was created in F or G.

The browser process leader's `/proc/<pid>/task/<pid>` entry disappeared while
other threads, including `CrBrowserMain`, remained. This happened both with
the wait trace enabled (C) and disabled (D); whether it is a normal CEF/Wine
thread transition remains unknown. The broker recorded browser, GPU and two
utility children in C, but no renderer child. Do not treat surviving browser
threads as proof its initial callback returned.

Steam generated the same `Win32Font.cpp:1129` assertion (`Couldn't get string
length`) in the earlier A-E runs; F/G were not checked for this assertion.
An assertion with the same text is also present in
the device's 2026-09-18 20:50 dump, so temporal coincidence alone cannot make
it the cause of this Stage 1 regression. The device's surviving 20:50 logs
show `CreateResponse`/`AfterCreated` but no same-run `BrowserReady` or
`SetName`; that round is not a confirmed Stage 1 success. Older 14:55
`webhelper.txt` lines do contain `SetName` and renderer requests; their full
HAP/Steam/profile identity is not established by this investigation.

## Changes and next boundary

`WINEHUA_DIAG_QUIET=1` is a new, opt-in Want environment key. It stops App
early-fault diagnostic processing and Wine SMC/TRAP formatting and stack
inspection while preserving the fault router, FEX call, Wine SEH and trap
handler calls. The updated wait probe pairs entries and all inproc/server
exits, reports `fsync`/`ntsync`/`server`, and preserves `errno`. The code is
uncommitted in the main tree and in the Wine worktree. The additional
`WINEHUA_WAIT_OBJECT_TRACE=1` probe is opt-in: it queries the first observed
low-valued indefinite waits from `CrBrowserMain` and records matched
`NtSetEvent`/`NtResetEvent` calls in the same process. It does not track
handle close/reuse or assign a cross-process object ID. Wine build, runtime
assembly, HAP signing, payload closure and device installation succeeded.

Next locate the actual browser/client initialization task after `AfterCreated`
and its producer, then connect it to a same-generation IPC object or a typed
FEX guest boundary. Avoid assuming that `CrBrowserMain`'s unnamed wake event
is the Steam cross-process notification. Recheck the two F SIGSEGV exits in a
separate controlled comparison before using this optional probe for longer
runs. Do not change GPU options or assume a sync bug merely
because a worker remains in an indefinite wait. Stage 2-4, login, and game
launch were not validated.
