#!/usr/bin/env python3
"""Exercise the actual assembly font-policy generator and its encodings."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class FontPolicyTests(unittest.TestCase):
    def test_chinese_aliases_and_inf_encoding(self):
        source = (ROOT / 'scripts/assemble.sh').read_text(encoding='utf-8')
        anchor = "python3 - \"$wine_data/share/wine\" <<'PY'\n"
        generator = source.split(anchor, 1)[1].split('\nPY\n', 1)[0]
        fixture = ('[Version]\nSignature="$Chicago$"\n;; OHOS font substitutes\n'
                   'HKLM,%FontSubStr%,"SimSun",,"Noto Serif CJK SC"\n'
                   'HKLM,%FontSubStr%,"宋体",,"Noto Serif CJK SC"\n'
                   'HKLM,%FontSubStr%,"黑体",,"Noto Sans CJK SC"\n[MCI]\n')
        with tempfile.TemporaryDirectory(prefix='winehua-font-policy-') as directory:
            path = Path(directory)
            (path / 'wine.inf').write_text(fixture, encoding='utf-8')
            subprocess.run([sys.executable, '-', str(path)], input=generator, text=True, check=True)
            registry = (path / 'winehua-fonts.reg').read_text(encoding='utf-16')
            self.assertIn('"宋体"="Noto Serif CJK SC"', registry)
            self.assertIn('"黑体"="Noto Sans CJK SC"', registry)
            self.assertNotIn(r'\u5b8b', registry)
            self.assertTrue((path / 'wine.inf').read_bytes().startswith(b'\xef\xbb\xbf'))
            self.assertEqual((path / 'wine.inf').read_text(encoding='utf-8-sig'), fixture)


if __name__ == '__main__':
    unittest.main()
