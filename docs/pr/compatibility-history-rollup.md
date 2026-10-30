# Compatibility rollup: graphics creation, text rendering, resource lifetime, desktop recovery and input

## Purpose and scope

This replaces the narrowly scoped IME draft with a review of **all significant compatibility work
developed in this workspace**, including the earlier graphics-creation and missing-text fixes.
The baseline is the official `master` commit `3eecbd7`. The historical observations below were
recorded during October 25–29, 2026; this submission was assembled on October 30, 2026.

This is an integration-review **Draft**, not a claim that every game, device or historical failure
has been fully fixed. The description distinguishes implemented fixes, qualified compatibility
profiles, opt-in experiments, withdrawn experiments and incomplete validation.

The changes include native code, reproducible Wine/Mesa/VirGL patches, dependency/build fixes,
phone/frontend integration, the game library, and the associated regression tools. Commit groups
keep the technical and product changes identifiable. Under the project's branch policy, shared
technical fixes belong on `master`; the product/frontend group should be split or routed to
`main-ui` before merge. It is present in this integration draft so the historical contribution is
visible rather than silently omitted.

## 1. GL/EGL creation failures and invalid resource handling

- Validate `eglChooseConfig` success and the returned configuration count before creating a context.
- Reject `EGL_NO_CONTEXT` and report the EGL error. Stop the renderer after a failed
  `eglMakeCurrent` instead of continuing with an unusable GL context.
- Create and validate a replacement native window before retiring the existing renderer.
  Keep native-window ownership in one place, including initialization-failure paths, to avoid
  double destruction and recovery with an invalid handle.
- Carry surface identity through resize/destroy callbacks. Late callbacks from an old XComponent
  cannot destroy a replacement renderer; root migration resolves the actual surface owner.
- Retire and join replaced render threads before they can compete for frames or shared resources.
- Handle GL 3.2 applications requesting the promoted `glVertexAttribDivisor` name by using the
  compatible ARB entry **only when `GL_ARB_instanced_arrays` is actually supported**.
  This repairs a missing function-entry path; it does not fabricate extension support.
- Remove an unused SPIR-V `ShaderLayer` capability only for the qualified Maleoon workaround.
  Preserve genuine Layer variables/member decorations and leave unrelated/malformed modules alone.
  The isolated failing native pipeline changed from failure to success after removing the unused
  declaration; lowering SPIR-V or disabling MSAA alone had not solved that failure.

These are distinct creation/entry-point/lifetime defects. They must not be summarized as one
universal "invalid GL handle" root cause or as proof that all GL crashes are eliminated.

## 2. Missing Chinese text and legacy bitmap text

Two separate defects affected the legacy DirectDraw game:

1. English font-family requests could find CJK glyphs while Chinese aliases fell back to
   `HarmonyOS Sans Digit`. The font policy now maps the Chinese serif/sans aliases to the
   corresponding Noto CJK families.
2. The game reads GDI text through a 16-bit bitmap mask and discards gray antialiased pixels.
   Merely enumerating a font or finding a CJK glyph was insufficient. In the recorded fixture,
   501 nonzero antialiased pixels became one accepted pixel after the game's threshold, while
   all 318 monochrome pixels survived.

The Wine font patch provides an opt-in `WINEHUA_MONO_TEXT=1` path for default-quality text.
Automatic selection is restricted to the identified game executable and directory. Explicit font
quality and other programs retain their behavior; an explicit override can disable the workaround.

Generated registry data uses real Unicode characters and BOM-marked UTF-16, not JSON-style
literal escapes. The relevant `wine.inf` data uses UTF-8 with a BOM. The startup policy waits for
the import process to finish before recording its migration marker.

## 3. DirectDraw white screens and source-coordinate clipping

Window clipper rectangles can extend outside the primary surface. Passing these bounds through
unchanged caused `DDERR_INVALIDRECT` and discarded the blit. The compatibility patch clips to the
destination surface while retaining the original destination rectangle for source-coordinate
mapping, avoiding clipping-induced offsets or stretching.

The device regression covers negative origins, right/bottom overflow, scaled source mapping,
fully invisible rectangles and pixel readback. Both x86 and x64 improved from **7 failures out of
37 checks** to **37/37 passing** in that historical verification.

## 4. Correct Guest/Host transfer visibility and GL presentation routing

- An independent Guest SHM shadow is not automatically `HOST_COHERENT` with the Host mapping.
  The qualified Zink transfer profile flushes/invalidate ranges at the actual transfer boundary.
- Read mappings invalidate before reading. Write mappings flush after writing; they must not
  invalidate first and copy stale Host data over new Guest data. This distinction corrected the
  earlier stretched-triangle corruption.
- Texture transfers preserve full row/layer spans; buffer readback respects real offsets.
- A GL game using WineD3D → Zink still follows the Wine GL drawable/readback path. Selecting
  Vulkan internally does not make it emit WineHua's private Vulkan-present messages.
- Do not switch the Host to a Vulkan target merely because the game uses Zink. Detect a GL frame
  arriving on the wrong target rather than silently dropping it into a black window.

The independent GL probe includes instancing, nonzero offsets, indirect draws, buffer textures,
FBO/stencil/R32F paths and game-like layouts. It is a correctness probe, not a substitute for the
full game's workload or a performance benchmark.

## 5. Renderer cleanup, shared descriptors and bounded caches

- Explicit buffer destruction and implicit device teardown now share bookkeeping cleanup.
  Detach buffer wrappers from intrusive bound-memory lists before freeing them, and unmap memory
  before calling the driver's memory-free operation. This addresses the recorded teardown UAF.
- Close the sender's descriptor after a successful `SCM_RIGHTS` send. The receiver obtains its own
  descriptor even when the sender and receiver are threads in one process; retaining the sender
  descriptor leaked resource references.
- Bound the qualified profile's **unused** BO cache to 64 MiB rather than deriving a large budget
  from every advertised Vulkan heap. Do not free active resources to meet that budget.
- Reuse at most one eligible idle readback staging resource per context, subject to format,
  capacity, reference-count and GPU-completion checks. Preserve the original synchronization.
- Notify the Venus ring after publishing submissions carrying synchronous replies. Ordinary
  asynchronous submission coalescing, fences and memory barriers remain in place.

The cleanup defect and descriptor leak have separate tests. A 64 MiB cache budget alone did
**not** eliminate the observed longer-running game SIGKILL/low-memory cases. RSS, Swap and
reserved virtual address space must not be conflated, and memory-pressure exits must not be
reported as universally solved by the teardown fix.

## 6. Desktop first frames, fixed output and foreground recovery

- Establish the session's logical-output policy before starting the server. A preference changed
  during startup applies to the next session, not half of the current one.
- Fixed-resolution mode suppresses physical XComponent feedback and therefore needs an explicit
  initial configure source. Send it outside the manager lock before publishing the desktop root.
- A root without pixels must not block real visible child buffers forever. Compose them on the
  compositor's own opaque base, without forging Guest root pixels, dimensions or `HasFrame()`.
- Include the base source in scene signatures, handle real root pixels arriving later, preserve
  damage boundaries, and clear stale content once when the last child disappears.
- Request a cached repaint for foreground restoration and same-size surface callbacks. A consumed
  frame followed by failed swap still needs a retry. Only successful presentation acknowledges
  the captured repaint request; a newer request cannot be cleared by an older swap.
- Pause work while hidden and skip repeated static frames or uninitialized empty textures.
  Keep diagnostic pixel scans/frame logs behind the trace switch.
- Use a 16 ms bounded producer-buffer wait and 250 ms recovery throttle for failed producer
  surfaces; retain the consumer, context and shared objects where possible.

Historical device coverage includes fixed/automatic output, preference changes during startup,
same-size restoration, bufferless roots, actual GDI menu input and a 1200-frame final-pixel check.
Removing a SurfaceQueue error alone did not prove that every ordinary VirGL game image was fixed.

## 7. Process lifetime and game-launch consistency

- Keep locks, allocation, logging and NAPI calls out of the `SIGCHLD` handler. Publish a lock-free
  notification and reap/report exit status from the normal monitoring thread.
- Bound the OHOS desktop-driver readiness `WM_NULL` handshake to 5000 ms with
  `SMTO_ABORTIFHUNG`, then continue the normal driver lookup/fallback. This is a bounded
  readiness path, not a diagnosis of every startup hang.
- Disable irrelevant `winemenubuilder.exe` integration on phones while preserving explicit
  DLL overrides. Scope the disk-dynacache workaround to the qualified program; keep runtime
  dynamic translation enabled and do not delete user caches.
- Decouple monochrome-text compatibility from `WINE_D3D_CONFIG=csmt=0`. Preserve the game's
  required text mode without silently disabling WineD3D's command-stream thread. Keep explicit
  user scheduling overrides and the diagnostic synchronous-GDI fallback.
- Keep the default DXVK profiles and explicit D3D9 selection unambiguous. A DirectDraw game is
  not converted into a D3D9 game by selecting a DXVK D3D9 override.

The recorded Explorer/game-library comparison used the same Wine payload, 1024×768 output and
64 matching drag gestures. Median displayed FPS was **65.484 / 64.524** over roughly 28 seconds.
This supports short-run launch parity; it does not establish a precise speedup or power reduction.

## 8. Virtual keyboard, pointer modes, stationary holds and modal focus

- Reconstruct IME passthrough `lParam` with `MAKELPARAM(1, vsc)` and retain the correct
  `WM_IME_KEYDOWN`/`WM_IME_KEYUP` distinction. Correct VK values alone do not prove SDL input.
- Give virtual single-key taps a 100 ms minimum hold, without delaying an already-long hold.
  Cancel pending releases on teardown and before reusing a control.
- Track finger ownership and shared-key reference counts. Another finger's Up must not release
  the held key; Cancel/hide/remove/edit transitions must not leave keys held.
- Preserve valid child keyboard focus, route visible modals ahead of their disabled owners, and
  recover input after the modal surface is destroyed.
- Route virtual left/middle/right buttons through pointer-button events, with paired release to
  the captured target. Keep the visible cursor overlay out of hit testing and update it from input
  events rather than idle polling.
- Add a persisted panel button for relative-to-finger and follow-finger desktop mouse movement,
  shared with the existing touchpad setting and supported by both desktop hosts.
- Preserve the cursor across relative finger lifts/mode changes and use the actual surface bounds.
  Mode changes release the old gesture and require a fresh Down; Cancel/hide/teardown clear timers.
- Give direct touch an immediate left-button Down and a paired Up, even when held stationary.
  Explicit left/middle/right panel buttons hold their own button without requiring a Move event.
- In relative mode, a stationary touch starts a left hold after 500 ms and the second tap starts
  it immediately. Keep cursor positioning and two-finger scrolling; remove long-press-to-right-click.

The [mouse-mode follow-up](../debugging/mouse-modes-en.md) records the fresh signed v18 build,
in-place installation and eight device checks from October 30. Guest Windows messages confirmed
stationary direct and left/middle/right holds, cursor-preserving switching, relative motion without
clicking and follow-finger placement. These controlled checks used the verified v17 Wine payload;
they are not a full clean runtime rebuild or complete game/multi-device acceptance.

The IME field regression has **60 checks**. Controlled Guest fixtures also cover middle/right
button pairs, Enter closing a modal, and subsequent keyboard recovery. Real Darkest Dungeon Esc
menu response was verified. Independent visual acceptance of every skill key 1–5, all game error
dialogs, all IME composition flows and all devices remains incomplete.

## 9. Phone integration, runtime components and the native game library

- Keep the phone's management page foreground until the player explicitly enters the desktop;
  background readiness must not wait for an XComponent that has not been opened.
- Share persistent phone output/quality settings and apply session snapshots consistently.
  Lower-resolution quality modes are user settings, not evidence of a same-quality optimization.
- Package the qualified Zink runtime as a separate managed component, validate its manifest and
  resource availability, and preserve explicit renderer overrides and fallback behavior.
- Limit automatic program profiles to verified program/device/runtime combinations. In particular,
  Darkest Dungeon acceptance uses `/_windows/win64/Darkest.exe`, not a different Steam/non-Steam
  or 32-bit executable.
- Provide a standalone native game library with recursive discovery, loop-safe path handling,
  main-executable selection and per-program launch settings. Keep helper/setup/upload/save tools
  available for explicit selection without choosing them as the automatic game entry.
- Verify directory identity before aliasing a downloaded game to `C:\games`; the observed
  `C:\games` and `Z:\games` directories were different.
- Use the currently foreground library Ability context to request the desktop, avoiding the
  platform's background-context 201 rejection, and reuse the existing readiness contract.

The product/frontend changes remain a separate review group because of the project's
`master`/`main-ui` integration policy.

## 10. Reproducible build, packaging and diagnostics

- Persist each Wine/Mesa/VirGL fix as a patch and wire it into the appropriate build. Force the
  direction of reverse patch probes so an unapplied patch cannot be mistaken for an applied one.
- Rebuild stale or missing critical Wine outputs; treat missing required `.so` artifacts as errors.
  Keep optional dependency modes explicit rather than silently accepting a mismatched cache.
- Separate native build tools from target pkg-config/sysroot metadata, support versioned-library
  naming differences and keep Windows executable wrappers/path quoting consistent.
- Provide the Linux SDK overlay and checked ELF relocation tools without changing segment layout,
  silently truncating string tables, or modifying a file when a dependency cannot be resolved.
- Package an explicitly selected runtime, verify ZIP members/manifests/hashes, and restore the
  original working rawfiles in `finally`. HAP contents, device runtime and build output are distinct.
- Transfer diagnostic EXEs with absolute source paths and verify the returned hash and this run's
  start/end markers. A copied directory or an old `failures=0` file is not a successful new test.
- Keep code, evidence summaries and selected screenshots in the review; exclude private signing
  configuration, credentials, game saves, runtime binaries, local device identifiers and branding
  replacements. The submodule gitlinks remain unchanged.

## Experiments deliberately not reinstated

The draft does not restore the forced GL→Vulkan SurfaceQueue route, per-frame PBO experiment,
standalone readback-merge experiment, ineffective partial-texture upload, invalid glthread
configuration, global coherent-shadow refresh, temporary pixel probes or unverified aggressive
CPU/texture/AA settings. Their useful diagnostic conclusions are retained in the history, while
their withdrawn code/defaults remain out of the selected implementation.

Direct-to-SHM readback is included as an **opt-in, default-off** path with state/slot ownership
and fallback tests. It removes a Wine-side copy under eligible conditions; GPU→CPU readback
still exists. It is not presented as GPU zero-copy or as a measured game/power improvement.

## Validation and remaining gates

See [the validation matrix](../debugging/compatibility-validation-en.md) for current reruns and
historical evidence boundaries, and [the engineering history](../debugging/compatibility-history-en.md)
for the failed hypotheses and important lessons.

- Historical device evidence includes DirectDraw 37/37 on both architectures, qualified Zink GL
  pixel checks, 1200-frame and simplified 6000-frame probes, desktop recovery, launch parity,
  controlled modal/button tests, and restored real-game Esc response.
- The v17 installation record verified the two updated `imm32.dll` architectures in both managed
  runtime and prefix locations, the matching runtime manifest, and engine `state:ready`.
- The subsequent v18 input candidate built and installed successfully with that same Wine payload;
  all eight isolated mouse-mode/hold checks passed against actual Guest button messages.
- These records came from different stages and configurations. They are not one clean run of
  the final rollup branch, and a simplified GL soak is not long-run real-game stability proof.
- A clean full native/Wine/Mesa/HAP build, complete device core/wine-vulkan/dxvk/d3d12 gates,
  multiple devices, full IME composition and longer real-game runs remain merge gates unless
  the validation matrix explicitly records a new run.

No user environment reset or game-data operation is part of preparing this PR. The earlier
Chinese IME-only draft has been closed in favor of this English historical integration review.
