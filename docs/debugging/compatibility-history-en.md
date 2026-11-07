# Compatibility engineering history and lessons learned

Prepared on October 30, 2026. This records the significant work in this workspace through
October 29, 2026, relative to official `master` commit `3eecbd7`. It is not a claim to have
implemented every pre-existing WineHua customization or fixed every historical crash.

The [integration PR](../pr/compatibility-history-rollup.md) describes the included code.
The [validation matrix](compatibility-validation-en.md) separates fresh host checks from
historical device observations and incomplete acceptance.

## Graphics creation is a chain of independent contracts

A valid native window, usable EGL configuration, created EGL context, successful make-current,
supported Guest desktop-GL context and a non-null extension entry are different prerequisites.
Checking only the last operation obscures where object creation actually failed.

The Host renderer now validates configuration selection and context creation, stops after failed
make-current, and does not destroy the old renderer before validating a replacement native window.
Initialization failure keeps native-window destruction under one owner. Late callbacks carry
surface identity, preventing an old handle's destruction from taking down a new renderer.

Guest desktop OpenGL 3.2 is not equivalent to Host GLES support. An earlier game context request
failed with `EGL_BAD_MATCH (0x3009)` on the original VirGL route; a basic compatibility context
working did not establish that the game's required core context was supported. The qualified
Zink path addresses that program/driver combination rather than forcing a universal context flag.

A separate failure involved `glVertexAttribDivisor` returning no usable entry even though
`GL_ARB_instanced_arrays` supplied the compatible ARB function. The fallback is extension-gated.
Another failure involved a shader advertising an unused `ShaderLayer` capability: the native
Maleoon graphics-pipeline probe failed until that redundant declaration was removed. Real Layer
usage must remain intact. These issues must not be collapsed into a single GL-handle explanation.

## Fonts can exist while text remains invisible

Two independent text failures were found:

- Chinese family aliases selected a digit-only fallback, even though English family names could
  resolve to CJK-capable fonts. Fix the aliases and verify actual glyph selection.
- The legacy game's 16-bit text mask rejected gray antialiased pixels. A CJK glyph existing was
  insufficient: the recorded antialiased mask retained only 1 of 501 nonzero pixels after filtering,
  whereas the monochrome mask retained all 318 pixels.

The monochrome workaround is scoped to the known executable/directory and default-quality text.
It is not a global antialiasing change. An explicit opt-out remains available.
Registry generation must preserve real Unicode and the expected UTF-16 BOM; `wine.inf` encoding
is checked independently. Import completion, not spawn success, controls the migration marker.

Text compatibility originally also forced `csmt=0`. That unrelated scheduling override explained
why an Explorer launch and a Unix-launched game library path could run differently. Keep font
compatibility and WineD3D scheduling independent and preserve explicit user environment settings.

## A white or black window does not identify a rendering layer

| Evidence | Incorrect shortcut | Correct distinction |
| --- | --- | --- |
| DirectDraw blit fails for out-of-bounds clip rectangles | Change the entire GPU backend | Clip destination bounds while retaining the original source mapping |
| EGL initialized, root still has no pixels | Declare GPU initialization fixed | Trace root recognition, configure, attach and commit separately |
| Children have real buffers but root is bufferless | Wait indefinitely for background pixels | Compose actual child content on the compositor's own base |
| Surface returns at the same dimensions | Skip repaint because geometry is unchanged | A new or restored consumer can still need the cached image |
| New frame consumed, swap failed, no later commit | Wait for an application update | Retain the repaint request until presentation succeeds |
| GL frame reaches a private Vulkan target | Assume Zink implies the private present protocol | Preserve the actual GL drawable/readback route |
| Producer temporarily has no free buffers | Rebuild the whole stack every frame | Use bounded waits and throttled producer-surface recovery |
| Framebuffer samples are black | Blame only final alpha composition | Investigate content generation and the earlier transfer/present boundary |

Fixed logical output and physical XComponent dimensions have different authority. Suppressing
physical resize feedback requires an independent first-configure path. A compositor fallback must
not forge Guest root pixels or change `HasFrame()`. Real-root arrival, partial damage and removal
of the final child each require explicit cache/base handling.

## Guest/Host visibility cannot be inferred from a coherent allocation flag

Independent Host and Guest shadow mappings need explicit visibility at the transfer boundary.
The first transfer workaround still produced stretched triangles because write maps could
invalidate and restore stale Host data before the Guest wrote its intended contents.
The corrected directionality invalidates reads and flushes writes after completion.
Row/layer spans, buffer offsets, indirect-command offsets and instance strides are part of the
contract; testing only small tightly packed uploads misses the game's real layouts.

Do not delete barriers or fences to obtain a superficially faster sample. Qualified staging reuse
requires compatible format/capacity, no external users and GPU completion; the cache remains bounded.

## Resource lifetime bugs and memory pressure are not interchangeable

Implicit device teardown bypassed the explicit buffer-destroy path. A freed wrapper remained on
an intrusive bound-memory list and was dereferenced during memory cleanup. The fix shares buffer
bookkeeping cleanup between explicit and implicit destruction, and unmaps before driver free.

A separate resource-reply leak retained sender descriptors after `SCM_RIGHTS`. The receiver has
its own descriptor even between threads of one process; the queued message preserves the backing
object until receive. Tests use actual sockets and descriptor counts rather than a mocked ownership rule.

The qualified 64 MiB unused-BO budget reduces avoidable cache amplification, but an observed game
still received SIGKILL in later low-memory runs. The cleanup fix is not evidence that all longer-run
exits are solved. VmSize includes reserved address space; RSS, Swap and duplicated descriptor
sizes need separate accounting. Some observed exits were explicitly user-initiated.

## Process monitoring must remain async-signal-safe

Logging, allocation, NAPI and process-table mutex operations do not belong in `SIGCHLD` handlers.
A signal can interrupt the thread already holding the same lock, freezing graphics/UI while audio
continues. The handler now publishes a lock-free notification; the ordinary monitor reaps children
and publishes their actual exit status. The regression deliberately exercises child exits while
the process-table lock is held.

A bounded OHOS desktop-driver readiness handshake avoids an infinite synchronous wait, but is
still a readiness policy, not proof that every pre-render startup stall has the same root cause.

## An input log is not an application acceptance test

The IME passthrough bug preserved VK values but placed the original message's high-word field
directly in the full `lParam`. SDL could not read the correct scan code or state bits.
Rebuild the complete parameter and verify the message type, scan, extended and release/repeat
flags. A longer tap cannot repair a malformed message.

Virtual controls also need real timing and ownership: minimum short-tap duration, immediate
release after a long hold, canceled timers, finger identity, shared-key counts, captured targets,
and modal focus recovery. `BTN_*` mouse buttons are not keyboard scan codes.
Controlled Guest windows and real game dialogs remain different acceptance surfaces.

## Preserve program identity and final launch environment

The same-looking path need not be the same directory. The observed `C:\games` and `Z:\games`
contents were different. Keep explicit working directories and validate aliases by directory identity.
Do not automatically select setup, workshop-upload, translation or save-migration tools as the game.

The Darkest Dungeon acceptance target is the specified `_windows/win64/Darkest.exe`. Results
from another 32-bit or non-Steam executable do not substitute for it.
Compare the final environment of the actual Windows game, not just the launcher's requested values.
Program-specific profile workarounds must remain qualified and explicit overrides must win.

On phones, background Ability contexts can be rejected with platform error 201. A later window-ready
timeout is a consequence, not the first failure. Use the foreground library context and keep display
readiness separate from process-start success. Returning to the management page must not immediately
force the user back into the game.

## Build and test facilities need their own acceptance

- A Wine source change does not enter an installed app merely by rebuilding a HAP. Rebuild the
  relevant architecture(s), assemble the payload and verify ZIP members, manifest and installed files.
- Reverse patch probes need a forced direction. Otherwise GNU patch can ignore `--reverse` and
  return success for an unapplied patch. Check first application and repeated application.
- Preserve executable shell line endings. A Windows checkout can inflate a tiny change into a
  whole-file diff or make an otherwise valid POSIX script fail before reaching the code under test.
- Separate native tool metadata from target pkg-config/sysroot metadata. Missing required runtime
  artifacts and changed optional-dependency modes must invalidate cached success.
- ELF relocation must preserve segment/table constraints and must not partly modify a file when a
  dependency cannot be resolved. Keep the process reproducible rather than recording a one-off edit.
- A submodule-shaped directory without independent Git metadata can make `git -C` fall back to the
  parent repository and produce a misleading empty diff. Keep replayable patches or valid pushed gitlinks.
- HDC relative paths can copy a directory instead of the intended EXE. Use absolute sources, explicit
  destinations and round-trip hashes. Old result files and stale screenshot coordinates are not new evidence.
- Use each fixture's declared include paths and build flags. A missing test-protocol header caused by
  an incomplete invocation is a harness setup error, not a DirectDraw runtime failure.
- Avoid shared output filenames between concurrent probes, and keep credentials and unfiltered
  environment logs out of public evidence.

## Keep successful policies separate from withdrawn experiments

The private-Vulkan target forced onto a GL game, per-frame PBO, isolated readback merge, ineffective
partial texture upload, global coherent refresh and invalid glthread experiments were withdrawn.
Aggressive CPU parameters, lower AA/texture settings and a changed storage location did not establish
the claimed stability fix and were not retained as release defaults.

Direct-to-SHM readback remains default-off and opt-in, with state restoration, slot ownership and
fallback checks. It removes a Wine-side copy, not GPU→CPU readback. A static byte count is not a
measured DDR, GPU utilization, power or game-FPS improvement.

The Mewgenics home-scene reference remained around 9 FPS. Higher menu rates and lower-resolution
quality settings are not evidence of a same-scene, same-quality performance gain. Displayed frames
must be distinguished from client commits and cached Host repaints, with samples deduplicated by
update sequence. A simplified GL soak is not complete real-game stability coverage.

## Stationary input needs a real button lifetime

The original general direct-touch gesture postponed left Down until movement crossed a threshold
and converted a stationary long touch to right-click on Up. That cannot represent a stationary
left hold to a polling game, and it is unnecessary when explicit middle/right buttons exist.
Direct desktop touch now pairs Down/Up immediately. Each panel mouse button retains its own
identity and lifetime; motion is not a condition for holding it.

The panel now switches the existing persisted mouse mode. Shared relative gestures preserve the
cursor and surface-local bounds; stationary and double-tap holds do not require a Move event.
Mode changes and lifecycle transitions must release the original owner and cancel deferred work
before another mode can consume the gesture. Fresh controlled device checks and their limits are
recorded in [the mouse-mode follow-up](mouse-modes-en.md).

## Runtime upgrades must not silently erase user data

The existing engine-update/reset UI can clear the Wine environment. Replacing a HAP is not consent
to factory-reset a prefix. The recorded v17 deployment used a known old payload, an exact two-DLL
ZIP delta, backups, four installed-DLL hash checks and a matching manifest before restart.
That verified manual migration is not a general automatic upgrader; unknown payloads must not be
declared upgraded by changing a hash alone.

Keep signing files, game data, device identifiers and runtime binaries outside public commits.
Record which assertions are fresh, historical, skipped, withdrawn or still unverified, even when
the user reports that the visible symptom now appears fixed.
