#!/usr/bin/env python3
"""Regenerate the embedded D2 diagnostic compute shader with glslangValidator."""

from pathlib import Path
import struct
import subprocess
import tempfile


root = Path(__file__).resolve().parents[1]
source = root / "entry/src/main/cpp/direct/direct_sample.comp"
header = source.with_name("direct_sample_spv.h")
with tempfile.TemporaryDirectory() as directory:
    binary = Path(directory) / "direct_sample.spv"
    subprocess.run(
        ["glslangValidator", "-V", "-S", "comp", "-o", str(binary), str(source)],
        check=True,
    )
    data = binary.read_bytes()
words = struct.unpack(f"<{len(data) // 4}I", data)
lines = [
    "// Generated from direct_sample.comp by scripts/generate_direct_sample_shader.py.",
    "#pragma once",
    "#include <cstdint>",
    "namespace winehua::direct {",
    "constexpr uint32_t kDirectSampleSpv[] = {",
]
for offset in range(0, len(words), 8):
    lines.append("    " + ", ".join(f"0x{word:08x}" for word in words[offset:offset + 8]) + ",")
lines += ["};", "} // namespace winehua::direct", ""]
header.write_text("\n".join(lines), encoding="utf-8")
