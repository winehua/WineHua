# VKD3D Readback Revalidation - 910 - 2026-08-06

## Scope

This checkpoint revalidates the VKD3D-Proton 2.6 limited-500K product path
after the persistent mapped-memory synchronization change. It is intentionally
not a game qualification or release gate by itself.

## Regression and root cause

Mesa commit `6604707` initially synchronized every persistently mapped coherent
allocation before queue submission. That included D3D12 READBACK allocations.
The Guest shadow for a readback heap still contained zeroes, so the submit-time
Guest-to-Host flush could overwrite data the GPU had produced on the Host.
The accurate smoke then reported `expected=11 actual=0`.

Mesa commit `2939cbb` records explicit CPU write intent and limits persistent
submit-time flushes to allocations with that marker. The VKD3D patch marks
mapped UPLOAD resources as CPU-written and explicitly excludes READBACK heaps.

## Artifact identity

- Main commit: `8d480cc`
- Mesa commit: `2939cbb816d3a3c5fcf3147fb8436e105d0a0ee8`
- VKD3D-Proton upstream commit: `3e5aab6fb3e18f81a71b339be4cb5cdf55140980`
- Signed HAP SHA-256: `801d1c728108908270651f2334554c9f1af7d853790e76ddb82ed658292d0a02`
- Embedded `wine-data.zip` SHA-256: `7ad07d7741035b2b76641633684d5f8f42d2e8cb264b8eaa6ad2c16c17c5d10b`
- VKD3D `d3d12.dll` SHA-256: `a3810f3cc57e9290aba2f1a172350fe06bc85da146c82de090551aee80ebba61`
- Device: `62T0225B10005882`, Maleoon 910 target

The HAP-embedded `wine-data.zip` hash matched the assembled rawfile. Guest EGL
was x86-64 and HAP `libentry.so` was AArch64.

## Fresh-install run 1

The bundle was force-stopped, fully uninstalled, installed from the artifact
above, and started through `EntryAbility`. The accurate D3D12 smoke was then
launched from the normal UI into the application-owned clean prefix.

Final result:

```json
{"status":"PASS","stage":"complete","frames":1000,"target_frames":1000,"fps":34.952,"frame_verified":true,"animation_verified":true,"buffer_copy_verified":true,"fence_verified":true,"readback_checks":2,"non_clear_pixels":52575,"first_frame_hash":"c8a34e196a37cd5a","last_frame_hash":"5d4a344924a06c50","exit_reason":"none","exit_message":0}
```

The physical display showed three colored rotating D3D12 objects and the title
`D3D12 CORE PASS; DISPLAY CHECK | 1000/1000 | 35.0 FPS`. The two readback
checks produced distinct non-zero hashes. The stderr log recorded only memory
ID `183` in persistent submit flushes. Readback memory ID `188` had zero such
flushes and was transferred Host-to-Guest through invalidate instead.

Searches of the captured stderr and Host logs found no
`vkDeviceWaitIdle`, `vkQueueWaitIdle`, global flush compensation,
`buffer_readback_mismatch`, `status=FAIL`, or device-loss marker.

## Remaining gate

This is one passing independent fresh-install run. Two more independent runs,
fresh upstream `triangle.exe` and `gears.exe` display checks, DXVK/desktop
regression, and a real DX12 game validation still remain mandatory before
pushing the submodule and main branches. A second fresh-install run reached
visible D3D12 rendering, but HDC/USB disconnected before its final artifacts
could be collected, so it is not credited.
