"""Exercise the build helper's real patch-direction checks on a fresh source."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = (ROOT / "scripts/build_wine.sh").read_text()
HELPER = SCRIPT[SCRIPT.index("ensure_wine_patch() {"):SCRIPT.index("\n}\n", SCRIPT.index("ensure_wine_patch() {")) + 3]
PATCH = """--- a/source.txt
+++ b/source.txt
@@ -1 +1 @@
-before
+after
"""


class WinePatchDetectionTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        (self.root / "change.patch").write_text(PATCH)

    def apply(self):
        return subprocess.run(["bash", "-c", "set -euo pipefail\nlog() { :; }\n" + HELPER +
            '\nWINE_SRC="$1"\nensure_wine_patch "$1/change.patch" test\n',
            "patch-test", str(self.root)], capture_output=True, text=True)

    def test_clean_source_is_patched(self):
        (self.root / "source.txt").write_text("before\n")
        result = self.apply()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual((self.root / "source.txt").read_text(), "after\n")

    def test_already_patched_source_stays_patched(self):
        (self.root / "source.txt").write_text("after\n")
        result = self.apply()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual((self.root / "source.txt").read_text(), "after\n")

    def test_conflicting_source_fails_without_modification(self):
        (self.root / "source.txt").write_text("different\n")
        self.assertNotEqual(self.apply().returncode, 0)
        self.assertEqual((self.root / "source.txt").read_text(), "different\n")


if __name__ == "__main__":
    unittest.main()
