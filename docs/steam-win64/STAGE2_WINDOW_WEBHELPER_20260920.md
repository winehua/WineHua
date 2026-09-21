# Steam win64: window binding and webhelper follow-up (2026-09-20)

> September 20 evidence review: see
> [ROOT_CAUSE_REVIEW_20260920.md](ROOT_CAUSE_REVIEW_20260920.md).
> The saved trace identifies CEF font-creation failure followed by recursive
> default/system-font initialization. A read-only tablet audit also found
> inconsistent HKCU external-font and HKLM DirectWrite font registration.
> Prior broad callback/stack hypotheses and the proposed next experiment
> below are superseded by that review. A font-only repair is not yet tested.

This is a device observation after the user reinstalled Steam. It updates the
Stage 1 outcome in `STAGE1_DEVICE_EVIDENCE_20260919.md`: this installation
reached `BrowserReady`, `SetName`, renderer creation, login, and the desktop
window. It does **not** establish a reliable, usable library UI. The Steam
profile and installation were not cleared during this investigation.

Raw evidence is local under
`F:\WineHua\device-evidence-20260920-clean-steam\`. Screenshots and Steam
logs may contain account information; do not publish them as-is. Times below
are device-local on September 20. App host PID 41589, Steam host PID 28874,
browser host PID 28930, and GPU/presenter host PID 28970 were alive during
the initial window sequence. The Steam client reported build 1788652215.
The HAP and runtime hashes were not revalidated in this follow-up.

| Time | Same-run observation | Implication |
| --- | --- | --- |
| 10:56:39 | `BrowserReady: handle:65536`, `SetName: SP Shared JS Context`, renderer request | The previous initialization barrier was crossed in this installation. |
| 11:13:37 | Login window 700x440, producer `0x712a00000003`, `binding=bound`, `class=OK` | Its native frame was consumed by the compositor. |
| 11:13:41-11:13:56 | Steam desktop window 1280x800, owner `(28930,35)`, toplevel 24, producer `0x712a00000018` bound by `unique-geometry`; snapshots report `class=OK`, producer and draw ages 0-3 ms | Initial main-window matching and frame consumption worked. Popup 706x800 and friends window 300x650 also bound. |
| 11:13:57 | Steam UI logs `Uncaught (in promise) #<Object>` | A page error preceded the disappearance, but its cause and effect are not yet established. |
| 11:13:59 | `webhelper.txt` records `SP Desktop... WasHidden 1`; compositor receives explicit `xs_destroy` for toplevel 24 at 11:13:59.463 and destroys two popup toplevels shortly after | The Wine client removed the role before the compositor removed the window. This is not a first-bind failure. The browser process remained alive. |
| 11:15-11:22 | Only the Wine desktop, file manager, taskbar, and Steam tray icon are visible; producer 1280x800 stopped at 2495 frames | The white background is the exposed desktop, not proof of a blank active Steam window. |
| 11:22:58-11:23:10 | Tray interaction exposed Steam's `steamwebhelper` not responding dialog (700x330 SHM). `steamui_html.txt` logs `ShowJumpList`, then `Shutting down webhelper process 1416`; `webhelper.txt` logs `Quit message loop` | Steam itself detected a nonresponsive UI component. The controlled selection of the dialog's webhelper-restart option coincided with shutdown. |
| 11:26:51-11:38:01 | Old browser host PID 28930 is gone; no new browser start or `BrowserReady` in the Steam logs. Steam host PID 28874 and WineHua remain | Webhelper recovery did not complete in the observation window. A new `steamerrorreporter64.exe` host was briefly present; do not count it as a new browser. |

`WINDOW-REG visible=0` alone is **not** evidence that Steam was hidden:
`IsToplevelVisibleLocked()` includes `HasFrame()`, so native-only windows may
report zero without an SHM frame. Here the stronger evidence is `WasHidden 1`
plus `xs_destroy` and the loss of the actual toplevel state. Likewise, the
producer diagnostic's `binding=bound` after the role was destroyed does not
mean it still had a drawable window.

There is a separate, reproducible lifecycle defect in the current source:
`xdg_shell.cpp::xs_destroy()` calls `OnToplevelDestroyed()` and then
`wl_resource_destroy()`, whose `xs_resource_destroy()` repeats the cleanup and
the `Destroyed` event. It leaves `SurfaceData::hasToplevel` and `toplevelId`
set while the `wl_surface` survives, so registry snapshots retain a ghost
role. `wl_core.cpp` invalidates present bindings only when the `wl_surface`
resource is destroyed; the main producer still reported `binding=bound` after
its `xdg_surface` role and toplevel had gone away. The local source now
invalidates the binding when its toplevel role is destroyed, clears the
role/identity and geometry flag, and sends one `Destroyed` event. The patch
has not been installed on the tablet or exercised by a role-destroy/recreate
regression test. It cannot explain the
initial 11:13:41-11:13:56 successful display or establish why webhelper
stopped responding.

Verification of the source-only change: both modified translation units
passed the installed OHOS arm64 Clang syntax check; `toplevel_event_test`
passed 62 checks and `presenter_common_test` passed. The first five existing
host test programs passed, but `make test` could not proceed beyond them:
its `env_spec_test` rule references the nonexistent
`entry/src/main/cpp/env_spec.cpp` instead of `wine/env_spec.cpp`. No test in
this set exercises a real Wayland role teardown, so the device rebind check
remains open.

Next reproduce from the unchanged installation with bounded browser-thread
and Steam message-loop evidence around the **first** loss of progress. Record
the transition from the last successful renderer/CEF task to the first failed
call or callback, along with the browser/GPU child exits and actual loaded
backend. Separately verify whether Steam can restart webhelper and create a
new `BrowserReady` and window without clearing the prefix. Only after the
browser remains responsive should the local role-lifecycle fix be deployed and
checked for rebind behavior. This follow-up made no tablet runtime or GPU changes;
normal library display and three cold starts have not been validated.

## September 20 compositor build and tablet retest (12:21-12:40)

This follow-up used the existing Steam installation and prefix; no Steam data
was removed. The native-only change in `zc_bridge.cpp::GetOccluders` skips a
bound upper window's SHM occluder when its entire SHM frame is zero. The
ordinary SHM path is unchanged. It is a narrow workaround for an observed
black placeholder, **not** a repair of CEF rendering or a general policy to
hide black windows. The deployed version was described as scanning under the
compositor lock on each relevant overlay redraw. Later local source caches
the result by FrameSerial; performance and a same-state A/B remain open.

`NATIVE_ARCH=arm64-v8a bash scripts/package.sh hap` succeeded in the
`wineohos-build` container, including the runtime closure check
(`wineArch=aarch64`). The signed HAP SHA-256 was
`46753a0fcd61786e09330c04cd5cb9a68a197bb6151041f258d497ead7138437`;
the uploaded HAP had the same hash on target `5KPBB25818203996`.
`bm install -p /data/local/tmp/winehua-steam-occlusion.hap -r` succeeded.
This did not rebuild Wine, FEX, Mesa or the runtime rawfiles. Client build
remained `1788652215`. The screenshots and SHM captures below are local under
`F:\WineHua\device-evidence-20260920-steam-review\` and may contain
account information.

| Tablet run | Same-run facts | Visible result |
| --- | --- | --- |
| 12:23, `-cef-force-gpu -no-cef-sandbox`, shortly after app startup | `BrowserReady` and `SetName`; first renderer host PID 58766 exited SIGSEGV after 8.4 s | Only Wine desktop in `steam-occlusion-post.jpeg`; no main-window comparison possible. |
| 12:26, same args after 40 s app warmup | `BrowserReady`; renderer PID 60767 exited SIGSEGV after 9.5 s, before main window | Only Wine desktop in `steam-occlusion-cold.jpeg`. Warmup alone did not resolve it. |
| 12:30, same args plus `WINEHUA_DIAG_QUIET=1` | Login content visible; same-generation main 1280x800 and popup 706x800 both bound; main producer continued receiving frames; popup producer later aged into `ProducerDead`. One later GPU child and one renderer exited SIGSEGV. | Login rendered in `steam-occlusion-quiet.jpeg`. Main in `steam-occlusion-quiet-main.jpeg` and `steam-occlusion-quiet-steady.jpeg` had a top color/partial UI wedge but a mostly black body. No usable library. |
| 12:37, same quiet args plus `WINEHUA_CEF_ANGLE_BACKEND=vulkan` | GPU report confirmed `ANGLE_VULKAN`; login and main producers bound, but CEF reported requested GLES 3.0 above supported 2.0, missing Skia mailboxes, and GPU/renderer SIGSEGV | Black login then black main and eventual exposed Wine desktop in `steam-occlusion-angle-vulkan*.jpeg`. Vulkan is not a fix here. |
| 12:42, returned to quiet D3D11 args | `BrowserReady` and main 1280x800 binding recurred; renderer PID 4841 later exited SIGSEGV after 85.8 s and the window was removed | Wine desktop in `steam-occlusion-final.jpeg`. The quieter configuration is not a stable fix. |
| 12:47, `-cef-disable-gpu -no-cef-sandbox` plus quiet diagnostics | GPU report selected `ANGLE_SWIFTSHADER`; main windows had SHM frames but no native producer, and renderers 7702 and 8225 exited SIGSEGV | Entire display was black in `steam-occlusion-quiet-software.jpeg`. Disabling GPU did not recover HTML. |

In the 12:30 run the captured main and popup SHM frames each contained **zero
nonzero bytes** (4,096,000 and 2,259,200 bytes respectively). Their files
are `frame-main-62640-32.raw` and `frame-popup-62640-28.raw`. Thus the
visible login and partial main content came from a native producer. The
initial 706-pixel-wide black center block was not reproduced as the sole
artifact after the patch, but this is not a controlled before/after of the
same CEF state: the new main frame is itself substantially black. The
compositor patch cannot be credited with restoring the main screen.

The quiet D3D11 run's GPU report explicitly identified `ANGLE_D3D11` through
Virtio-GPU Venus. At 12:33:16 the CEF log repeatedly reported
`VertexDataManager::reserveSpaceForAttrib:523` and `GL_INVALID_OPERATION`:
"Vertex buffer is not big enough for the draw call." This is a concrete
guest-rendering error to reproduce with the same D3D11/ANGLE/DXVK/Venus
contract. A renderer exited SIGSEGV at 12:32:53 and a GPU process at
12:33:01. The first `WINEHUA_DIAG_QUIET=1` run progressed farther than the
two verbose runs, but its own later crashes and the second quiet run rule out
quiet logging alone as a stable fix. The switch suppresses diagnostic
formatting and leaves Wine/FEX exception routing intact. The current default generated
large amounts of fault and producer diagnostic output, so repeated quiet and
verbose cold-start controls are warranted before changing defaults.

Next, reduce the ANGLE D3D11 vertex-buffer failure to one draw call with
buffer allocation/stride/offset/vertex-count evidence, and correlate the
first renderer or GPU SIGSEGV with its guest and host stack. Keep the
occluder fix separate from this guest graphics investigation. Once native
frames contain correct HTML, validate simultaneous main/popup presentation;
the current EGL compositor only consumes one native surface at a time.
Neither normal library navigation nor three consecutive clean cold starts
passed this retest. Do not treat the HAP build or a `BrowserReady` line as
main-screen acceptance.

## September 20 browser crash and display controls (13:29-14:27)

The unchanged Steam installation and prefix were used throughout. An app-only
compositor A/B (new HAP `7cfdc606...` versus original `46753a0f...`) produced
the same browser SIGSEGV/restart pattern; it does not implicate the compositor
change. A temporary FEX boundary DLL and a diagnostic HAP were subsequently
removed. The tablet was left on the original HAP
`46753a0fcd61786e09330c04cd5cb9a68a197bb6151041f258d497ead7138437`,
with the original `libarm64ecfex.dll` SHA-256
`8aba586e7988b01cd1d24685bef778ed71f64da9dc3bf525bd720ef6596f0cfc`.
The Steam webhelper image inspected locally is the pinned AMD64 PE with SHA-256
`f9ee1d1cc0f06fad16c9a277136a2e1b00b6b668901e28454a7dd8af29afde0d`.

The temporary FEX trace from the pinned webhelper enters `AfterCreated` at
RVA `0x131950`, passes `0x131d11`, and calls `0x126ae0` from `0x131e27`.
The same Windows thread then runs a repeated call sequence: the trace calls
RVA `0x2e4360` 61 times while the guest stack pointer descends from about
`0xa1eb48` to `0x222938`. Disassembly identifies `0x2e4360` as a wrapper
which calls an object's virtual method at vtable offset 8 and decrements its
own reference count. This establishes repeated nested object calls, not the
identity of the faulty implementation or a FEX atomic/refcount defect.
Under the 8 MiB stack policy, browser host PID 49794 faults at `0x221f88`
inside its `00220000-00221000 ---p` guard region, and Wine logs
`unrecoverable thread stack overflow`. This is a real failure in that run.

At 32 MiB, browser PID 51774 maps `00220000-00222000 ---p` followed by
`00222000-02220000 rw-p`: the larger reservation actually took effect.
Nevertheless browsers still exit with SIGSEGV after roughly 6-9 seconds.
The `0x400000000` SIGSEGV records in that run include other host PIDs
(for example 51794), and the early-fault logger did **not** capture a
SIGSEGV for browser PID 51774. The first fatal browser signal in the 16/32 MiB
runs remains unlocated; equal exit times do not prove it is, or is not, another
instance of the recursive stack exhaustion. Do not increase the global stack
minimum on this evidence. The source-only change to `wine_child.cpp` that
reserves SIGSEGV fault slots was built in diagnostic HAP `448e1003...`, but
that HAP is not installed and the change is not a behavioral repair.

A final bounded probe at 14:30 temporarily substituted the previously used
FEX boundary DLL, set the 32 MiB minimum and quiet diagnostics, and retained
`-cef-force-gpu -no-cef-sandbox`. Browser host PID 7228 entered the same
callback with guest RSP `0x221ee88`; its Windows TID 1280 reached call record
19104 with RSP `0x1393950`, still well above the `0x220000` guard boundary.
The repeated stack descent did not stop, but Steam ended four successive
browsers with signal 3 (Quit) after 10.2-12.7 seconds, before this probe
observed a guard fault. The debug DLL changes timing and signal outcome, so
this does **not** identify the original 32 MiB SIGSEGV. Raw records are local
in `boundary-stack32-20260920-{wine-stderr,cef-diag}.log`. The App was stopped
and the original FEX DLL was restored with its SHA-256 checked again.

With the original HAP and FEX DLL, three additional launches used quiet
diagnostics and the same retained Steam data. Each reached `AfterCreated`,
restarted its browser with host signal 11, and never reached `BrowserReady`,
`SetName`, or a renderer request in the observed interval:

| Tablet run | Steam arguments | Observation |
| --- | --- | --- |
| 14:14-14:18 | `-cef-force-gpu -no-cef-sandbox` | Browser PID 58926 exited after 8.7 s, PID 59248 after 5.8 s; repeated restarts over 180 s. |
| 14:19-14:22 | `-no-cef-sandbox` | Browser PID 63954 exited after 5.7 s; repeated restarts over 180 s. |
| 14:25-14:26 | `-cef-disable-gpu -no-cef-sandbox` | Browser PID 3763 exited after 9.6 s, PID 4412 after 4.6 s; repeated restarts. |

The three same-run screenshots (`steam-run-20260920-1415.jpeg`,
`steam-run-20260920-clean.jpeg`, and
`steam-run-20260920-disable-gpu.jpeg`) are byte-identical and show only the
Wine desktop. They are private files under
`F:\WineHua\device-evidence-20260920-steam-review\`; the full logs there
may contain account information. Changing GPU selection therefore did not
recover the browser in these runs. The earlier 12:30 run on this HAP reached
a login window and partial main content, so the separate ANGLE D3D11
vertex-buffer failure and compositor presentation still need attention once
the browser can survive. There was no main window in these three controls on
which to test window matching or HTML pixels.

Next collect a bounded, same-process guest call/return and stack sample for
the **first** SIGSEGV with the 16/32 MiB reservation, correlating the browser
host PID/TID with the FEX state and Wine signal handling. Map recurrent
targets to the modules loaded in that exact process before changing a
refcount, exception, or stack contract. Re-run against the unchanged Steam
profile, then inspect renderer, CEF pixels, and native/SHM producers only
after a browser stays alive. No new Proton/Wine behavior change, font reset,
Steam reinstall, or runtime deployment was made in these controls; the App
was force-stopped afterwards. Usable login/library UI and three cold starts
are still unverified.
