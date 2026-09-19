import hashlib
import io
import json
import struct
import sys
import unittest
from pathlib import Path
from zipfile import ZipFile


sys.path.insert(0, str(Path(__file__).resolve().parent))
from check_runtime_components import Archive, ValidationError, elf_needed, validate_graphics_runtime_payload


ELF_HEADER = struct.Struct("<HHIQQQIHHHHHH")
PROGRAM_HEADER = struct.Struct("<IIQQQQQQ")
DYNAMIC_ENTRY = struct.Struct("<qQ")


def make_dynamic_elf() -> bytes:
    image = bytearray(0x400)
    image[:16] = b"\x7fELF\x02\x01\x01" + b"\0" * 9
    ELF_HEADER.pack_into(
        image, 16, 3, 183, 1, 0, 64, 0, 0,
        64, PROGRAM_HEADER.size, 2, 0, 0, 0,
    )
    PROGRAM_HEADER.pack_into(
        image, 64, 1, 4, 0, 0, 0, len(image), len(image), 0x1000,
    )
    PROGRAM_HEADER.pack_into(
        image, 64 + PROGRAM_HEADER.size, 2, 4, 0x200, 0x200, 0x200,
        DYNAMIC_ENTRY.size * 4, DYNAMIC_ENTRY.size * 4, 8,
    )
    DYNAMIC_ENTRY.pack_into(image, 0x200, 1, 1)
    DYNAMIC_ENTRY.pack_into(image, 0x200 + DYNAMIC_ENTRY.size, 5, 0x300)
    DYNAMIC_ENTRY.pack_into(image, 0x200 + DYNAMIC_ENTRY.size * 2, 10, 13)
    DYNAMIC_ENTRY.pack_into(image, 0x200 + DYNAMIC_ENTRY.size * 3, 0, 0)
    image[0x300:0x30D] = b"\0libmedia.so\0"
    return bytes(image)


class ElfNeededTest(unittest.TestCase):
    def test_reads_needed_library_from_aarch64_elf(self) -> None:
        self.assertEqual(elf_needed("fixture", make_dynamic_elf()), ("libmedia.so",))

    def test_rejects_missing_dynamic_string_table(self) -> None:
        image = bytearray(make_dynamic_elf())
        DYNAMIC_ENTRY.pack_into(image, 0x200 + DYNAMIC_ENTRY.size, 0, 0)
        with self.assertRaises(ValidationError):
            elf_needed("fixture", bytes(image))


def make_graphics_archive(vkd3d_enabled: bool) -> Archive:
    stream = io.BytesIO()
    d3d12 = b"d3d12"
    with ZipFile(stream, "w") as archive:
        for profile in ("legacy", "modern-2.6"):
            for architecture in ("x86", "x64", "arm64x"):
                for filename in ("d3d11.dll", "dxgi.dll"):
                    archive.writestr(f"dxvk/{profile}/{architecture}/{filename}", filename)
        archive.writestr("vkd3d/limited-500k/x64/d3d12.dll", d3d12)
        archive.writestr("dxvk/manifest.json", json.dumps({
            "schemaVersion": 2,
            "backend": "dxvk",
            "defaultProfile": "legacy",
            "runtimes": {
                "legacy": {"version": "1.10.3", "state": "stable"},
                "modern-2.6": {
                    "version": "2.6.2",
                    "state": "adapted-game-validated-capability-gated",
                },
            },
        }))
        archive.writestr("vkd3d/manifest.json", json.dumps({
            "schemaVersion": 1,
            "profile": "limited-500k",
            "version": "2.6",
            "defaultEnabled": vkd3d_enabled,
            "files": {"x64/d3d12.dll": hashlib.sha256(d3d12).hexdigest()},
        }))
    stream.seek(0)
    return Archive(stream)


class GraphicsRuntimePolicyTest(unittest.TestCase):
    def test_accepts_default_legacy_dxvk_and_disabled_vkd3d(self) -> None:
        archive = make_graphics_archive(vkd3d_enabled=False)
        try:
            validate_graphics_runtime_payload(archive, "aarch64")
        finally:
            archive.close()

    def test_rejects_vkd3d_manifest_marked_enabled(self) -> None:
        archive = make_graphics_archive(vkd3d_enabled=True)
        try:
            with self.assertRaisesRegex(ValidationError, "VKD3D product enablement"):
                validate_graphics_runtime_payload(archive, "aarch64")
        finally:
            archive.close()


if __name__ == "__main__":
    unittest.main()
