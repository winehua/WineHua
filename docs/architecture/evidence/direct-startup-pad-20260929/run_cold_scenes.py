import json
import subprocess
import sys
from pathlib import Path

root = Path(r"F:\WineHua\proton-ohos-worktree")
sys.path.insert(0, str(root))
from automation.smoke import resolve_hdc, hdc_shell

archive = Path(r"F:\WineHua\.temp\direct-pad-startup-20260929")
hdc = resolve_hdc()
device = "5KPBB25818203996"
outcomes = []
try:
    for number in range(1, 4):
        run_id = f"pad-scene-ready-cold-{number}-20260929"
        rc, output = hdc_shell(hdc, device, "aa force-stop app.hackeris.winehua", timeout=10)
        print("COLD_STOP", number, rc, output.strip(), flush=True)
        rc, output = hdc_shell(hdc, device, "ps -A -o PID,PPID,UID,NAME", timeout=10)
        remaining = [line for line in output.splitlines() if "20020271" in line]
        if rc or remaining:
            raise RuntimeError(f"App processes remain before cold start: {remaining}")
        hdc_shell(hdc, device, "power-shell timeout -o 600000", timeout=10)
        capture_dir = archive / f"cold-{number}-captures"
        capture = subprocess.Popen([sys.executable, "-X", "utf8", "automation/capture_direct_desktop_scene.py",
            "--run-id", run_id, "--device", device, "--archive", str(capture_dir),
            "--renderer", "vulkan", "--settle-seconds", "1.2", "--probe-input", "--timeout", "240"], cwd=root)
        try:
            run = subprocess.run([sys.executable, "-X", "utf8", "automation/smoke.py", "run",
                "--suite", "direct-desktop-scene", "--prefix", "reuse", "--device", device,
                "--run-id", run_id, "--archive-root", str(archive), "--desktop-mode", "virtual",
                "--desktop-renderer", "vulkan", "--direct-ncp-session", "--env", "WINEHUA_D3D_INPUT_TRACE=1",
                "--timeout-minutes", "3", "--keep-app", "--poll-seconds", "2", "--skip-push"], cwd=root, timeout=240)
            if run.returncode and capture.poll() is None:
                capture.terminate()
            capture_code = capture.wait(timeout=30)
        finally:
            if capture.poll() is None:
                capture.terminate()
                capture.wait(timeout=15)
        rc, pid_text = hdc_shell(hdc, device, "pidof app.hackeris.winehua", timeout=10)
        pid = int(pid_text.strip())
        log_dir = archive / f"cold-{number}-logs"
        log_dir.mkdir(parents=True, exist_ok=True)
        for tag in ["SMOKE", "DirectVkDesktop", "DirectWineSurface", "wine"]:
            rc, output = hdc_shell(hdc, device, f"hilog -z100 -T {tag}", timeout=10)
            lines = [line.split("|__env=")[0] for line in output.splitlines() if f" {pid} " in line]
            if tag == "wine":
                lines = [line for line in lines if any(s in line for s in ["state:ready", "state:stopped", "desktop", "ensureDesktop"])]
            (log_dir / f"{tag}.log").write_bytes(("\n".join(lines)+"\n").encode("utf-8"))
        outcome = {"runId": run_id, "appPid": pid, "coldProcessListEmpty": True,
                   "prewarm": False, "stallDump": False, "smokeExit": run.returncode,
                   "captureExit": capture_code}
        outcomes.append(outcome)
        (archive / "cold-scene-outcomes.json").write_bytes((json.dumps(outcomes, indent=2)+"\n").encode("utf-8"))
        print("COLD_SCENE_OUTCOME", json.dumps(outcome), flush=True)
        if run.returncode or capture_code:
            sys.exit(1)
finally:
    rc, output = hdc_shell(hdc, device, "power-shell timeout -r", timeout=10)
    (archive / "screen-timeout-restored.log").write_bytes(output.encode("utf-8"))
    print("SCREEN_TIMEOUT_RESTORED", rc, output.strip(), flush=True)
