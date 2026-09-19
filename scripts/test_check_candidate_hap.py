import struct
import sys
import unittest
from pathlib import Path


sys.path.insert(0, str(Path(__file__).resolve().parent))
from check_candidate_hap import ELF_HEADER, PROGRAM_HEADER, load_signature


def make_elf(section_offset: int, section_count: int) -> bytearray:
    image = bytearray(512)
    ident = b"\x7fELF\x02\x01\x01" + b"\0" * 9
    ELF_HEADER.pack_into(
        image, 0, ident, 3, 183, 1, 0, ELF_HEADER.size, section_offset, 0,
        ELF_HEADER.size, PROGRAM_HEADER.size, 1, 64, section_count, 1,
    )
    PROGRAM_HEADER.pack_into(
        image, ELF_HEADER.size, 1, 5, 0, 0, 0, len(image), len(image), 0x1000,
    )
    image[160:176] = b"candidate-payload"
    return image


class CandidateHapElfTest(unittest.TestCase):
    def test_release_section_table_changes_do_not_change_runtime_signature(self) -> None:
        built = make_elf(256, 3)
        packaged = make_elf(0, 0)
        self.assertEqual(load_signature("built", built), load_signature("packaged", packaged))

    def test_loadable_byte_change_fails_runtime_signature(self) -> None:
        built = make_elf(256, 3)
        packaged = make_elf(0, 0)
        packaged[160] ^= 0xFF
        self.assertNotEqual(load_signature("built", built), load_signature("packaged", packaged))


if __name__ == "__main__":
    unittest.main()
