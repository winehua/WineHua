#!/usr/bin/env python3
"""Run bounded, interleaved Direct/Venus measurements on one explicitly chosen device.

Use --plan-only to inspect the exact commands. A benchmark PASS validates a
measurement; gains still require comparable output work and real-game evidence.
"""
import argparse
from datetime import datetime
import hashlib
import json
from pathlib import Path
import re
import statistics
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from automation.smoke import BUNDLE, hdc_shell, hdc_sandbox_shell, resolve_hdc

ROOT = Path(__file__).resolve().parents[1]
PROFILES = {
    "light": {"draws": 1, "small": 1, "fps": 0},
    "commands": {"draws": 1000, "small": 1, "fps": 0},
    "fill": {"draws": 100, "small": 0, "fps": 0},
    "fixed60": {"draws": 100, "small": 1, "fps": 60},
}


def capture_environment(hdc, device, app_pid=None):
    """Read boundary state only; no device polling or screenshots while timing."""
    captured = {"capturedAt": datetime.now().astimezone().isoformat()}
    for label, command in (
        ("thermal", "hidumper -s ThermalService -a -t"),
        ("thermalLevel", "hidumper -s ThermalService -a -l"),
        ("battery", "hidumper -s BatteryService -a -i"),
        ("display", "hidumper -s DisplayManagerService -a -a"),
    ):
        code, output = hdc_shell(hdc, device, command, timeout=10)
        if label == "display":
            # The dump begins with unrelated client names and historical events.
            # Keep current screen properties only, without inferring scan-out FPS.
            output = output.partition("---------------- Screen ID:")[2]
            keys = ("screen id", "rsscreenid", "activemodes", "refreshrate", "rotation:",
                    "screenpowerstate", "width:", "height:")
            output = "\n".join(line for line in output.splitlines()
                               if any(key in line.lower() for key in keys))
        captured[label] = {"exit": code, "output": output.strip()}
    if app_pid is not None:
        code, output = hdc_shell(hdc, device, "hilog -z200 -T testTag", timeout=10)
        lifecycle = [line for line in output.splitlines()
                     if re.search(rf"\s{int(app_pid)}\s", line)
                     and ("Ability onForeground" in line or "Ability onBackground" in line)]
        captured["entryAbilityLifecycle"] = {"pid": app_pid, "exit": code, "events": lifecycle,
                                             "backgroundObserved": any("Ability onBackground" in line for line in lifecycle)}
        code, output = hdc_shell(hdc, device, "hilog -z200 -T DWA", timeout=10)
        captured["desktopAbilityLifecycle"] = {
            "pid": app_pid, "exit": code,
            "events": [line for line in output.splitlines()
                       if re.search(rf"\s{int(app_pid)}\s", line)
                       and ("onForeground" in line or "onBackground" in line)]}
        code, output = hdc_shell(hdc, device, "aa dump -l", timeout=10)
        abilities = []
        for mission in output.split("Mission ID #"):
            if f"bundle name [{BUNDLE}]" not in mission:
                continue
            name = re.search(r"main name \[([^]]+)\]", mission)
            state = re.search(r"^\s*state #(\w+)", mission, re.MULTILINE)
            app_state = re.search(r"^\s*app state #(\w+)", mission, re.MULTILINE)
            abilities.append({"ability": name.group(1) if name else None,
                              "state": state.group(1) if state else None,
                              "appState": app_state.group(1) if app_state else None})
        captured["appAbilities"] = {"exit": code, "abilities": abilities}
    return captured


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--device", required=True)
    parser.add_argument("--archive", type=Path, required=True)
    parser.add_argument("--hap", type=Path, required=True)
    parser.add_argument("--payload", type=Path, default=ROOT / "build/smoke-performance-payload")
    parser.add_argument("--architecture", choices=("amd64", "arm64"), default="amd64")
    parser.add_argument("--profile", choices=PROFILES, default="fixed60")
    parser.add_argument("--blocks", type=int, default=3, help="Each block is Venus, Direct, Direct, Venus (default six samples each)")
    parser.add_argument("--sanity", action="store_true", help="Short validation of the measurement tool; not performance acceptance")
    parser.add_argument("--plan-only", action="store_true")
    args = parser.parse_args()
    if not 1 <= args.blocks <= 5:
        parser.error("blocks must be in 1..5")
    if args.sanity and args.profile != "light":
        parser.error("sanity runs use --profile light")
    args.hap = args.hap.resolve()
    args.payload = args.payload.resolve()
    args.archive = args.archive.resolve()
    args.archive.mkdir(parents=True, exist_ok=True)
    if not args.hap.is_file() or not (args.payload / "manifest.json").is_file():
        parser.error("HAP/payload missing; build the measurement package first")
    stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    seconds, warmup = (12, 3000) if args.sanity else (75, 15000)
    profile = PROFILES[args.profile]
    suite = f"direct-performance-{args.architecture}"
    test_id = f"d3d11-benchmark-{args.architecture}-x64"
    sequence = ["venus", "direct", "direct", "venus"] * args.blocks
    if args.sanity:
        sequence = ["venus", "direct"]
    plan = []
    for index, backend in enumerate(sequence, 1):
        run_id = f"perf-{args.architecture}-{args.profile}-{stamp}-{index}-{backend}"
        command = [sys.executable, "-X", "utf8", "automation/smoke.py", "run", "--suite", suite,
            "--prefix", "reuse", "--device", args.device, "--run-id", run_id,
            "--archive-root", str(args.archive), "--payload", str(args.payload), "--desktop-mode", "virtual",
            "--desktop-renderer", "vulkan" if backend == "direct" else "egl", "--direct-ncp-session",
            "--env", f"WINEHUA_VULKAN_BACKEND={backend}",
            "--env", f"WINEHUA_BENCHMARK_WARMUP_MS={warmup}",
            "--env", f"WINEHUA_BENCHMARK_DRAWS={profile['draws']}",
            "--env", f"WINEHUA_BENCHMARK_SMALL_SCISSOR={profile['small']}",
            "--env", f"WINEHUA_BENCHMARK_FPS={profile['fps']}",
            "--seconds", str(seconds), "--timeout-minutes", "4", "--poll-seconds", "5", "--keep-app", "--skip-push"]
        plan.append({"runId": run_id, "backend": backend, "command": command})
    with args.hap.open("rb") as stream:
        hap_hash = hashlib.file_digest(stream, "sha256").hexdigest()
    contract = {"device": args.device, "hapSha256": hap_hash,
                "payloadVersion": json.loads((args.payload / "manifest.json").read_text(encoding="utf8"))["suiteVersion"],
                "architecture": args.architecture, "profile": args.profile, "workload": profile,
                "sanityOnly": args.sanity, "warmupMs": warmup, "configuredRunSeconds": seconds, "runs": plan}
    (args.archive / "plan.json").write_text(json.dumps(contract, indent=2) + "\n", encoding="utf8")
    if args.plan_only:
        print(json.dumps(contract, indent=2))
        return 0
    policy = {"mode": "automation", "target": args.device, "artifactDir": str(args.archive),
              "redaction": "No signing logs, raw environment, tokens or unrelated app data.",
              "actions": "Install the exact HAP, push fixture payload, stop/start test app, bounded measurements, restore screen timeout."}
    (args.archive / "automation-policy.json").write_text(json.dumps(policy, indent=2) + "\n", encoding="utf8")
    hdc = resolve_hdc()
    outcomes = []
    def invoke(command, timeout=300):
        # Smoke itself polls for four minutes; leave time to collect its failure
        # before enforcing the outer bound. Preserve an outer timeout as a row.
        try:
            return subprocess.run(command, cwd=ROOT, timeout=timeout).returncode
        except subprocess.TimeoutExpired:
            print("Outer command timeout; preserve failure and stop trials", file=sys.stderr)
            return 124
    for action in ([sys.executable, "-X", "utf8", "automation/smoke.py", "install", "--device", args.device, "--hap", str(args.hap)],
                   [sys.executable, "-X", "utf8", "automation/smoke.py", "push", "--device", args.device, "--payload", str(args.payload)]):
        if invoke(action):
            return 1
    code, uid_text = hdc_sandbox_shell(hdc, args.device, "stat -c %u data/storage/el2/base/files", timeout=10)
    if code or not uid_text.strip().isdigit():
        print("App UID unavailable; cannot verify isolated app sessions", file=sys.stderr)
        return 1
    uid = uid_text.strip()
    try:
        hdc_shell(hdc, args.device, "power-shell timeout -o 3600000", timeout=10)
        for item in plan:
            code, output = hdc_shell(hdc, args.device, f"aa force-stop {BUNDLE}", timeout=10)
            code, processes = hdc_shell(hdc, args.device, "ps -A -o PID,PPID,UID,NAME", timeout=10)
            leftovers = [line for line in processes.splitlines() if len(line.split()) >= 4 and line.split()[2] == uid]
            if code or leftovers:
                raise RuntimeError(f"App processes remained before backend switch: {leftovers}")
            environment_before = capture_environment(hdc, args.device)
            print(f"PERF RUN {item['backend']} {item['runId']}", flush=True)
            code = invoke(item["command"])
            directory = args.archive / f"{suite}-{item['runId']}"
            result_path = directory / "device-results" / f"{test_id}.json"
            outcome = {"runId": item["runId"], "backend": item["backend"], "exit": code,
                       "coldProcessListEmpty": True, "environmentBefore": environment_before}
            app_pid = None
            if result_path.is_file():
                result = json.loads(result_path.read_text(encoding="utf8"))
                outcome["benchmark"] = result.get("benchmark")
                outcome["status"] = result.get("status")
            host_path = directory / "host-summary.json"
            if host_path.is_file():
                host = json.loads(host_path.read_text(encoding="utf8"))
                verdicts = [verdict for test in host.get("tests", []) for verdict in test.get("verdicts", [])
                            if verdict.get("check") == "benchmark"]
                if len(verdicts) == 1 and verdicts[0].get("status") == "PASS":
                    outcome["validatedMetrics"] = verdicts[0]["metrics"]
            window_path = result_path.with_name(result_path.name + ".hap-performance.json")
            if window_path.is_file():
                app_pid = json.loads(window_path.read_text(encoding="utf8"))["start"]["pid"]
            measured = outcome.get("validatedMetrics") or {}
            expected = {"vulkanBackend": item["backend"], "warmupMs": warmup,
                        "configuredRunMs": seconds * 1000, "drawsPerFrame": profile["draws"],
                        "smallScissor": bool(profile["small"]), "targetFps": profile["fps"]}
            outcome["measurementValid"] = code == 0 and all(measured.get(key) == value for key, value in expected.items())
            if code == 0 and not outcome["measurementValid"]:
                outcome["contractMismatch"] = {key: {"expected": value, "reported": measured.get(key)}
                                               for key, value in expected.items() if measured.get(key) != value}
                code = outcome["exit"] = 1
            outcome["environmentAfter"] = capture_environment(hdc, args.device, app_pid)
            outcomes.append(outcome)
            (args.archive / "outcomes.json").write_text(json.dumps(outcomes, indent=2) + "\n", encoding="utf8")
            if code:
                hdc_shell(hdc, args.device, f"aa force-stop {BUNDLE}", timeout=10)
                print("Measurement failed; preserve artifacts and resolve before more trials", file=sys.stderr)
                return 1
    finally:
        code, output = hdc_shell(hdc, args.device, "power-shell timeout -r", timeout=10)
        (args.archive / "screen-timeout-restored.log").write_text(output, encoding="utf8")
    summary = {"sanityOnly": args.sanity, "performanceGainVerified": False, "samples": {}}
    for backend in ("venus", "direct"):
        samples = [item["benchmark"] for item in outcomes if item["backend"] == backend]
        summary["samples"][backend] = {"count": len(samples), "medianGamePresentFps": statistics.median(sample["averageFps"] for sample in samples),
            "medianFrameMsP95": statistics.median(sample["frameMsP95"] for sample in samples),
            "medianFrameMsP99": statistics.median(sample["frameMsP99"] for sample in samples)}
        validated = [item["validatedMetrics"] for item in outcomes
                     if item["backend"] == backend and "validatedMetrics" in item]
        if len(validated) == len(samples):
            for metric in ("cpuMsPerFrame", "hapAcceptedGamePresentFps", "hapCpuMsPerSecond", "estimatedHapCpuMsPerGameFrame"):
                values = [sample.get(metric) for sample in validated]
                summary["samples"][backend]["median_" + metric] = (
                    statistics.median(values) if all(value is not None for value in values) else None)
    summary["nextReview"] = "Validate matching accepted output work, thermal/foreground conditions, noise and real-game coverage before claiming a gain."
    (args.archive / "summary.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf8")
    print(json.dumps(summary, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
