#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* >>> WHGP_PROTOCOL_BEGIN */
/* WineHua Gamepad Protocol (WHGP) v2 — host <-> winebus bus_ohos.
 * Canonical Controller Space on the wire (source-independent):
 *   lx/rx: left negative, right positive
 *   ly/ry: down negative, up positive
 *   hat_x: left negative, right positive
 *   hat_y: down negative, up positive
 *   lt/rt: 0..32767
 * Host: entry/src/main/cpp/input/controller/gamepad_ipc_protocol.h
 * Wine: thirdparty/wine/dlls/winebus.sys/winehua_gamepad_protocol.h
 * Keep this marked protocol block byte-identical in both files.
 */

#define WHGP_MAGIC 0x50474857u /* 'WHGP' LE */
#define WHGP_VERSION 2
#define WHGP_MSG_STATE 1
#define WHGP_MSG_RESET 2
#define WHGP_MSG_RUMBLE 3

#pragma pack(push, 1)
struct whgp_header {
    uint32_t magic;
    uint16_t version;
    uint16_t msg_type;
    uint32_t slot;
    uint32_t payload_size;
};

struct whgp_state_v2 {
    uint32_t buttons; /* bit0=A ... bit9=R3, bit10=Guide */
    int16_t lx; /* right + */
    int16_t ly; /* up + */
    int16_t rx;
    int16_t ry;
    uint16_t lt; /* 0..32767 */
    uint16_t rt;
    int8_t hat_x; /* -1/0/+1, right + */
    int8_t hat_y; /* -1/0/+1, up + */
    uint8_t reserved[2];
};

/* Wine → host. duration_ms 0 means "until next update". */
struct whgp_rumble_v1 {
    uint16_t low;  /* left / rumble motor, 0..65535 */
    uint16_t high; /* right / buzz motor, 0..65535 */
    uint32_t duration_ms;
};
#pragma pack(pop)

static inline int whgp_version_matches(uint16_t version)
{
    return version == (uint16_t)WHGP_VERSION;
}

/* Stick Y is already canonical (+Y=Up), matching XInput/GIP. Do not invert. */
static inline int32_t whgp_stick_y_to_hid(int16_t y)
{
    return (int32_t)y;
}

/* HID hatswitch helper is +Y=Down; WHGP hat_y is canonical +Y=Up. */
static inline int32_t whgp_hat_y_to_hid(int8_t y)
{
    return -(int32_t)y;
}
/* <<< WHGP_PROTOCOL_END */

#ifdef __cplusplus
}
#endif
