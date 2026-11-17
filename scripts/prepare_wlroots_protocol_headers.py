#!/usr/bin/env python3
"""Generate architecture-independent protocol enums for wlroots host builds."""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

INCLUDE = re.compile(r'^\s*#\s*include\s*[<"]wayland-protocols/([a-z0-9-]+)-enum\.h[>"]', re.M)


def prepare(wlroots, protocols, scanner, output):
    needed = set()
    for header in (wlroots / 'include').rglob('*.h'):
        needed.update(INCLUDE.findall(header.read_text(encoding='utf-8')))
    if not needed:
        raise ValueError('wlroots protocol enum inventory is empty')
    output.mkdir(parents=True, exist_ok=True)
    # Stage every result before replacing existing generated headers. Never copy
    # the rest of the guest sysroot into the host include path.
    with tempfile.TemporaryDirectory(prefix='.wlroots-enums-', dir=output) as temp:
        stage = Path(temp)
        for name in sorted(needed):
            matches = list(protocols.rglob(name + '.xml'))
            if len(matches) != 1:
                raise ValueError(f'Expected one pinned protocol XML for {name}, found {len(matches)}')
            target = stage / (name + '-enum.h')
            subprocess.run([str(scanner), '--strict', 'enum-header', str(matches[0]), str(target)], check=True)
            if not target.is_file() or target.stat().st_size == 0:
                raise ValueError('Scanner did not produce ' + target.name)
        for target in stage.iterdir():
            target.replace(output / target.name)
    return sorted(needed)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for option in ('wlroots', 'protocols', 'scanner', 'output'):
        parser.add_argument('--' + option, type=Path, required=True)
    args = parser.parse_args()
    result = prepare(args.wlroots, args.protocols, args.scanner, args.output)
    print(f'Prepared {len(result)} wlroots protocol enum headers')

if __name__ == '__main__':
    main()
