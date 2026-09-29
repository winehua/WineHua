#!/usr/bin/env python3
"""Capture real foreground frames at Direct desktop fixture phases.

Start before `smoke.py run --suite direct-desktop-scene`, using the same
run-id and renderer. Fixture JSON covers Win32 operations; these screenshots
and bounded App GPU logs are the independent display evidence.
"""
import argparse
import json
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from automation.smoke import hdc_sandbox_shell, hdc_shell, resolve_hdc, snapshot_frame


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run-id", required=True)
    parser.add_argument("--device", required=True)
    parser.add_argument("--archive", type=Path, required=True)
    parser.add_argument("--renderer", choices=("egl", "vulkan"), default="vulkan")
    parser.add_argument("--timeout", type=int, default=240)
    parser.add_argument("--settle-seconds", type=float, default=0.5,
                        help="Wait for the visible window transition before each capture")
    parser.add_argument("--probe-input", action="store_true",
                        help="Check real taps on the 1280x800 fixture desktop at 2560x1600 output; requires WINEHUA_D3D_INPUT_TRACE=1")
    args = parser.parse_args()
    if not args.run_id.replace("-", "").replace("_", "").isalnum():
        parser.error("run-id must contain only letters, digits, underscores and hyphens")
    hdc = resolve_hdc()
    result = f"data/storage/el2/base/files/.wine/drive_c/smoke/results/{args.run_id}/direct-desktop-scene-x64.json"
    deadline = time.monotonic() + args.timeout
    captured, seen, restored, input_checks = [], set(), False, []
    while time.monotonic() < deadline:
        rc, output = hdc_sandbox_shell(hdc, args.device, f"tail -1 {result}.steps.log", timeout=10)
        fields = output.strip().split()
        if rc == 0 and len(fields) == 2 and fields[0].isdigit():
            if not restored:
                hdc_shell(hdc, args.device, "aa start -a EntryAbility -b app.hackeris.winehua "
                          "--ps winehua.direct_ncp_session 1 --ps winehua.desktopMode virtual "
                          f"--ps winehua.desktop_renderer {args.renderer}")
                restored = True
            phase = fields[1]
            if phase not in seen:
                seen.add(phase)
                observed_at = time.monotonic()
                time.sleep(max(0, args.settle_seconds))
                if args.probe_input and phase in ("fullscreen", "windowed"):
                    # The windowed phase lasts only three seconds. Inject before
                    # screenCap/file transfer, which can consume that interval.
                    point = (1280, 800) if phase == "fullscreen" else (948, 710)
                    _, latest = hdc_sandbox_shell(hdc, args.device, f"tail -1 {result}.steps.log", timeout=10)
                    check = {"stage": phase, "tap": point, "status": "FAIL",
                             "phaseBeforeTap": latest.strip()}
                    if latest.strip().split() == fields:
                        rc, output = hdc_shell(hdc, args.device, f"uitest uiInput click {point[0]} {point[1]}")
                        check["commandExit"] = rc
                        input_deadline = time.monotonic() + 1.0
                        while True:
                            _, trace = hdc_sandbox_shell(hdc, args.device, f"tail -1 {result}.a.json.input.log", timeout=10)
                            values = trace.strip().split()
                            if len(values) == 5 and all(v.lstrip("-").isdigit() for v in values):
                                tick, x, y, width, height = map(int, values)
                                expected = (640, 400) if phase == "fullscreen" else (350, 225)
                                extent = (1280, 800) if phase == "fullscreen" else (700, 450)
                                check.update({"receivedTickMs": tick, "received": [x, y],
                                              "clientExtent": [width, height], "expected": expected})
                                if tick >= int(fields[0]):
                                    if rc == 0 and (width, height) == extent and abs(x - expected[0]) <= 3 and abs(y - expected[1]) <= 3:
                                        check["status"] = "PASS"
                                    break
                            if time.monotonic() >= input_deadline:
                                check["reason"] = "no fresh WM_LBUTTONUP before deadline"
                                break
                            time.sleep(0.05)
                    else:
                        check["reason"] = "fixture phase changed before injection"
                    check["elapsedSinceObservedMs"] = round((time.monotonic() - observed_at) * 1000)
                    input_checks.append(check)
                    print("input", check, flush=True)
                _, before = hdc_sandbox_shell(hdc, args.device, f"tail -1 {result}.steps.log", timeout=10)
                path = snapshot_frame(hdc, args.device, args.archive, f"scene-{phase}")
                _, after = hdc_sandbox_shell(hdc, args.device, f"tail -1 {result}.steps.log", timeout=10)
                captured.append({"stage": phase, "deviceTickMs": fields[0], "image": str(path),
                                 "phaseBeforeCapture": before.strip(), "phaseAfterCapture": after.strip(),
                                 "phaseStable": before.strip().split() == fields and after.strip().split() == fields})
                print(phase, path, flush=True)
        _, output = hdc_sandbox_shell(hdc, args.device, f"cat {result}", timeout=10)
        if output.strip().startswith("{"):
            break
        time.sleep(0.4)
    args.archive.mkdir(parents=True, exist_ok=True)
    (args.archive / "scene-captures.json").write_text(json.dumps(captured, indent=2) + "\n", encoding="utf-8")
    if args.probe_input:
        (args.archive / "input-checks.json").write_text(json.dumps(input_checks, indent=2) + "\n", encoding="utf-8")
        if len(input_checks) != 2 or any(check["status"] != "PASS" for check in input_checks):
            return 1
    required = {"initial-render", "overlap", "move", "gdi-cover", "alpha-cover",
                "minimize", "restore", "destroy-b", "fullscreen", "windowed", "desktop-idle"}
    if required - seen or any(item["image"] == "None" or not item["phaseStable"] for item in captured):
        print(f"Incomplete visual evidence; missing phases: {sorted(required - seen)}", file=sys.stderr)
        print(f"Unstable captures: {[item['stage'] for item in captured if not item['phaseStable']]}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
