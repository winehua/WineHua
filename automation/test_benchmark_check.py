import copy
import csv
import json
from pathlib import Path
import tempfile
import unittest

from automation.checks.benchmark import check


class BenchmarkCheckTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        (self.root / "device-results").mkdir()
        self.csv = self.root / "device-results/benchmark.json.frames.csv"
        with self.csv.open("w", newline="") as stream:
            writer = csv.writer(stream)
            writer.writerow(["frame", "frameMs", "presentMs"])
            for frame in range(1, 101):
                writer.writerow([frame, 20, 3])
        self.ctx = {"run_dir": self.root, "test_id": "benchmark", "test": {"exe": "x64-fex/benchmark.exe"},
                    "result": {"status": "PASS", "benchmark": {
                        "complete": True, "valid": True, "peMachine": "AMD64", "vulkanBackend": "direct",
                        "warmupMs": 1000, "configuredRunMs": 3000, "measuredFrames": 100,
                        "measuredMs": 2000, "averageFps": 50, "frameMsP50": 20, "frameMsP95": 20,
                        "frameMsP99": 20, "over16_67Ms": 100, "over33_33Ms": 0,
                        "presentMeanMs": 3, "presentMsP95": 3, "presentMsP99": 3,
                        "cpuAvailable": True, "cpuMs": 500, "cpuMsPerFrame": 5}}}

    def test_valid_artifact(self):
        self.assertEqual(check(self.ctx)["status"], "PASS")

    def test_corrupted_or_incomplete_metrics(self):
        for name, value in [("averageFps", 60), ("frameMsP99", float("nan")),
                            ("measuredFrames", 101), ("configuredRunMs", 6000),
                            ("peMachine", "ARM64"), ("complete", False), ("cpuMs", 0)]:
            with self.subTest(name=name):
                ctx = copy.deepcopy(self.ctx)
                ctx["result"]["benchmark"][name] = value
                self.assertEqual(check(ctx)["status"], "FAIL")

    def test_missing_raw_frames(self):
        self.csv.unlink()
        self.assertEqual(check(self.ctx)["status"], "FAIL")

    def test_cpu_unavailable_is_not_zero(self):
        metrics = self.ctx["result"]["benchmark"]
        metrics.update(cpuAvailable=False, cpuMs=None, cpuMsPerFrame=None)
        self.assertEqual(check(self.ctx)["status"], "PASS")
        metrics["cpuMs"] = 0
        self.assertEqual(check(self.ctx)["status"], "FAIL")

    def test_hap_window_and_actual_output(self):
        self.ctx["test"]["benchmarkMonitor"] = True
        self.ctx["result"]["runId"] = "unit-fixture"
        begin = {"pid": 100, "monotonicAvailable": True, "cpuAvailable": True,
                 "monotonicNs": 1000000000, "processCpuNs": 100000000,
                 "directActive": True, "directPresents": 10, "directGamePresents": 10,
                 "eglPresents": 0, "eglGpuPresents": 0}
        end = {**begin, "monotonicNs": 3000000000, "processCpuNs": 300000000,
               "directPresents": 110, "directGamePresents": 110}
        host = {"runId": "unit-fixture", "testId": "benchmark", "start": begin, "end": end, "pollIntervalMs": 500}
        path = self.root / "device-results/benchmark.json.hap-performance.json"
        def write(document):
            path.write_text(json.dumps(document), encoding="utf8")
        write(host)
        self.assertEqual(check(self.ctx)["status"], "PASS")
        for key, value in [("pid", 101), ("directGamePresents", 10), ("directActive", False),
                           ("processCpuNs", float("nan")), ("monotonicNs", 6000000000)]:
            with self.subTest(key=key):
                changed = copy.deepcopy(host)
                changed["end"][key] = value
                write(changed)
                self.assertEqual(check(self.ctx)["status"], "FAIL")


if __name__ == "__main__":
    unittest.main()
