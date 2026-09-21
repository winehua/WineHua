"""Run one bounded HDC operation and keep a local, target-redacted command ledger."""
import argparse
import datetime
import json
from pathlib import Path
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--target", required=True)
parser.add_argument("--artifacts", type=Path, required=True)
parser.add_argument("--label", required=True)
parser.add_argument("--timeout", type=int, default=45)
parser.add_argument("--show", action="store_true")
parser.add_argument("command", nargs=argparse.REMAINDER)
args = parser.parse_args()
command = args.command[1:] if args.command[:1] == ["--"] else args.command
assert command
assert Path(args.label).name == args.label
args.artifacts.mkdir(parents=True, exist_ok=True)
output = args.artifacts / (args.label + ".log")
ledger = {
    "time": datetime.datetime.now(datetime.timezone.utc).isoformat(),
    "target": "handoff-tablet", "policy": "automation", "decision": "allowed",
    "redactionStatus": "raw-local-only; review before external sharing",
    "redactionPolicy": "harmony-next-default-v1", "timeoutSeconds": args.timeout,
    "sourceCommand": ["hdc", "-t", "<handoff-tablet>"] + command,
    "artifacts": [str(output)],
}
with output.open("wb") as stream:
    try:
        result = subprocess.run(["hdc", "-t", args.target] + command,
                                stdout=stream, stderr=subprocess.STDOUT, timeout=args.timeout)
        ledger["returncode"] = result.returncode
    except subprocess.TimeoutExpired:
        ledger["returncode"] = 124
captured = output.read_text(encoding="utf-8", errors="replace")
# hdc can return zero when the device is disconnected or bm rejects a HAP.
if ledger["returncode"] == 0 and any(
    line.startswith(("[Fail]", "error: failed", "invalid number of parameters", "Invalid log type"))
    for line in captured.splitlines()
):
    ledger["returncode"] = 1
with (args.artifacts / "commands.jsonl").open("a", encoding="utf-8") as stream:
    stream.write(json.dumps(ledger) + "\n")
print(json.dumps({"label": args.label, "returncode": ledger["returncode"], "bytes": output.stat().st_size}))
if args.show:
    print(captured[-5000:])
raise SystemExit(ledger["returncode"])
