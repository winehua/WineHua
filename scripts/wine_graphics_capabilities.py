#!/usr/bin/env python3
"""Reject Wine Wayland builds that silently omit the OpenGL implementation."""
import argparse
from pathlib import Path
import re
import subprocess
import sys


def check_config(build):
    config = (build / 'include/config.h').read_text()
    if not re.search(r'^\s*#define\s+HAVE_LIBWAYLAND_EGL\s+1\s*$', config, re.M):
        raise ValueError('Wayland OpenGL disabled (HAVE_LIBWAYLAND_EGL missing). '
                         'Configure with WAYLAND_EGL_CFLAGS/LIBS from the target sysroot.')
    makefile = (build / 'Makefile').read_text()
    match = re.search(r'^WAYLAND_EGL_LIBS[ 	]*=[ 	]*(.*)$', makefile, re.M)
    if not match or not match.group(1).strip():
        raise ValueError('WAYLAND_EGL_LIBS is empty; refusing the OpenGL stub driver')


def check_driver(driver, readelf):
    symbols = subprocess.check_output([readelf, '--dyn-syms', str(driver)], text=True)
    for name in ('wl_egl_window_create', 'wl_egl_window_resize'):
        if not re.search(r'\b' + name + r'\b', symbols):
            raise ValueError(f'Wayland driver has no {name}; OpenGL implementation not linked')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--driver', type=Path)
    parser.add_argument('--readelf', default='readelf')
    args = parser.parse_args()
    try:
        check_config(args.build)
        if args.driver:
            check_driver(args.driver, args.readelf)
    except (ValueError, OSError, subprocess.SubprocessError) as error:
        print('error: ' + str(error), file=sys.stderr)
        return 1
    print('Wine Wayland OpenGL capability verified' + (' (configured and linked)' if args.driver else ' (configured)'))
    return 0


if __name__ == '__main__':
    sys.exit(main())
