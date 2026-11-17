"""Make wine.inf change when the packaged prefix builtins change.

WineEngineService preserves identical wine.inf files (including their mtime).
Wineboot uses that mtime to decide whether to refresh installed builtins. A
content identity in an INF comment connects the two without deleting prefixes
or refreshing them for unrelated graphics/Unix library updates.
"""
import argparse
import hashlib
from pathlib import Path, PurePosixPath

MARKER = b'; VintagePomelo builtin identity: sha256='
PE_SUFFIXES = {'.dll', '.exe', '.drv', '.ocx', '.cpl', '.acm'}


def is_builtin(name):
    path = PurePosixPath(name)
    return (len(path.parts) == 3 and path.parts[0] == 'bin'
            and path.parts[1].endswith('-windows')
            and path.suffix.lower() in PE_SUFFIXES)


def stamp(content, hashes):
    entries = sorted(hashes)
    if not entries:
        raise ValueError('No packaged Wine PE builtins found')
    identity = hashlib.sha256()
    for name, checksum in entries:
        identity.update(name.encode('utf-8') + b'\0' + checksum.encode('ascii') + b'\n')
    lines = [line for line in content.splitlines(keepends=True)
             if not line.startswith(MARKER)]
    clean = b''.join(lines)
    newline = b'\r\n' if b'\r\n' in content else b'\n'
    if clean and not clean.endswith(b'\n'):
        clean += newline
    return clean + MARKER + identity.hexdigest().encode('ascii') + newline


def stamp_archive(archive, replacements=None):
    """Targeted DLL staging must refresh the same prefix identity as assemble."""
    replacements = replacements or {}
    names = set(archive.namelist()) | set(replacements)
    hashes = [(name, hashlib.sha256(replacements[name] if name in replacements
                                    else archive.read(name)).hexdigest())
              for name in names if is_builtin(name)]
    return stamp(archive.read('share/wine/wine.inf'), hashes)


def validate_archive(archive):
    if archive.read('share/wine/wine.inf') != stamp_archive(archive):
        raise ValueError('Wine builtin DLL identity is stale; prefix upgrades would miss changed DLLs')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('runtime', type=Path)
    runtime = parser.parse_args().runtime
    inf = runtime / 'share/wine/wine.inf'
    hashes = [(p.relative_to(runtime).as_posix(), hashlib.sha256(p.read_bytes()).hexdigest())
              for p in (runtime / 'bin').glob('*-windows/*')
              if p.is_file() and is_builtin(p.relative_to(runtime).as_posix())]
    original = inf.read_bytes()
    updated = stamp(original, hashes)
    if updated != original:
        inf.write_bytes(updated)


if __name__ == '__main__':
    main()
