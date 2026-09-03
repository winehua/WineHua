#!/usr/bin/env python3
"""Keep host and Wine WHGP v2 protocol headers in lockstep."""
from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HOST = ROOT / "entry/src/main/cpp/input/controller/gamepad_ipc_protocol.h"
WINE = ROOT / "thirdparty/wine/dlls/winebus.sys/winehua_gamepad_protocol.h"
BUS = ROOT / "thirdparty/wine/dlls/winebus.sys/bus_ohos.c"
BRIDGE = ROOT / "entry/src/main/cpp/input/controller/gamepad_bridge.cpp"
BEGIN = "/* >>> WHGP_PROTOCOL_BEGIN */"
END = "/* <<< WHGP_PROTOCOL_END */"

REQUIRED_SNIPPETS = (
    "#define WHGP_VERSION 2",
    "struct whgp_state_v2",
    "ly/ry: down negative, up positive",
    "hat_y: down negative, up positive",
    "whgp_stick_y_to_hid",
    "whgp_hat_y_to_hid",
    "WHGP_MSG_STATE 1",
    "WHGP_MSG_RUMBLE 3",
)


def extract_block(path: Path) -> str:
    text = path.read_text(encoding="utf-8")
    start = text.find(BEGIN)
    end = text.find(END)
    if start < 0 or end < 0 or end <= start:
        raise SystemExit(f"{path}: missing WHGP protocol markers")
    return text[start : end + len(END)]


def main() -> int:
    host = extract_block(HOST)
    wine = extract_block(WINE)
    if host != wine:
        print("WHGP protocol blocks differ between host and wine headers", file=sys.stderr)
        return 1
    for snippet in REQUIRED_SNIPPETS:
        if snippet not in host:
            print(f"WHGP protocol block missing {snippet!r}", file=sys.stderr)
            return 1

    bus = BUS.read_text(encoding="utf-8")
    if "whgp_stick_y_to_hid(body->ly)" not in bus:
        print("bus_ohos.c must pass canonical ly through whgp_stick_y_to_hid", file=sys.stderr)
        return 1
    if "ly = -(LONG)body->ly" in bus or "ly = -body->ly" in bus:
        print("bus_ohos.c must not invert canonical analog Y", file=sys.stderr)
        return 1
    if "WHGP protocol mismatch: peer=" not in bus:
        print("bus_ohos.c must log WHGP protocol mismatch", file=sys.stderr)
        return 1
    if "whgp_hat_y_to_hid(body->hat_y)" not in bus:
        print("bus_ohos.c must convert hat Y with whgp_hat_y_to_hid", file=sys.stderr)
        return 1

    bridge = BRIDGE.read_text(encoding="utf-8")
    if "whgp_state_v2" not in bridge:
        print("gamepad_bridge.cpp must publish whgp_state_v2", file=sys.stderr)
        return 1
    if "WHGP protocol mismatch: peer=" not in bridge:
        print("gamepad_bridge.cpp must log WHGP protocol mismatch", file=sys.stderr)
        return 1
    if "body.ly = -" in bridge or "body.ly = -state" in bridge:
        print("gamepad_bridge.cpp must not invert ly", file=sys.stderr)
        return 1

    print("verify_whgp_protocol: host/wine WHGP v2 blocks match")
    return 0


if __name__ == "__main__":
    sys.exit(main())
