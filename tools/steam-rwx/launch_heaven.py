"""Launch Heaven 4.0 with the data and system-script arguments used by its launcher."""
import argparse
from pathlib import Path
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--target", required=True)
    parser.add_argument("--artifacts", type=Path, required=True)
    parser.add_argument("--game-dir", required=True, help=r"Windows path containing bin and data")
    parser.add_argument("--label", default="heaven-d3d11")
    parser.add_argument("--container", default="default")
    args = parser.parse_args()
    command = [sys.executable, str(Path(__file__).with_name("launch_want.py")),
               "--target", args.target, "--artifacts", str(args.artifacts),
               "--label", args.label, "--container", args.container,
               "--exe", args.game_dir.rstrip("\\/") + r"\bin\Heaven.exe"]
    for value in ["-data_path", "../", "-engine_config", "../data/heaven_4.0.cfg",
                  "-system_script", "heaven/unigine.cpp", "-video_app", "direct3d11",
                  "-video_width", "800", "-video_height", "600", "-video_fullscreen", "0",
                  "-sound_app", "null", "-extern_define", "RELEASE"]:
        command.append("--arg=" + value)
    for value in ["WINEHUA_WOW64_ENGINE=box", "WINEHUA_WINEDEBUG=-all,err+all",
                  "WINEHUA_EARLY_FAULT=0"]:
        command.extend(["--env", value])
    return subprocess.call(command)


if __name__ == "__main__":
    raise SystemExit(main())
