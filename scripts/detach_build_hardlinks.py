#!/usr/bin/env python3
"""Make reused build outputs independent before a compiler/stager overwrites them."""
import os
from pathlib import Path
import shutil
import sys
import tempfile

def detach(root):
    count = size = 0
    if not root.exists(): return count, size
    for directory, dirs, files in os.walk(root, followlinks=False):
        dirs[:] = [name for name in dirs if not (Path(directory) / name).is_symlink()]
        for name in files:
            path = Path(directory) / name
            if path.is_symlink(): continue
            stat = path.stat()
            if stat.st_nlink <= 1: continue
            fd, temp = tempfile.mkstemp(prefix='.vp-output-', dir=directory)
            os.close(fd)
            try:
                shutil.copy2(path, temp)
                os.replace(temp, path)
            finally:
                if os.path.exists(temp): os.unlink(temp)
            count += 1
            size += stat.st_size
    return count, size

if __name__ == '__main__':
    for arg in sys.argv[1:]:
        count, size = detach(Path(arg))
        print(f'Isolated {count} shared outputs ({size} bytes) in {arg}')
