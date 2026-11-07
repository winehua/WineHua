"""Inspect Clang response files without changing the arguments sent to Clang."""
from pathlib import Path
import shlex
import sys


def expanded(args, stack=()):
    for arg in args:
        if arg.startswith('@'):
            path = Path(arg[1:]).resolve()
            if path in stack or len(stack) >= 16:
                raise ValueError('cyclic or excessively nested Clang response file')
            yield from expanded(shlex.split(path.read_text(encoding='utf-8-sig')), (*stack, path))
        else:
            yield arg


def inspect(args):
    target = ''
    link = True
    next_target = False
    for arg in expanded(args):
        if next_target:
            target = arg
            next_target = False
        if arg.startswith(('--target=', '-target=')):
            target = arg.partition('=')[2]
        elif arg in ('--target', '-target'):
            next_target = True
        if arg in ('-c','-E','-S','-fsyntax-only','-M','-MM','--version','-dumpversion','-dumpmachine'):
            link = False
    if '\n' in target or '\r' in target:
        raise ValueError('invalid target triple')
    return target, link


if __name__ == '__main__':
    target, link = inspect(sys.argv[1:])
    print(target)
    print(int(link))
