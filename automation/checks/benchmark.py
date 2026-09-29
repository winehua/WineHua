"""Validate measured benchmark JSON against the independent raw frame records.

A PASS verifies the measurement artifact; it never asserts a performance gain.
"""
import csv
import math


def check(ctx):
    def fail(message):
        return {"status": "FAIL", "stage": "benchmark", "message": message}

    result = ctx.get("result") or {}
    metrics = result.get("benchmark") or {}
    if result.get("status") != "PASS" or metrics.get("complete") is not True or metrics.get("valid") is not True:
        return fail("benchmark incomplete or invalid")
    expected_machine = "AMD64" if ctx.get("test", {}).get("exe", "").startswith("x64-fex/") else "ARM64"
    if metrics.get("peMachine") != expected_machine:
        return fail(f"expected {expected_machine} PE, got {metrics.get('peMachine')}")
    if metrics.get("vulkanBackend") not in ("direct", "venus"):
        return fail("missing actual benchmark Vulkan backend")
    try:
        path = ctx["run_dir"] / "device-results" / f"{ctx['test_id']}.json.frames.csv"
        with path.open(newline="", encoding="utf8") as stream:
            rows = list(csv.DictReader(stream))
        frame_ms = [float(row["frameMs"]) for row in rows]
        present_ms = [float(row["presentMs"]) for row in rows]
        if len(rows) < 60 or len(rows) != metrics["measuredFrames"]:
            return fail("missing or inconsistent raw frame count")
        if any(int(row["frame"]) != i + 1 for i, row in enumerate(rows)):
            return fail("raw frame sequence has gaps")
        if any(not math.isfinite(frame) or not math.isfinite(present) or frame <= 0 or present < 0 or present > frame + .001
               for frame, present in zip(frame_ms, present_ms)):
            return fail("invalid raw frame/present timing")
        actual_ms = sum(frame_ms)
        if metrics["configuredRunMs"] <= metrics["warmupMs"] or actual_ms < metrics["configuredRunMs"] - metrics["warmupMs"] - 1000:
            return fail("measured interval ended early")
        expected = {"measuredMs": actual_ms, "averageFps": len(rows) * 1000 / actual_ms,
                    "presentMeanMs": sum(present_ms) / len(rows),
                    "over16_67Ms": sum(value > 1000 / 60 for value in frame_ms),
                    "over33_33Ms": sum(value > 1000 / 30 for value in frame_ms)}
        for samples, prefix, fractions in [(frame_ms, "frameMs", (.50, .95, .99)),
                                           (present_ms, "presentMs", (.95, .99))]:
            samples = sorted(samples)
            for fraction in fractions:
                expected[f"{prefix}P{int(fraction * 100)}"] = samples[math.ceil(len(rows) * fraction) - 1]
        if metrics.get("cpuAvailable") is True:
            cpu = metrics["cpuMs"]
            if not isinstance(cpu, (int, float)) or not math.isfinite(cpu) or cpu <= 0:
                return fail("CPU measurement is unavailable or zero despite cpuAvailable=true")
            expected["cpuMsPerFrame"] = cpu / len(rows)
        elif metrics.get("cpuMs") is not None or metrics.get("cpuMsPerFrame") is not None:
            return fail("unavailable CPU must be null")
        for name, value in expected.items():
            reported = metrics[name]
            if not isinstance(reported, (int, float)) or not math.isfinite(reported) or not math.isclose(reported, value, rel_tol=1e-6, abs_tol=.001):
                return fail(f"JSON/raw records disagree for {name}")
        if ctx.get("test", {}).get("benchmarkMonitor"):
            host_path = ctx["run_dir"] / "device-results" / f"{ctx['test_id']}.json.hap-performance.json"
            import json
            host = json.loads(host_path.read_text(encoding="utf8"))
            begin, end = host["start"], host["end"]
            for snapshot in (begin, end):
                for name in ("pid", "monotonicNs", "processCpuNs", "directPresents", "directGamePresents", "eglPresents", "eglGpuPresents"):
                    if not isinstance(snapshot[name], (int, float)) or not math.isfinite(snapshot[name]) or snapshot[name] < 0:
                        return fail(f"invalid HAP snapshot {name}")
            if host["runId"] != result["runId"] or host["testId"] != ctx["test_id"] or begin["pid"] != end["pid"]:
                return fail("HAP measurement identity changed")
            if not begin["monotonicAvailable"] or not end["monotonicAvailable"]:
                return fail("HAP monotonic clock unavailable")
            host_ms = (end["monotonicNs"] - begin["monotonicNs"]) / 1e6
            if host_ms <= 0 or abs(host_ms - actual_ms) > 2 * host["pollIntervalMs"] + 200:
                return fail("HAP measurement window does not match game steady-state interval")
            direct = metrics["vulkanBackend"] == "direct"
            if begin["directActive"] is not direct or end["directActive"] is not direct:
                return fail("actual HAP compositor does not match Direct/Venus request")
            if direct:
                output_frames = end["directGamePresents"] - begin["directGamePresents"]
                if output_frames <= 0:
                    return fail("game reported frames but HAP did not accept Direct presents")
                metrics["hapAcceptedGamePresentFps"] = output_frames * 1000 / host_ms
            else:
                output_frames = end["eglGpuPresents"] - begin["eglGpuPresents"]
                if output_frames <= 0:
                    return fail("Venus reported frames but HAP did not accept fresh GPU presents")
                metrics["hapAcceptedGamePresentFps"] = output_frames * 1000 / host_ms
            if begin["cpuAvailable"] and end["cpuAvailable"]:
                host_cpu_ms = (end["processCpuNs"] - begin["processCpuNs"]) / 1e6
                if host_cpu_ms < 0:
                    return fail("HAP CPU clock regressed")
                metrics["hapCpuMsPerSecond"] = host_cpu_ms * 1000 / host_ms
                metrics["estimatedHapCpuMsPerGameFrame"] = host_cpu_ms / host_ms * actual_ms / len(rows)
            metrics["hapObservationMs"] = host_ms
    except (OSError, KeyError, ValueError, TypeError, ZeroDivisionError) as error:
        return fail(f"benchmark artifact error: {error}")
    return {"status": "PASS", "stage": "benchmark",
            "message": f"{len(rows)} steady-state frames; measurement valid, no speedup assertion", "metrics": metrics}
