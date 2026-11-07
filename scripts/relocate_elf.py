#!/usr/bin/env python3
"""Relocate ELF dependencies without moving sections or changing load segments.

OHOS's loader cannot reliably load files whose layout was rewritten by patchelf.
Absolute DT_NEEDED names already contain the basename: point at that suffix in
the existing string table. Keep version references consistent as well.
"""
import argparse
from pathlib import Path
import struct


class ElfImage:
    def __init__(self, path):
        self.path = path
        self.data = bytearray(path.read_bytes())
        self.changes = []
        b = self.data
        if b[:4] != b'\x7fELF' or b[4] not in (1, 2) or b[5] not in (1, 2):
            raise ValueError(f'Invalid ELF: {path}')
        self.endian = '<' if b[5] == 1 else '>'
        self.word = 'Q' if b[4] == 2 else 'I'
        self.width = struct.calcsize(self.word)
        self.machine = self.unpack('H', 18)[0]
        phoff = self.unpack(self.word, 32 if self.width == 8 else 28)[0]
        phsize, phnum = self.unpack('HH', 54 if self.width == 8 else 42)
        segments = [self.unpack('IIQQQQQQ' if self.width == 8 else 'IIIIIIII', phoff + i * phsize)
                    for i in range(phnum)]
        self.loads = []
        self.entries = []
        dynamic = None
        for segment in segments:
            if self.width == 8:
                kind, _, offset, address, _, size, _, _ = segment
            else:
                kind, offset, address, _, size, _, _, _ = segment
            if kind == 1:
                self.loads.append((address, offset, size))
            elif kind == 2:
                dynamic = (offset, size)
        if dynamic:
            for offset in range(dynamic[0], sum(dynamic), self.width * 2):
                tag, value = self.unpack(self.word * 2, offset)
                if not tag:
                    break
                self.entries.append((tag, value, offset + self.width))
        self.tags = {tag: value for tag, value, _ in self.entries}
        self.strbase = self.file_offset(self.tags[5]) if 5 in self.tags else 0

    def unpack(self, fmt, offset):
        return struct.unpack_from(self.endian + fmt, self.data, offset)

    def file_offset(self, address):
        for start, offset, size in self.loads:
            if start <= address < start + size:
                return offset + address - start
        raise ValueError(f'{self.path}: unmapped ELF address {address:#x}')

    def string(self, index):
        if index >= self.tags.get(10, 0):
            raise ValueError(f'{self.path}: invalid string offset {index}')
        start = self.strbase + index
        return bytes(self.data[start:self.data.index(0, start)]).decode('utf-8')

    def plan(self, build_root, provided):
        replacements = {}
        dependencies = search_paths = 0
        for tag, value, offset in self.entries:
            if tag == 1:
                name = self.string(value)
                if '/' in name:
                    basename = name.rsplit('/', 1)[1]
                    if (self.machine, basename) not in provided:
                        raise ValueError(f'{self.path}: bundled dependency missing for {name}')
                    new_value = value + len(name[:name.rfind('/') + 1].encode('utf-8'))
                    replacements[value] = new_value
                    self.changes.append((offset, self.word, new_value))
                    dependencies += 1
            elif tag in (15, 29):  # DT_RPATH / DT_RUNPATH
                path = self.string(value)
                keep = [p for p in path.split(':')
                        if p != build_root and not p.startswith(build_root + '/')]
                new_path = ':'.join(keep)
                if new_path == path:
                    continue
                if not new_path:
                    new_value = 0  # The first ELF string is always empty.
                elif path.endswith(new_path):
                    new_value = value + len(path[:-len(new_path)].encode('utf-8'))
                else:
                    raise ValueError(f'{self.path}: rebuild with a portable RUNPATH: {path}')
                self.changes.append((offset, self.word, new_value))
                search_paths += 1
        address = self.tags.get(0x6ffffffe, 0)  # DT_VERNEED
        for _ in range(self.tags.get(0x6fffffff, 0)):  # DT_VERNEEDNUM
            offset = self.file_offset(address)
            _, _, filename, _, next_entry = self.unpack('HHIII', offset)
            if filename in replacements:
                self.changes.append((offset + 4, 'I', replacements[filename]))
            if not next_entry:
                break
            address += next_entry
        return dependencies, search_paths

    def apply(self):
        for offset, fmt, value in self.changes:
            struct.pack_into(self.endian + fmt, self.data, offset, value)
        if self.changes:
            self.path.write_bytes(self.data)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-root', required=True, type=Path)
    parser.add_argument('directories', nargs='+', type=Path)
    args = parser.parse_args()
    paths = {p.resolve() for directory in args.directories
             for p in directory.rglob('*') if p.is_file()}
    images = {}
    for path in sorted(paths):
        with path.open('rb') as handle:
            if handle.read(4) == b'\x7fELF':
                images[path] = ElfImage(path)
    provided = {(images[p.resolve()].machine, p.name) for directory in args.directories
                for p in directory.rglob('*') if p.is_file() and p.resolve() in images}
    dependencies = search_paths = 0
    # Validate every file before mutating any file.
    for image in images.values():
        d, r = image.plan(str(args.build_root.resolve()), provided)
        dependencies += d
        search_paths += r
    for image in images.values():
        image.apply()
    print(f'Checked {len(images)} ELF files; relocated {dependencies} dependencies, '
          f'removed build paths from {search_paths} search paths; layouts preserved.')


if __name__ == '__main__':
    main()
