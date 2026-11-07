"""Response-file links must use OHOS runtime libraries rather than host defaults."""
from pathlib import Path
import tempfile
import unittest
from linux_sdk_driver_args import inspect


class DriverArgumentTests(unittest.TestCase):
    def test_direct_target_and_compile_mode(self):
        self.assertEqual(inspect(['--target=x86_64-linux-ohos','object.o']),('x86_64-linux-ohos',True))
        self.assertEqual(inspect(['-target','aarch64-linux-ohos','-c','source.c']),('aarch64-linux-ohos',False))

    def test_link_and_compile_response_files_with_quoted_paths(self):
        with tempfile.TemporaryDirectory() as temporary:
            response = Path(temporary)/'link arguments.rsp'
            response.write_text('--target=x86_64-linux-ohos "folder with spaces/object.o" -l:libunwind.a')
            self.assertEqual(inspect(['@'+str(response)]),('x86_64-linux-ohos',True))
            response.write_text('-target aarch64-linux-ohos -c "source with spaces.c"')
            self.assertEqual(inspect(['@'+str(response)]),('aarch64-linux-ohos',False))

    def test_nested_response_files_and_cycles(self):
        with tempfile.TemporaryDirectory() as temporary:
            outer = Path(temporary)/'outer.rsp'
            inner = Path(temporary)/'inner.rsp'
            # Forward slashes are accepted by Clang on both build hosts.
            outer.write_text('@"'+inner.as_posix()+'"')
            inner.write_text('--target=aarch64-linux-ohos object.o')
            self.assertEqual(inspect(['@'+str(outer)]),('aarch64-linux-ohos',True))
            inner.write_text('@"'+outer.as_posix()+'"')
            with self.assertRaises(ValueError): inspect(['@'+str(outer)])

    def test_missing_response_file_is_an_error(self):
        with self.assertRaises(OSError): inspect(['@definitely-missing-winehua-response.rsp'])


if __name__ == '__main__': unittest.main()
