#!/usr/bin/env python3
"""Sign/unsign ELF files under a directory with binary-sign-tool."""

import argparse
import concurrent.futures
import os
import subprocess
import sys
import threading

BINARY_SIGN_TOOL = "binary-sign-tool"
LLVM_OBJCOPY = "llvm-objcopy"
MAX_WORKERS = min(os.cpu_count() or 1, 8)


class _ScanTracker:
    """Tracks in-flight scan tasks so main() waits until all are done."""
    def __init__(self):
        self._lock = threading.Lock()
        self._pending = 0
        self._all_submitted = False
        self._event = threading.Event()

    def add(self) -> None:
        with self._lock:
            self._pending += 1

    def done(self) -> None:
        should_set = False
        with self._lock:
            self._pending -= 1
            if self._pending == 0 and self._all_submitted:
                should_set = True
        if should_set:
            self._event.set()

    def mark_submitted(self) -> None:
        should_set = False
        with self._lock:
            self._all_submitted = True
            if self._pending == 0:
                should_set = True
        if should_set:
            self._event.set()

    def wait(self) -> None:
        self._event.wait()


def is_elf(filepath: str) -> bool:
    try:
        with open(filepath, "rb") as f:
            magic = f.read(4)
    except OSError:
        return False
    return magic == b"\x7fELF"


def unsign_elf(filepath: str) -> None:
    cmd = [
        LLVM_OBJCOPY,
        "--remove-section", ".codesign",
        filepath,
    ]
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        print(f"FAILED: {filepath} (unsign): {result.stderr.strip()}", file=sys.stderr)
        return False
    print(f"UNSIGNED: {filepath}")
    return True


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
        print(f"FAILED: {filepath} (sign): {result.stderr.strip()}", file=sys.stderr)
    else:
        print(f"SIGNED: {filepath}")


def process_file(filepath: str, do_unsign: bool, do_sign: bool, dry_run: bool) -> None:
    if dry_run:
        action = "UNSIGN" if do_unsign else "SIGN"
        print(f"WOULD_{action}: {filepath}")
        return
    if do_unsign:
        unsign_elf(filepath)
    if do_sign:
        sign_elf(filepath)


def walk_and_submit(path: str, sign_executor: concurrent.futures.ThreadPoolExecutor,
                    do_unsign: bool, do_sign: bool, dry_run: bool,
                    scan_pool: concurrent.futures.ThreadPoolExecutor,
                    scan_slots: threading.Semaphore,
                    tracker: _ScanTracker) -> None:
    """Walk a directory tree, offloading subdirectories to the scan pool when slots
    are available, and submit ELF files to the sign pool."""
    try:
        if os.path.isfile(path):
            if not os.path.islink(path) and is_elf(path):
                sign_executor.submit(process_file, path, do_unsign, do_sign, dry_run)
            return
        for dirpath, dirnames, filenames in os.walk(path):
            # Try to offload subdirectories to idle scan threads.
            for dirname in list(dirnames):
                subdir = os.path.join(dirpath, dirname)
                if scan_slots.acquire(blocking=False):
                    dirnames.remove(dirname)
                    tracker.add()
                    scan_pool.submit(
                        walk_and_submit, subdir,
                        sign_executor, do_unsign, do_sign, dry_run,
                        scan_pool, scan_slots, tracker,
                    )
            for filename in filenames:
                filepath = os.path.join(dirpath, filename)
                if os.path.islink(filepath):
                    continue
                if not is_elf(filepath):
                    continue
                sign_executor.submit(process_file, filepath, do_unsign, do_sign, dry_run)
    finally:
        scan_slots.release()
        tracker.done()


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Sign or unsign ELF files using binary-sign-tool."
    )
    group = parser.add_mutually_exclusive_group()
    group.add_argument(
        "--unsign", action="store_true",
        help="Remove .codesign section via llvm-objcopy"
    )
    group.add_argument(
        "--resign", action="store_true",
        help="Remove .codesign section then re-sign"
    )
    parser.add_argument("path", help="File or directory to process")
    parser.add_argument(
        "--dry-run", action="store_true",
        help="Only list ELF files that would be processed, without making changes"
    )
    args = parser.parse_args()

    if not os.path.isdir(args.path) and not os.path.lexists(args.path):
        print(f"Error: {args.path} is not a file or directory", file=sys.stderr)
        sys.exit(1)

    do_unsign = args.unsign or args.resign
    do_sign = not args.unsign  # sign by default, or on --resign

    mode = " (DRY RUN)" if args.dry_run else ""
    print(f"Scanning and processing with up to {MAX_WORKERS} threads{mode}")

    if not os.path.isdir(args.path):
        if not os.path.islink(args.path) and is_elf(args.path):
            process_file(args.path, do_unsign, do_sign, args.dry_run)
        print("Done.")
        return

    scan_slots = threading.Semaphore(MAX_WORKERS)
    tracker = _ScanTracker()

    with concurrent.futures.ThreadPoolExecutor(max_workers=MAX_WORKERS) as sign_pool:
        with concurrent.futures.ThreadPoolExecutor(max_workers=MAX_WORKERS) as scan_pool:
            for entry in os.listdir(args.path):
                entry_path = os.path.join(args.path, entry)
                scan_slots.acquire()
                tracker.add()
                scan_pool.submit(
                    walk_and_submit, entry_path,
                    sign_pool, do_unsign, do_sign, args.dry_run,
                    scan_pool, scan_slots, tracker,
                )
            tracker.mark_submitted()
            tracker.wait()


if __name__ == "__main__":
    main()
