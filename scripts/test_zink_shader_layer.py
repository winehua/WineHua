#!/usr/bin/env python3
"""Compile the actual Mesa workaround and check that used capabilities survive."""
import ctypes
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
MESA = ROOT / 'thirdparty/mesa'


class Shader(ctypes.Structure):
    _fields_ = [('words', ctypes.POINTER(ctypes.c_uint32)),
                ('num_words', ctypes.c_size_t), ('tcs_vertices_out_word', ctypes.c_uint32)]


@unittest.skipUnless(sys.platform.startswith('linux') and shutil.which('cc'), 'requires Linux cc')
class ShaderLayerTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='winehua-layer-')
        cls.addClassCleanup(cls.temp.cleanup)
        text = (MESA / 'src/gallium/drivers/zink/nir_to_spirv/nir_to_spirv.c').read_text()
        start = text.index('static void\nremove_unused_shader_layer(')
        end = text.index('\nstruct ntv_context {', start)
        function = text[start:end].replace('static void\n', 'void\n', 1)
        header = (MESA / 'src/gallium/drivers/zink/nir_to_spirv/nir_to_spirv.h').read_text()
        struct_start = header.index('struct spirv_shader {')
        struct_end = header.index('};', struct_start) + 2
        source = '#include <stddef.h>\n#include <stdint.h>\n#include <string.h>\n#include "src/compiler/spirv/spirv.h"\n'
        source += header[struct_start:struct_end] + '\n' + function
        directory = Path(cls.temp.name)
        (directory / 'check.c').write_text(source)
        subprocess.run(['cc', '-shared', '-fPIC', '-Wall', '-Wextra', '-Werror',
                        '-I', str(MESA), str(directory / 'check.c'), '-o', str(directory / 'check.so')], check=True)
        cls.library = ctypes.CDLL(str(directory / 'check.so'))
        cls.library.remove_unused_shader_layer.argtypes = [ctypes.POINTER(Shader)]

    def apply(self, instructions, offset=0):
        words = [0x07230203, 0x10600, 0, 100, 0] + instructions
        array = (ctypes.c_uint32 * len(words))(*words)
        shader = Shader(array, len(words), offset)
        self.library.remove_unused_shader_layer(ctypes.byref(shader))
        return list(array[:shader.num_words]), shader.tcs_vertices_out_word

    def test_unused_capability_removed_without_changing_other_words(self):
        instructions = [0x20011, 69, 0x20011, 1, 0x3000e, 0, 3]
        words, offset = self.apply(instructions, offset=10)
        self.assertEqual(words[5:], instructions[2:])
        self.assertEqual(offset, 8)
        self.assertEqual(self.apply(words[5:])[0], words)

    def test_layer_variable_and_member_capabilities_preserved(self):
        for decoration in ([0x40047, 7, 11, 9], [0x50048, 7, 0, 11, 9]):
            instructions = [0x20011, 69] + decoration
            self.assertEqual(self.apply(instructions)[0][5:], instructions)

    def test_other_builtins_are_not_layer(self):
        instructions = [0x20011, 69, 0x40047, 7, 11, 0]
        self.assertEqual(self.apply(instructions)[0][5:], instructions[2:])

    def test_absent_capability_and_malformed_instructions_are_untouched(self):
        for instructions in ([0x20011, 1], [0x20011, 69, 0], [0x20011, 69, 0x50048, 7]):
            self.assertEqual(self.apply(instructions)[0][5:], instructions)


if __name__ == '__main__':
    unittest.main()
