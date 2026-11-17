#!/usr/bin/env python3
"""Copy a built runtime dependency, never an SDK link-only fallback."""
import argparse
from pathlib import Path
import shutil
from runtime_library_guard import RuntimeLibraryError, validate_library


def copy_runtime(ext, sdk, destination, name, soname, linker='', system=False):
    source = ext / soname
    target = destination / soname
    if source.is_file():
        # An ext symlink into the SDK is not a built runtime dependency.
        if source.resolve().is_relative_to(sdk.resolve()):
            raise RuntimeLibraryError(f'{source}: resolves into the SDK, build a runtime library')
        validate_library(source.read_bytes(), str(source))
        shutil.copyfile(source, target)
        if linker and linker != soname:
            shutil.copyfile(target, destination / linker)
        return
    if system and soname == 'libz.so':
        if target.exists() or target.is_symlink():
            # Only a byte-identical known generated SDK copy can be removed.
            # A changed file is left alone and blocks packaging for review.
            sdk_file = sdk / name
            if sdk_file.is_file() and target.read_bytes() == sdk_file.read_bytes():
                try:
                    validate_library(sdk_file.read_bytes(), str(sdk_file))
                except RuntimeLibraryError:
                    target.unlink()
                else:
                    raise RuntimeLibraryError(f'{target}: unexpected SDK runtime copy; review it before packaging')
            else:
                raise RuntimeLibraryError(f'{target}: unowned stale libz; review it before packaging')
        print('libz.so: use device system runtime (no SDK stub bundled)')
        return
    raise RuntimeLibraryError(f'{soname}: missing built runtime in {ext}; SDK fallback is forbidden')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ext', required=True, type=Path)
    parser.add_argument('--sdk', required=True, type=Path)
    parser.add_argument('--dest', required=True, type=Path)
    parser.add_argument('--system', action='store_true')
    parser.add_argument('name')
    parser.add_argument('soname')
    parser.add_argument('linker', nargs='?', default='')
    args = parser.parse_args()
    try:
        copy_runtime(args.ext, args.sdk, args.dest, args.name, args.soname, args.linker, args.system)
    except (RuntimeLibraryError, OSError) as error:
        parser.exit(1, f'runtime provenance: {error}\n')


if __name__ == '__main__':
    main()
