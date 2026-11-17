#!/usr/bin/env python3
"""Reject Wine configure results that silently disable required X11 features."""
import argparse
from pathlib import Path
import re

REQUIRED = {'HAVE_X11_EXTENSIONS_XINPUT2_H': '1', 'SONAME_LIBXI': '"libXi.so.6"',
    'SONAME_LIBXRANDR': '"libXrandr.so.2"', 'SONAME_LIBXRENDER': '"libXrender.so.1"',
    'SONAME_LIBXCURSOR': '"libXcursor.so.1"', 'SONAME_LIBXFIXES': '"libXfixes.so.3"',
    'SONAME_LIBXINERAMA': '"libXinerama.so.1"'}

def verify(text):
    macros = dict(re.findall(r'^#define\s+(\w+)\s+([^\n]+)', text, re.M))
    for name, value in REQUIRED.items():
        if macros.get(name, '').strip() != value:
            raise ValueError('Wine X11 feature disabled or wrong SONAME: ' + name)

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--header', type=Path, required=True)
    args = parser.parse_args()
    verify(args.header.read_text())
    print('Wine X11 input/display/cursor features enabled')
