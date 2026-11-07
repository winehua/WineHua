# Desktop mouse modes and stationary button holds

## User behavior

Open the game touch-key panel and use its mouse-mode button to switch between:

- **Relative to the finger**: sliding changes the current cursor position by the finger's movement.
  Lifting and touching elsewhere preserves the cursor position. Cursor speed applies to these deltas.
- **Follow the finger**: touching places the cursor at that point and immediately holds the left
  button. Releasing the finger releases that same button. Movement is not required for a hold.

The mode is shared with the existing touchpad setting and persisted in `input_settings`.
The panel shows the current mode. It exposes the switch on desktop hosts; individual multiwindow
Wine-window overlays retain their existing input path.

Left, middle and right panel buttons each send their own button-down immediately and keep it held
until Up or Cancel. A stationary hold is not converted into a different button or a delayed click.
The pointer remains at the game cursor rather than jumping to the panel button.

Relative cursor gestures preserve tap-to-left-click and two-finger scrolling. A second tap starts a
left-button hold immediately; a stationary first touch starts a left-button hold after 500 ms even
without a Move event. Moving to position the cursor cancels that stationary-hold timer. Right and
middle clicks use their explicit panel buttons; a screen long press no longer synthesizes right-click.

## Implementation and regression boundaries

`DesktopTouchpad` shares the relative gesture logic between `DesktopLayer` and `DesktopWindow`.
Its bounds come from the actual XComponent, in physical pixels, rather than the full display.
The shared cursor supplies the starting position after direct touch, physical mouse movement or a
mode change. Motion remains inside the surface, including its last valid pixel.

Mode changes release the captured old gesture, clear scroll/double-tap state and require a fresh
Down. Another finger's Up cannot release the owner. Empty Cancel, hidden surfaces and component
teardown clear timers and release to the original target. The existing virtual-key minimum hold
continues to apply only to keyboard taps; mouse-button releases are immediate.

Host checks, using Node and the installed TypeScript compiler:

```sh
node scripts/test_mouse_modes.cjs /path/to/typescript.js
node scripts/test_virtual_key_pulse.cjs /path/to/typescript.js
node scripts/test_desktop_pointer.cjs /path/to/typescript.js
node scripts/test_touch_controls.cjs /path/to/typescript.js
node scripts/test_game_touch.cjs /path/to/typescript.js
```

The mouse-mode check executes the shared controller and the actual touch/lifecycle methods from
both desktop hosts. The virtual-key check executes the actual control methods for stationary
left/middle/right Down, another finger's Up, release and empty Cancel. Settings checks reload
both mode values from mocked preferences.

## Fresh device verification on October 30, 2026

The signed v18 candidate was built from the working integration sources and installed with
`hdc install -r`. The previously verified v17 Wine payload was explicitly retained. There was no
uninstall, prefix reset, game-data removal or full native/Wine/Mesa rebuild.

- HAP SHA-256: `514fe53daf79cfc54e3f7c22fdbda631d760da8d6f2d83a68bb3290e9f8b5845`.
- Wine payload SHA-256: `f35c1e15d192fb904832b777b2f101427940ecbf9a4e16fbd3460cbbc7332aa7`.
- Native library SHA-256: `70d78214b308c99ca59f9f14bd30b7a4d233e09eae326338f68d642c2cb5bd80`.
- Installed managed-runtime manifest matched that payload.

A Guest Windows input-audit window logged actual `WM_*BUTTONDOWN`/`WM_*BUTTONUP` messages:

| Check | Fresh observed result |
| --- | --- |
| Direct stationary screen hold | Left Down/Up separated by 1509 ms |
| Left panel button, no movement | Left Down/Up separated by 1498 ms |
| Middle panel button, no movement | Middle Down/Up separated by 1508 ms |
| Right panel button, no movement | Right Down/Up separated by 1539 ms |
| Switch to relative | Cursor position unchanged |
| Relative swipe of approximately 100 by 20 pixels | Cursor moved 96 by 19 pixels; no Guest button messages |
| Relative stationary screen hold | Left Down/Up separated by 1004 ms; no right-button substitution |
| Switch back to direct | Cursor followed the tested touch coordinates |

An earlier sequence containing concurrent extra pointer events was rejected; the table reports
the subsequent isolated eight-check run. These are controlled-fixture results on one phone,
not independent real-game acceptance of every button or multi-device coverage.

During the reported black-background observation, the captured test window continued painting
and the renderer reported zero failed swaps. The surrounding bufferless desktop base was black.
This observation does not prove that every historical desktop black-screen path is fixed.
