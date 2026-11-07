"""A single passing cube or a fallback backend cannot qualify the D3D9 suite."""
import copy
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from automation.checks import evaluate


class D3d9ProfileCoverageTests(unittest.TestCase):
    def setUp(self):
        self.results = [{'testId': api+'-'+arch, 'renderer': api, 'peArchitecture': arch,
                         'd3dBackend': 'dxvk_legacy_d3d9', 'status': 'PASS', 'stage': 'present',
                         'frames': 600, 'initHresult': '0x00000000', 'presentHresult': '0x00000000'}
                        for api in ('D3D9','D3D11') for arch in ('x64','x86')]

    def verdict(self, records):
        return evaluate(['d3d9-profile-coverage'], {'summary': {'tests': records}})['status']

    def test_all_required_presentations_pass(self):
        self.assertEqual(self.verdict(self.results), 'PASS')

    def test_each_architecture_and_api_is_required(self):
        for index in range(4):
            self.assertEqual(self.verdict(self.results[:index]+self.results[index+1:]), 'FAIL')
        self.assertEqual(self.verdict(self.results[:1]), 'FAIL')
        self.assertEqual(self.verdict([]), 'FAIL')

    def test_failures_fallbacks_and_missing_frames_are_rejected(self):
        for field, value in [('status','FAIL'), ('stage','initialization'),
                             ('d3dBackend','dxvk_legacy'), ('d3dBackend','wined3d'),
                             ('frames',0), ('frames',None), ('initHresult','0x8876086a'),
                             ('presentHresult','0x80004005'), ('peArchitecture','arm64')]:
            records = copy.deepcopy(self.results)
            records[0][field] = value
            with self.subTest(field=field,value=value):
                self.assertEqual(self.verdict(records), 'FAIL')

    def test_duplicates_cannot_fill_missing_coverage(self):
        self.assertEqual(self.verdict(self.results[:-1]+[self.results[0]]), 'FAIL')
        self.assertEqual(self.verdict(self.results+[self.results[0]]), 'FAIL')

    def test_d3d11_matrix_policy_remains_strict(self):
        self.assertEqual(evaluate(['coverage'], {'summary': {'tests': self.results}})['status'], 'FAIL')


if __name__ == '__main__':
    unittest.main()
