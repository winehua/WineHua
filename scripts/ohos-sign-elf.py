#!/usr/bin/env python3
"""Sign all ELF files under the given directory with binary-sign-tool."""

import os
import subprocess
import sys

BINARY_SIGN_TOOL = "binary-sign-tool"


def is_elf(filepath: str) -> bool:
    try:
        with open(filepath, "rb") as f:
            magic = f.read(4)
    except OSError:
        return False
    return magic == b"\x7fELF"


def sign_elf(filepath: str) -> None:
    cmd = [
        BINARY_SIGN_TOOL,
        "sign",
        "-inFile", filepath,
        "-outFile", filepath,
        "-selfSign", "1",
    ]
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        print(f"FAILED: {filepath}: {result.stderr.strip()}", file=sys.stderr)
    else:
        print(f"SIGNED: {filepath}")


def main() -> None:
    if len(sys.argv) != 2:
        print(f"Usage: {sys.argv[0]} <directory>", file=sys.stderr)
        sys.exit(1)

    root = sys.argv[1]
    if not os.path.isdir(root):
        print(f"Error: {root} is not a directory", file=sys.stderr)
        sys.exit(1)

    for dirpath, dirnames, filenames in os.walk(root):
        for filename in filenames:
            filepath = os.path.join(dirpath, filename)
            if os.path.islink(filepath):
                continue
            if not is_elf(filepath):
                continue
            sign_elf(filepath)


if __name__ == "__main__":
    main()
