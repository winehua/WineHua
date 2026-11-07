#!/usr/bin/env python3
"""Compile the actual Wine IME fallback message initializers and check their wire fields."""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=ROOT / 'thirdparty/wine/dlls/imm32/ime.c')
    parser.add_argument('--cc', default='cc')
    args = parser.parse_args()
    source = args.source.read_text(encoding='utf-8')
    body = source[source.index('UINT WINAPI ImeToAsciiEx('):]
    blocks = re.findall(r'TRANSMSG msg = \{.*?\};', body, re.S)
    fallback = next(b for b in blocks if 'WM_KEYUP' in b)
    unconsumed = next(b for b in blocks if 'WM_IME_KEYDOWN' in b)
    harness = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
typedef unsigned int UINT;
typedef uintptr_t WPARAM;
typedef intptr_t LPARAM;
typedef struct { UINT message; WPARAM wParam; LPARAM lParam; } TRANSMSG;
#define WM_KEYDOWN 0x100
#define WM_KEYUP 0x101
#define WM_IME_KEYDOWN 0x290
#define WM_IME_KEYUP 0x291
#define KF_UP 0x8000
#define MAKELPARAM(low, high) ((LPARAM)(int32_t)(((uint32_t)(high) << 16) | (uint16_t)(low)))
static TRANSMSG fallback(UINT vkey, UINT vsc) { FALLBACK return msg; }
static TRANSMSG unconsumed(UINT vkey, UINT vsc) { UNCONSUMED return msg; }
int main(void) {
    /* Escape, skill 1-5, Enter, extended Enter, Ctrl and extended arrow. */
    const UINT scans[] = {1, 2, 3, 4, 5, 6, 0x1c, 0x11c, 0x1d, 0x14d};
    const UINT keys[] = {27, 49, 50, 51, 52, 53, 13, 13, 17, 39};
    const UINT flags[] = {0, 0x4000, 0xc000};
    unsigned count = 0;
    for (unsigned i = 0; i < sizeof(scans) / sizeof(scans[0]); i++) {
        for (unsigned f = 0; f < sizeof(flags) / sizeof(flags[0]); f++) {
            UINT scan = scans[i] | flags[f];
            TRANSMSG messages[] = {fallback(keys[i], scan), unconsumed(keys[i], scan)};
            for (unsigned branch = 0; branch < 2; branch++) {
                TRANSMSG msg = messages[branch];
                uint32_t wire = (uint32_t)msg.lParam;
                assert(msg.wParam == keys[i]);
                assert(((wire >> 16) & 0xff) == (scans[i] & 0xff));
                assert(((wire >> 24) & 1) == ((scans[i] >> 8) & 1));
                assert(((wire >> 30) & 1) == ((flags[f] >> 14) & 1));
                assert(((wire >> 31) & 1) == ((flags[f] >> 15) & 1));
                assert((wire & 0xffff) == 1);
                assert(msg.message == (branch ? ((scan & KF_UP) ? WM_IME_KEYUP : WM_IME_KEYDOWN)
                                             : ((scan & KF_UP) ? WM_KEYUP : WM_KEYDOWN)));
                count++;
            }
        }
    }
    printf("PASS: %u actual IME fallback messages preserve scan, extended, repeat and release flags\n", count);
    return 0;
}
'''.replace('FALLBACK', fallback).replace('UNCONSUMED', unconsumed)
    with tempfile.TemporaryDirectory(prefix='winehua-ime-key-') as directory:
        source_path = Path(directory) / 'test.c'
        exe = Path(directory) / 'test'
        source_path.write_text(harness, encoding='utf-8')
        subprocess.run([args.cc, '-std=c11', '-Wall', '-Wextra', '-Werror', str(source_path), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)

if __name__ == '__main__':
    main()
