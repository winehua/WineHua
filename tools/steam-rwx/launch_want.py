"""Launch through GameHook using its decoded JSON argv and environment fields.

The legacy game_argN and d3d_env_valueN fields are literal strings; percent
encoding them silently sends the encoded text to Wine. Keep the argument count
alongside JSON because GameHook uses it as the decoding limit.
"""
import argparse
import json
from pathlib import Path
import subprocess
import sys
from urllib.parse import quote


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--target", required=True)
    parser.add_argument("--artifacts", type=Path, required=True)
    parser.add_argument("--label", required=True)
    parser.add_argument("--exe", required=True)
    parser.add_argument("--container", default="default")
    parser.add_argument("--arg", action="append", default=[])
    parser.add_argument("--env", action="append", default=[])
    args = parser.parse_args()
    env = []
    for value in args.env:
        key, sep, val = value.partition("=")
        if not sep or not key:
            parser.error("--env requires KEY=VALUE")
        env.append({"key": key, "value": val})
    values = {
        "winehua.mode": "game",
        "winehua.game_path": quote(args.exe, safe=""),
        "winehua.game_argc": str(len(args.arg)),
        "winehua.game_args_json": quote(json.dumps(args.arg), safe=""),
        "winehua.container_id": quote(args.container, safe=""),
        "winehua.d3d_env_json": quote(json.dumps(env), safe=""),
    }
    command = "aa start -b app.hackeris.winehua -a EntryAbility"
    for key, value in values.items():
        command += f" --ps {key} {value}"
    return subprocess.call([
        sys.executable, str(Path(__file__).with_name("device_command.py")),
        "--target", args.target, "--artifacts", str(args.artifacts),
        "--label", args.label, "--show", "--", "shell", command,
    ])


if __name__ == "__main__":
    raise SystemExit(main())
