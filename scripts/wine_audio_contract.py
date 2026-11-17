"""Read the shared WASAPI format actually compiled into wineohos.so.

The driver exports the same constant that GetMixFormat copies to its caller.
Unlike a source/patch hash, this survives stripping and identifies the library
that is about to be shipped, including when an old runtime HAP is reused.
"""
import struct


SYMBOL = b'winehua_shared_audio_format_v1'


def shared_audio_format(data):
    if len(data) < 64 or data[:4] != b'\x7fELF' or data[4] not in (1, 2) or data[5] not in (1, 2):
        raise ValueError('invalid ELF header')
    endian = '<' if data[5] == 1 else '>'
    wide = data[4] == 2

    def read(fmt, offset):
        if offset < 0 or offset + struct.calcsize(endian + fmt) > len(data):
            raise ValueError('truncated ELF structure')
        return struct.unpack_from(endian + fmt, data, offset)

    def region(offset, size):
        if offset < 0 or size < 0 or offset + size > len(data):
            raise ValueError('truncated ELF data')
        return data[offset:offset + size]

    offset, = read('Q' if wide else 'I', 40 if wide else 32)
    stride, count = read('HH', 58 if wide else 46)
    fmt = 'IIQQQQIIQQ' if wide else 'IIIIIIIIII'
    if stride < struct.calcsize(endian + fmt):
        raise ValueError('invalid section stride')
    sections = [read(fmt, offset + i * stride) for i in range(count)]
    found = []
    for section in sections:
        if section[1] != 11:  # SHT_DYNSYM is retained by llvm-strip.
            continue
        if section[6] >= len(sections) or section[9] < (24 if wide else 16):
            raise ValueError('invalid dynamic symbol table')
        strings = sections[section[6]]
        names = region(strings[4], strings[5])
        region(section[4], section[5])
        for pos in range(section[4], section[4] + section[5], section[9]):
            symbol = read('IBBHQQ' if wide else 'IIIBBH', pos)
            name, info, visibility, index, value, size = (symbol if wide else
                (symbol[0], symbol[3], symbol[4], symbol[5], symbol[1], symbol[2]))
            if name >= len(names) or names[name:].split(b'\0', 1)[0] != SYMBOL:
                continue
            if (not 0 < index < len(sections) or info & 15 != 1 or
                    info >> 4 != 1 or visibility & 3 not in (0, 3) or size != 40):
                raise ValueError('invalid exported shared audio format')
            owner = sections[index]
            relative = value - owner[3]
            if not owner[2] & 2 or owner[1] == 8 or relative < 0 or relative + size > owner[5]:
                raise ValueError('shared audio format is not in loaded ELF data')
            found.append(region(owner[4] + relative, size))
    if len(found) != 1:
        raise ValueError('missing shared Float32 audio contract; rebuild wineohos.drv with patches 0058 and 0062')
    values = struct.unpack(endian + 'HHIIHHHHIIHH8s', found[0])
    expected = (0xfffe, 2, 48000, 384000, 8, 32, 22, 32, 3,
                3, 0, 0x10, b'\x80\x00\x00\xaa\x00\x38\x9b\x71')
    if values != expected:
        raise ValueError('shared WASAPI mix format must be 48000 Hz stereo IEEE Float32 (32 bits, blockAlign 8)')
    return {'sampleRate': values[2], 'channels': values[1], 'bitsPerSample': values[5],
            'blockAlign': values[4], 'averageBytesPerSecond': values[3], 'format': 'IEEE_FLOAT'}
