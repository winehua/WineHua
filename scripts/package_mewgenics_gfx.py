#!/usr/bin/env python3
"""Package the separately qualified Mewgenics GL runtime without touching Wine."""
import argparse
import hashlib
import json
from pathlib import Path
import zipfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--input', type=Path, required=True, help='relocated x86_64 Zink bundle')
    parser.add_argument('--output', type=Path, default=ROOT / 'entry/src/main/resources/rawfile')
    args = parser.parse_args()
    for name in ('lib/libEGL.so', 'lib/libgallium-25.0.1.so', 'lib/dri/virtio_gpu_dri.so'):
        if not (args.input / name).is_file():
            parser.error(f'missing required Zink runtime file: {name}')
    driver = (args.input / 'lib/dri/virtio_gpu_dri.so').read_bytes()
    if b'ZINK_WINEHUA_SHADOW_TRANSFERS' not in driver:
        parser.error('runtime lacks the required shadow transfer fix')
    args.output.mkdir(parents=True, exist_ok=True)
    output = args.output / 'mewgenics-gfx.zip'
    with zipfile.ZipFile(output, 'w', compression=zipfile.ZIP_DEFLATED, compresslevel=6) as archive:
        for path in sorted(args.input.rglob('*')):
            if path.is_file():
                name = path.relative_to(args.input).as_posix()
                if name != '.manifest':
                    archive.write(path, name)
    with zipfile.ZipFile(output) as archive:
        assert archive.testzip() is None
    manifest = dict(schemaVersion=1, profile='mewgenics-zink-v6', payload=output.name,
                    payloadSha256=hashlib.sha256(output.read_bytes()).hexdigest(),
                    driverSha256=hashlib.sha256(driver).hexdigest())
    (args.output / 'mewgenics-gfx-manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(f'{output}: {output.stat().st_size} bytes; CRC PASS')


if __name__ == '__main__':
    main()
