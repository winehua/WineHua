// ArkUI MouseButton → Linux evdev button code 映射
// 参考: Linux include/uapi/linux/input-event-codes.h
//
// ArkTS MouseButton (全局枚举, 不需要 import):
//   None=0, Left=1, Right=2, Middle=4, Back=8, Forward=16
//
// Linux evdev button codes:
//   BTN_LEFT=0x110, BTN_RIGHT=0x111, BTN_MIDDLE=0x112,
//   BTN_BACK=0x116, BTN_FORWARD=0x115

const BTN_LEFT    = 0x110
const BTN_RIGHT   = 0x111
const BTN_MIDDLE  = 0x112
const BTN_BACK    = 0x116
const BTN_FORWARD = 0x115

// ArkUI MouseButton → evdev button code
export function mapMouseButton(arkUIButton: number): number {
  switch (arkUIButton) {
    case 1:  return BTN_LEFT     // MouseButton.Left
    case 2:  return BTN_RIGHT    // MouseButton.Right
    case 4:  return BTN_MIDDLE   // MouseButton.Middle
    case 8:  return BTN_BACK     // MouseButton.Back
    case 16: return BTN_FORWARD  // MouseButton.Forward
    default: return 0            // unknown
  }
}
