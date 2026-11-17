#!/usr/bin/env python3
"""Fetch the pinned LLVM-MinGW archive and validate it before extraction."""
import hashlib
import os
from pathlib import Path
import sys

from github_release_download import open_distribution

NAME = 'llvm-mingw-20260826-ucrt-ubuntu-22.04-x86_64.tar.xz'
DIST = {'source': 'https://github.com/mstorsjo/llvm-mingw/releases/download/20260826/' + NAME,
        'file': NAME, 'bytes': 83880560}
SHA256 = 'cee8d2ce3da5145ce4dc882e70d0b0719a783d53a99752c60948fc0659975a65'


def digest(path):
    value = hashlib.sha256()
    with path.open('rb') as source:
        for block in iter(lambda: source.read(1024 * 1024), b''):
            value.update(block)
    return value.hexdigest()


def download(target, dist=DIST, checksum=SHA256, opener=None):
    target = Path(target)
    pending = target.with_suffix(target.suffix + '.part')
    if target.is_symlink() or pending.is_symlink():
        raise ValueError('Linked toolchain archive rejected')
    if target.is_file() and target.stat().st_size == dist['bytes'] and digest(target) == checksum:
        return
    target.parent.mkdir(parents=True, exist_ok=True)
    try:
        with open_distribution(dist, repository='mstorsjo/llvm-mingw', opener=opener) as source, pending.open('wb') as output:
            size = 0
            for block in iter(lambda: source.read(1024 * 1024), b''):
                size += len(block)
                if size > dist['bytes']:
                    raise ValueError('Oversized LLVM-MinGW archive')
                output.write(block)
        if pending.stat().st_size != dist['bytes'] or digest(pending) != checksum:
            raise ValueError('LLVM-MinGW archive checksum mismatch')
        os.replace(pending, target)
    finally:
        pending.unlink(missing_ok=True)


if __name__ == '__main__':
    if len(sys.argv) != 2:
        raise SystemExit('usage: download_llvm_mingw.py <target-archive>')
    download(sys.argv[1])
    print('Pinned LLVM-MinGW archive verified')
