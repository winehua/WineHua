# Canonical Controller Coordinates

Controller Hub uses one platform-independent space for every source:

- Stick X: right positive, `[-1, 1]`
- Stick Y: **up** positive, `[-1, 1]`
- Hat X: right positive, `-1 / 0 / +1`
- Hat Y: **up** positive, `-1 / 0 / +1`
- Trigger: `0..1`

## Boundaries

1. **Source adapters** convert platform raw input into canonical values.
   - OH Game Controller Kit thumbs match SDL raw polarity (up is negative): `canonicalY = -rawY`.
   - Touch overlay screen space is +Y down: `ny = -dy / radius`.
   - Hat is converted independently of analog sticks.
2. **ControllerHub** merges canonical state only. `SetStick` updates a 2D stick atomically. Owners are per stick, with last-active `activitySequence` fallback. Unchanged state is not republished.
3. **WHGP v2** copies hub fields. `ly/ry > 0` means up. Version mismatch logs `WHGP protocol mismatch: peer=N expected=2` and disconnects. There is no v1 polarity guess.
4. **winebus `bus_ohos`** maps canonical sticks to HID/XInput without analog Y inversion. Hat Y is inverted only because the HID hatswitch helper is +Y=Down. `bus_sdl` SDL-raw inversion does not apply here.

Do not compensate in `InputDispatcher` with `whgpY = -y`. Do not let Hub or winebus invert based on source.

After a HAP that contains a new `winebus.so`, restart the Wine session. Host native reloads on install; the guest unix lib does not.
