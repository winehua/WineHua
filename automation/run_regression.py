#!/usr/bin/env python3
"""WineHua regression suite runner (WSL).

Builds the HAP in the canonical Docker container, validates the payload,
installs it through hdc, starts the App in smoke mode with Want parameters,
validates the deterministic fixed frames with validate_frame.py, and archives
all machine-readable results under the archive root.

The repository root is derived from this script's location, the hdc path comes
from WINEHUA_HDC or PATH, and the archive root comes from --archive-root,
WINEHUA_ARCHIVE_ROOT, or the repository build/ directory. The device is selected automatically from
`hdc list targets` (physical targets preferred) or via --device-id.

The D3D11 coverage policy is shared with the guest winehua_d3d11_smoke suite;
see docs/PHASE2_DXVK_STATUS_MEMO.md for the product baseline. The capability
matrix and VKD3D Gate-A suites were removed with the smoke rebuild
(docs/SMOKE_REBUILD_20260831.md) because their host-vulkan probe entry points
no longer exist in entry.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import time
from datetime import datetime
from pathlib import Path

try:
    from validate_frame import validate_d3d11_cube, validate_rgba_quadrants
    FRAME_VALIDATOR_IMPORT_ERROR = None
except ModuleNotFoundError as error:
    validate_d3d11_cube = None
    validate_rgba_quadrants = None
    FRAME_VALIDATOR_IMPORT_ERROR = error

REPO_ROOT = Path(__file__).resolve().parent.parent
BUNDLE = "app.hackeris.winehua"
ABILITY = "EntryAbility"
HAP_PATH = REPO_ROOT / "entry/build/default/outputs/default/entry-default-signed.hap"
RAWFILE_ZIP = REPO_ROOT / "entry/src/main/resources/rawfile/wine-data.zip"
DEVICE_SANDBOX = f"/data/app/el2/100/base/{BUNDLE}"
DOCKER_CONTAINER = "winehua-master-ext4"
DOCKER_REPO = "/data/src/winehua"

# 产品性能 profile (见 STATUS_MEMO)。smoke 会话固定出厂基线 — 不走 Want,
# 诊断档按 need 在 suites.json env 声明。
PRODUCT_PERF_PROFILE = "shadow-precise-dirty-ring-inline-upload-coverage-sort"

SUITES = (
    "core", "opengl", "audio", "d3d8", "d3d9",
    "wine-vulkan", "wine-vulkan-present",
    "dxvk", "dxvk-long", "dxvk-dynamic",
    "dxvk-modern-baseline", "dxvk-modern-long",
    "gpu-diagnostics", "dxvk26-requirements", "d3d12",
    "all", "long",
)

REQUIRED_PAYLOAD = (
    "smoke/manifest.json",
    "smoke/suites.json",
    "smoke/x64/winehua_audio_smoke.exe", "smoke/x86/winehua_audio_smoke.exe",
    "smoke/x64/winehua_graphics_smoke.exe", "smoke/x86/winehua_graphics_smoke.exe",
    "smoke/x64/winehua_vulkan_smoke.exe", "smoke/x86/winehua_vulkan_smoke.exe",
    "smoke/x64/winehua_d3d8_smoke.exe", "smoke/x86/winehua_d3d8_smoke.exe",
    "smoke/x64/winehua_d3d_switch_cube.exe", "smoke/x86/winehua_d3d_switch_cube.exe",
    "smoke/x64/winehua_d3d11_smoke.exe", "smoke/x86/winehua_d3d11_smoke.exe",
    "smoke/x64/winehua_gpu_diagnostics.exe", "smoke/x86/winehua_gpu_diagnostics.exe",
    "smoke/x64/winehua_dxvk26_requirements.exe", "smoke/x86/winehua_dxvk26_requirements.exe",
    "smoke/x64/winehua_d3d12_smoke.exe",
    "smoke/x64/triangle.exe", "smoke/x64/gears.exe",
    "dxvk/manifest.json",
    "dxvk/legacy/x64/d3d11.dll", "dxvk/legacy/x64/dxgi.dll",
    "dxvk/legacy/x86/d3d11.dll", "dxvk/legacy/x86/dxgi.dll",
    "dxvk/modern-2.6/x64/d3d11.dll", "dxvk/modern-2.6/x64/dxgi.dll",
    "dxvk/modern-2.6/x86/d3d11.dll", "dxvk/modern-2.6/x86/dxgi.dll",
    "bin/guest_vulkan/lib/libvulkan.so.1",
    "bin/guest_vulkan/lib/libvulkan_virtio.so",
    "bin/guest_vulkan/share/vulkan/icd.d/venus_icd.x86_64.json",
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="WineHua regression suite runner (WSL)")
    parser.add_argument("--suite", choices=SUITES, default="core")
    parser.add_argument("--prefix", choices=("reuse", "clean"), default="reuse")
    parser.add_argument("--runs", type=int, default=1)
    parser.add_argument("--long-seconds", type=int, default=3600,
                        help="wall-clock target for the dxvk-long suite")
    parser.add_argument("--gate", action="store_true",
                        help="Phase-2 entry gate: three reuse-prefix core runs and one clean-prefix core run")
    parser.add_argument("--skip-build", action="store_true")
    parser.add_argument("--device-id", default="",
                        help="hdc target; auto-selected (physical preferred) when empty")
    parser.add_argument("--archive-root", default="",
                        help="result archive root; WINEHUA_ARCHIVE_ROOT or "
                             "<repo>/build/automation-logs when empty")
    parser.add_argument("--timeout-minutes", type=int, default=15)
    return parser.parse_args()


def resolve_hdc() -> str:
    env_hdc = os.environ.get("WINEHUA_HDC")
    if env_hdc:
        path = Path(env_hdc)
        if not path.is_file():
            raise SystemExit(f"WINEHUA_HDC points to a missing file: {env_hdc}")
        return str(path)
    found = shutil.which("hdc")
    if found:
        return found
    raise SystemExit("hdc not found: set WINEHUA_HDC or add the hdc tool to PATH")


def run_hdc(hdc: str, device_id: str, *args: str) -> tuple[int, str]:
    result = subprocess.run(
        [hdc, "-t", device_id, *args],
        capture_output=True, text=True, errors="replace")
    return result.returncode, result.stdout


def hdc_local_path(path: Path) -> str:
    """Return a path usable by Windows HDC when the runner is in WSL."""
    resolved = path.resolve()
    value = str(resolved)
    match = re.fullmatch(r"/mnt/([A-Za-z])(?:/(.*))?", value)
    if match:
        suffix = (match.group(2) or "").replace("/", chr(92))
        return f"{match.group(1).upper()}:{chr(92)}{suffix}"
    if value.startswith("/home/"):
        return (chr(92) * 2 + "wsl.localhost" + chr(92) + "Ubuntu" +
                value.replace("/", chr(92)))
    return value


def run_hdc_install(hdc: str, device_id: str, hap_path: Path) -> tuple[int, str]:
    """Install through Windows PowerShell to preserve a WSL UNC path verbatim."""
    def ps_quote(value: str) -> str:
        return "'" + value.replace("'", "''") + "'"

    command = (
        f"& {ps_quote(hdc_local_path(Path(hdc)))} -t {ps_quote(device_id)} "
        f"install -r {ps_quote(hdc_local_path(hap_path))}")
    result = subprocess.run(
        ["powershell.exe", "-NoProfile", "-NonInteractive", "-Command", command],
        capture_output=True, text=True, errors="replace")
    return result.returncode, result.stdout


def run_hdc_windows(hdc: str, device_id: str, *args: str) -> tuple[int, str]:
    """Run an HDC command through Windows without collapsing its argv."""
    def ps_quote(value: str) -> str:
        return "'" + value.replace("'", "''") + "'"

    command = " ".join([
        f"& {ps_quote(hdc_local_path(Path(hdc)))}",
        "-t", ps_quote(device_id),
        *(ps_quote(arg) for arg in args),
    ])
    result = subprocess.run(
        ["powershell.exe", "-NoProfile", "-NonInteractive", "-Command", command],
        capture_output=True, text=True, errors="replace")
    return result.returncode, result.stdout


def get_device_text(hdc: str, device_id: str, remote_path: str) -> str:
    """Return the first balanced { ... } span from a remote file, or ''."""
    code, text = run_hdc(hdc, device_id, "shell", "cat", remote_path)
    if code != 0:
        return ""
    start = text.find("{")
    end = text.rfind("}")
    if start < 0 or end < start:
        return ""
    return text[start:end + 1].strip()


def save_device_file(hdc: str, device_id: str, remote_path: str, local_path: Path) -> None:
    local_path.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(
        [hdc, "-t", device_id, "file", "recv", remote_path,
         hdc_local_path(local_path)],
        capture_output=True, text=True)


def save_probe_results(hdc: str, device_id: str, run_directory: Path,
                       remote_directory: str, summary: dict) -> None:
    """Archive every probe result without treating HDC directory recv as evidence."""
    output = run_directory / "device-results"
    output.mkdir(parents=True, exist_ok=True)
    for test in summary.get("tests", []):
        test_id = str(test.get("testId", ""))
        if not test_id:
            continue
        text = get_device_text(hdc, device_id, f"{remote_directory}/{test_id}.json")
        target = output / f"{test_id}.json"
        if text:
            target.write_text(text + "\n", encoding="utf-8")
            continue
        # A crash often leaves no per-probe file. Preserve the exact
        # device-supplied suite entry so absence cannot be mistaken for PASS.
        archived = dict(test)
        archived["artifactSource"] = "device-suite-summary"
        archived["perProbeArtifact"] = "MISSING"
        write_json(target, archived)


def write_json(path: Path, payload: object) -> None:
    path.write_text(json.dumps(payload, sort_keys=True, indent=2) + "\n", encoding="utf-8")


def capture_frame(hdc: str, device_id: str, run_directory: Path, run_id: str,
                  test_id: str, validator) -> bool:
    if validator is None:
        raise RuntimeError(f"Frame validator unavailable: {FRAME_VALIDATOR_IMPORT_ERROR}")
    remote_image = f"/data/local/tmp/winehua-{run_id}-{test_id}.jpeg"
    local_image = run_directory / f"{test_id}.jpeg"
    visual_json = run_directory / f"{test_id}-visual.json"
    run_hdc(hdc, device_id, "shell", "snapshot_display", "-f", remote_image)
    save_device_file(hdc, device_id, remote_image, local_image)
    run_hdc(hdc, device_id, "shell", "rm", remote_image)
    report = validator(local_image)
    write_json(visual_json, report)
    return report["status"] == "PASS"


def capture_d3d11_frame(hdc: str, device_id: str, run_directory: Path,
                        run_id: str, test_id: str) -> bool:
    """snapshot + validate the D3D11 cube with up to four attempts."""
    if validate_d3d11_cube is None:
        raise RuntimeError(f"D3D11 frame validator unavailable: {FRAME_VALIDATOR_IMPORT_ERROR}")
    last_json = None
    for attempt in range(4):
        if attempt > 0:
            time.sleep(0.75)
        remote_image = f"/data/local/tmp/winehua-{run_id}-{test_id}.jpeg"
        local_image = run_directory / f"{test_id}.jpeg"
        visual_json = run_directory / f"{test_id}-visual.json"
        run_hdc(hdc, device_id, "shell", "snapshot_display", "-f", remote_image)
        save_device_file(hdc, device_id, remote_image, local_image)
        run_hdc(hdc, device_id, "shell", "rm", remote_image)
        attempt_json = visual_json.with_name(f"{test_id}-visual.json.attempt{attempt}")
        last_json = attempt_json
        report = validate_d3d11_cube(local_image)
        write_json(attempt_json, report)
        if report["status"] == "PASS":
            shutil.copyfile(attempt_json, visual_json)
            return True
    if last_json and last_json.is_file():
        shutil.copyfile(last_json, visual_json)
    return False


def invoke_build(log_path: Path) -> None:
    inspect = subprocess.run(
        ["docker", "inspect", DOCKER_CONTAINER],
        capture_output=True, text=True, errors="replace")
    if inspect.returncode != 0:
        raise RuntimeError(f"Docker container is unavailable: {DOCKER_CONTAINER}")
    running = subprocess.run(
        ["docker", "inspect", "--format", "{{.State.Running}}", DOCKER_CONTAINER],
        capture_output=True, text=True, errors="replace")
    if running.returncode != 0:
        raise RuntimeError(f"Cannot query Docker container: {DOCKER_CONTAINER}")
    if running.stdout.strip() != "true":
        started = subprocess.run(["docker", "start", DOCKER_CONTAINER],
                                 capture_output=True, text=True, errors="replace")
        if started.returncode != 0:
            raise RuntimeError(f"Cannot start Docker container: {DOCKER_CONTAINER}")
    result = subprocess.run(
        ["docker", "exec", "-w", DOCKER_REPO, DOCKER_CONTAINER,
         "make", "hap", "NATIVE_ARCH=arm64-v8a"],
        capture_output=True, text=True, errors="replace")
    redacted = [
        re.sub(r"(-keyPwd\s+)\S+", r"\1<redacted>",
               re.sub(r"(-keystorePwd\s+)\S+", r"\1<redacted>", line))
        for line in (result.stdout + result.stderr).splitlines()
    ]
    log_path.write_text("\n".join(redacted) + "\n", encoding="utf-8")
    if result.returncode != 0:
        raise RuntimeError(f"Build failed; see {log_path}")


def sh_capture(*command: str) -> str:
    result = subprocess.run(list(command), capture_output=True, text=True, errors="replace")
    return result.stdout.strip()


def sh_capture_shell(script: str) -> str:
    result = subprocess.run(["bash", "-lc", script], capture_output=True, text=True, errors="replace")
    return result.stdout.strip()


def get_artifact_metadata(output_directory: Path) -> dict:
    if not HAP_PATH.is_file():
        raise RuntimeError("Signed HAP does not exist")
    stat = sh_capture("stat", "-c", "%y %s", str(HAP_PATH))
    hap_hash = sh_capture("sha256sum", str(HAP_PATH)).split()[0]
    raw_hash = sh_capture("sha256sum", str(RAWFILE_ZIP)).split()[0]
    embedded_hash = sh_capture_shell(
        f"unzip -p '{HAP_PATH}' resources/rawfile/wine-data.zip | sha256sum").split()[0]
    if raw_hash != embedded_hash:
        raise RuntimeError("HAP embedded wine-data.zip hash does not match assembled payload")

    smoke_list = sh_capture("unzip", "-l", str(RAWFILE_ZIP))
    for required in REQUIRED_PAYLOAD:
        if required not in smoke_list:
            raise RuntimeError(f"Payload missing {required}")

    guest_arch = sh_capture_shell(
        f"unzip -p '{RAWFILE_ZIP}' bin/guest_gfx/lib/libEGL.so.1 | file -")
    host_arch = sh_capture_shell(
        f"unzip -p '{HAP_PATH}' libs/arm64-v8a/libentry.so | file -")
    if "x86-64" not in guest_arch:
        raise RuntimeError(f"Guest EGL architecture invalid: {guest_arch}")
    if "ARM aarch64" not in host_arch:
        raise RuntimeError(f"Host libentry architecture invalid: {host_arch}")

    main_commit = sh_capture("git", "-C", str(REPO_ROOT), "rev-parse", "HEAD")
    submodules = sh_capture("git", "-C", str(REPO_ROOT), "submodule", "status", "--recursive")
    dirty = sh_capture("git", "-C", str(REPO_ROOT), "status", "--short")

    metadata = {
        "schemaVersion": 1,
        "hap": str(HAP_PATH),
        "hapTimestampAndSize": stat,
        "hapSha256": hap_hash,
        "rawfileSha256": raw_hash,
        "mainCommit": main_commit,
        "submodules": submodules.splitlines(),
        "dirtySummary": dirty.splitlines() if dirty else [],
        "guestArchitecture": guest_arch,
        "hostArchitecture": host_arch,
    }
    write_json(output_directory / "artifact.json", metadata)
    return metadata


def get_d3d11_coverage(summary: dict, run_suite: str, long_seconds: int) -> dict:
    entries = []
    for test in summary.get("tests", []):
        metrics = test.get("metrics")
        # Visual examples (for example dxvk-cube-x64) prove device creation
        # and presentation but intentionally do not duplicate the exhaustive
        # feature metrics emitted by winehua_d3d11_smoke.
        if not metrics or metrics.get("rgba8SampleMatrix") is None:
            continue
        matrix = metrics.get("rgba8SampleMatrix", {})
        descriptor = metrics.get("descriptorMatrix", {})
        subresource = metrics.get("subresourceMatrix", {})
        texture3d = metrics.get("texture3dMatrix", {})
        heaven = metrics.get("heavenResourceMatrix", {})

        checks = {
            "featureLevel11": metrics.get("featureLevel") == "11.0",
            "shaderModel5": bool(metrics.get("shaderModel5")),
            "cubeGeometry": bool(metrics.get("cubeGeometry")),
            "drawIndexedInstanced": bool(metrics.get("drawIndexedInstanced")),
            "depthStencil": bool(metrics.get("depthStencil")),
            "alphaBlend": bool(metrics.get("alphaBlend")),
            "rasterizerState": bool(metrics.get("rasterizerState")),
            "constantBuffer": bool(metrics.get("constantBuffer")),
            "dynamicConstantBuffer": (run_suite != "dxvk-dynamic" or bool(metrics.get("dynamicConstantBuffer"))),
            "dynamicConstantReadback": (run_suite != "dxvk-dynamic" or bool(metrics.get("dynamicConstantReadback"))),
            "textureUpdate": bool(metrics.get("textureUpdate")),
            "textureUploadReadback": bool(metrics.get("textureUploadReadback")),
            "textureSamplingFunctional": bool(metrics.get("textureSampling")),
            "rgba8LoadPs": bool(matrix.get("loadPs", {}).get("pass")),
            "rgba8LoadCs": bool(matrix.get("loadCs", {}).get("pass")),
            "rgba8PointPs": bool(matrix.get("pointPs", {}).get("pass")),
            "rgba8PointCs": bool(matrix.get("pointCs", {}).get("pass")),
            "rgba8LinearPs": bool(matrix.get("linearPs", {}).get("pass")),
            "rgba8LinearCs": bool(matrix.get("linearCs", {}).get("pass")),
            "rgba8UpdatedUpload": bool(matrix.get("updated", {}).get("uploadPass")),
            "rgba8UpdatedLoadPs": bool(matrix.get("updated", {}).get("loadPs", {}).get("pass")),
            "rgba8UpdatedLoadCs": bool(matrix.get("updated", {}).get("loadCs", {}).get("pass")),
            "rgba8UpdatedPointPs": bool(matrix.get("updated", {}).get("pointPs", {}).get("pass")),
            "rgba8UpdatedPointCs": bool(matrix.get("updated", {}).get("pointCs", {}).get("pass")),
            "descriptorIdentity": bool(descriptor.get("initial", {}).get("pass")),
            "descriptorRebindDirtyState": bool(descriptor.get("rebind", {}).get("pass")),
            "descriptorUnbound": bool(descriptor.get("unbound", {}).get("pass")),
            "descriptorLifetime": bool(descriptor.get("lifetime", {}).get("pass")),
            "subresourceArrayLayers": bool(subresource.get("arrayLayers")),
            "subresourceMipLevels": bool(subresource.get("mipLevels")),
            "subresourceExplicitLod": bool(subresource.get("explicitLod")),
            "subresourceBarrierUpdate": bool(subresource.get("barrierUpdate")),
            "subresourceMatrix": bool(subresource.get("pass")),
            "texture3dCreated": bool(texture3d.get("created")),
            "texture3dUpload": bool(texture3d.get("upload")),
            "texture3dSingleDispatch": bool(texture3d.get("singleDispatch")),
            "texture3dUavToSrvBarrier": bool(texture3d.get("uavToSrvBarrier")),
            "texture3dPingPong": bool(texture3d.get("pingPong")),
            "heavenCubeMatrix": bool(heaven.get("cube", {}).get("pass")),
            "heavenTexture3dR8": bool(heaven.get("texture3d", {}).get("r8", {}).get("pass")),
            "heavenTexture3dRg8": bool(heaven.get("texture3d", {}).get("rg8", {}).get("pass")),
            "heavenD32DepthComparison": bool(heaven.get("depthComparisonSampler", {}).get("pass")),
            "heavenD24S8DepthComparison": bool(heaven.get("d24s8DepthComparisonSampler", {}).get("pass")),
            "heavenD24S8ExtendedMatrix": bool(heaven.get("d24s8ExtendedMatrix", {}).get("pass")),
            "heavenResourceMatrix": bool(heaven.get("pass")),
            "bcTextureCreated": metrics.get("bcTextureTest") == "created_sampled",
            "bcSamplingSubmitted": bool(metrics.get("bcSamplingSubmitted")),
            "bcSamplingFunctional": bool(metrics.get("bcSamplingFunctional")),
            "offscreenRenderTarget": bool(metrics.get("offscreenRenderTarget")),
            "msaa4xSupported": bool(metrics.get("msaa4xSupported")),
            "msaaResolveFunctional": bool(metrics.get("msaaResolveFunctional")),
            "computeShaderDispatch": bool(metrics.get("computeShaderDispatch")),
            "computeUavSubmitted": bool(metrics.get("computeUavSubmitted")),
            "computeUavFunctional": bool(metrics.get("computeUavFunctional")),
            "computeSampledImageFunctional": bool(metrics.get("computeSampledImageFunctional")),
            "longWallClock": (run_suite not in ("dxvk-long", "dxvk-modern-long") or
                              int(metrics.get("durationMs", 0)) >= long_seconds * 1000 - 2000),
            "present60Frames": int(metrics.get("presentFrames", 0)) >= 60,
            "presentResultSuccess": int(metrics.get("presentResult", -1)) == 0,
            "cpuFullFrameReadbackZero": int(metrics.get("cpuReadBytes", -1)) == 0,
            "cpuFullFrameUploadZero": int(metrics.get("cpuUploadBytes", -1)) == 0,
            "perFrameDeviceWaitIdleZero": int(metrics.get("perFrameDeviceWaitIdle", -1)) == 0,
            "noFallback": not bool(metrics.get("fallbackDetected")),
        }
        missing = sorted(key for key, value in checks.items() if not value)
        submitted_only = []
        if metrics.get("bcSamplingSubmitted") and not metrics.get("bcSamplingFunctional"):
            submitted_only.append("bcSampling")
        if metrics.get("computeUavSubmitted") and not metrics.get("computeUavFunctional"):
            submitted_only.append("computeUav")
        entries.append({
            "testId": test.get("testId"),
            "appStatus": test.get("status"),
            "requiredPass": not missing,
            "missingRequired": missing,
            "submittedOnly": submitted_only,
            "optionalDiagnostics": {
                "stencilQueryEnabled": bool(metrics.get("stencilQueryEnabled")),
                "stencilPixelFunctional": bool(metrics.get("stencilPixelFunctional")),
                "stencilQueryFunctional": bool(metrics.get("stencilFunctional")),
            },
            "metrics": {
                "presentFrames": int(metrics.get("presentFrames", 0)),
                "queueSubmitCount": int(metrics.get("queueSubmitCount", 0)),
                "featureProbeReadBytes": int(metrics.get("featureProbeReadBytes", 0)),
                "featureProbeGpuCopies": int(metrics.get("featureProbeGpuCopies", 0)),
                "durationMs": int(metrics.get("durationMs", 0)),
                "rgba8SampleMatrix": matrix,
                "descriptorMatrix": descriptor,
                "subresourceMatrix": subresource,
                "heavenResourceMatrix": heaven,
                "cpuReadBytes": int(metrics.get("cpuReadBytes", 0)),
                "cpuUploadBytes": int(metrics.get("cpuUploadBytes", 0)),
            },
        })
    required_pass = bool(entries) and all(entry["requiredPass"] for entry in entries)
    return {
        "schemaVersion": 1,
        "suite": run_suite,
        "status": "PASS" if required_pass else "FAIL",
        "tests": entries,
        "policy": ("required API/object/RGBA8 Load-POINT-LINEAR PS-CS/descriptor/subresource "
                   "array-mip-explicit-LOD-update/texture sampling/D24S8 "
                   "2D-array-per-view-cube-cube-array-linear-border/present/readback coverage; "
                   "ordinary R32_FLOAT comparison, optional MSAA resolve, and stencil query "
                   "are reported separately"),
    }


def dxvk_tests_for_suite(run_suite: str) -> list[str]:
    if run_suite == "d3d9":
        return ["d3d9-cube-x86", "d3d9-cube-x64"]
    if run_suite == "dxvk-dynamic":
        return ["dxvk-dynamic-cb-x86", "dxvk-dynamic-cb-x64"]
    if run_suite == "dxvk-long":
        return ["dxvk-long-x64"]
    if run_suite == "dxvk-modern-baseline":
        return ["dxvk-modern-baseline-x86", "dxvk-modern-baseline-x64",
                "dxvk-modern-cube-x64"]
    if run_suite == "dxvk-modern-long":
        return ["dxvk-modern-long-x64"]
    if run_suite in ("gpu-diagnostics", "dxvk26-requirements"):
        return []
    if run_suite == "all":
        return ["d3d9-cube-x86", "d3d9-cube-x64", "dxvk-legacy-x64", "dxvk-legacy-x86"]
    return ["dxvk-legacy-x64", "dxvk-legacy-x86"]


def invoke_one_run(hdc: str, device_id: str, run_suite: str, run_prefix: str,
                   run_id: str, root_directory: Path, perf_profile: str,
                   long_seconds: int, timeout_minutes: int,
                   capture_visuals: bool = True) -> bool:
    run_directory = root_directory / run_id
    run_directory.mkdir(parents=True, exist_ok=True)
    # SmokeRunner 聚合结果写到用户 prefix (复用 .wine) 的
    # drive_c/smoke/results/<run_id>/suite-summary.json; clean 语义由设备端
    # resetSmokePrefix 编排 (停→清→重启), host 脚本只传 winehua.prefix 请求,
    # 不在沙箱外删除应用文件。
    remote_results = f"{DEVICE_SANDBOX}/files/.wine/drive_c/smoke/results/{run_id}"
    remote_stable = f"{remote_results}/suite-summary.json"

    run_hdc(hdc, device_id, "shell", "aa", "force-stop", BUNDLE)
    # HDC shell cannot remove application-owned sandbox files. EntryAbility
    # performs and verifies the clean-prefix reset under the App UID before
    # starting Wayland, wineserver or Wine.
    run_hdc(hdc, device_id, "shell", "power-shell", "wakeup")
    run_hdc(hdc, device_id, "shell", "hilog", "-x")
    start_args = (
        "shell", "aa", "start", "-a", ABILITY, "-b", BUNDLE,
        "--ps", "winehua.mode", "smoke",
        "--ps", "winehua.suite", run_suite,
        "--ps", "winehua.run_id", run_id,
        "--ps", "winehua.prefix", run_prefix,
        "--ps", "winehua.long_seconds", str(long_seconds),
    )
    code, start_output = run_hdc_windows(hdc, device_id, *start_args)
    if "10106102" in start_output:
        # Devices without a credential can be dismissed with one deterministic
        # swipe.  A credential-protected lock remains an infrastructure error.
        run_hdc(hdc, device_id, "shell",
                "uitest uiInput swipe 1280 1350 1280 300 1200")
        code, start_output = run_hdc_windows(hdc, device_id, *start_args)
    (run_directory / "start.log").write_text(start_output, encoding="utf-8")
    if code != 0 or "start ability successfully" not in start_output:
        raise RuntimeError(f"Want start failed: {start_output.strip()}")

    run_timeout_minutes = timeout_minutes
    if run_suite in ("dxvk-long", "dxvk-modern-long"):
        run_timeout_minutes = max(timeout_minutes, (long_seconds + 300) // 60 + 5)
    deadline = time.monotonic() + run_timeout_minutes * 60
    captured: dict[str, bool] = {}
    summary_text = ""
    while time.monotonic() < deadline:
        if capture_visuals and run_suite in ("core", "opengl", "all", "long"):
            for test_id in ("opengl-x64", "opengl-x86"):
                if test_id in captured:
                    continue
                result_text = get_device_text(hdc, device_id, f"{remote_results}/{test_id}.json")
                if '"message"' in result_text and '"fixed-frame"' in result_text:
                    captured[test_id] = capture_frame(
                        hdc, device_id, run_directory, run_id, test_id, validate_rgba_quadrants)
        if capture_visuals and run_suite in ("d3d9", "dxvk", "dxvk-long", "dxvk-dynamic", "all",
                         "dxvk-modern-baseline", "dxvk-modern-long"):
            for test_id in dxvk_tests_for_suite(run_suite):
                if test_id in captured:
                    continue
                result_text = get_device_text(hdc, device_id, f"{remote_results}/{test_id}.json")
                if '"message"' in result_text and '"fixed-frame"' in result_text:
                    captured[test_id] = capture_d3d11_frame(
                        hdc, device_id, run_directory, run_id, test_id)
        summary_text = get_device_text(hdc, device_id, remote_stable)
        if summary_text:
            break
        time.sleep(0.5)

    if not summary_text:
        raise RuntimeError(f"Suite {run_id} timed out without suite-summary.json")
    summary_path = run_directory / "suite-summary.json"
    summary_path.write_text(summary_text + "\n", encoding="utf-8")
    summary = json.loads(summary_text)

    if capture_visuals and run_suite in ("core", "opengl", "all", "long"):
        for test_id in ("opengl-x64", "opengl-x86"):
            captured.setdefault(test_id, False)
    if capture_visuals and run_suite in ("d3d9", "dxvk", "dxvk-long", "dxvk-dynamic", "all",
                     "dxvk-modern-baseline", "dxvk-modern-long"):
        for test_id in dxvk_tests_for_suite(run_suite):
            captured.setdefault(test_id, False)

    _, hilog_text = run_hdc(hdc, device_id, "shell", "hilog", "-z", "10000", "-t", "app")
    (run_directory / "hilog.txt").write_text(hilog_text, encoding="utf-8")
    save_device_file(hdc, device_id,
                     f"{DEVICE_SANDBOX}/temp/wine_stderr_{datetime.now():%Y%m%d}.log",
                     run_directory / "wine-stderr.log")
    save_device_file(hdc, device_id, f"{DEVICE_SANDBOX}/cache/winehua_virgl_host.log",
                     run_directory / "virgl-host.log")
    save_device_file(hdc, device_id, f"{DEVICE_SANDBOX}/temp/winehua_vtest_frontbuffer.log",
                     run_directory / "vtest-frontbuffer.log")
    save_probe_results(hdc, device_id, run_directory, remote_results, summary)

    custom_border_selections = []
    wine_stderr_path = run_directory / "wine-stderr.log"
    if wine_stderr_path.is_file():
        seen = set()
        for match in re.finditer(
                r"custom-border path=([a-z-]+) reason=([a-zA-Z0-9_-]+)",
                wine_stderr_path.read_text(encoding="utf-8", errors="replace")):
            key = f"{match.group(1)}|{match.group(2)}"
            if key not in seen:
                seen.add(key)
                custom_border_selections.append(
                    {"path": match.group(1), "reason": match.group(2)})

    visual_pass = True if not capture_visuals else not any(not value for value in captured.values())
    if run_suite in ("dxvk", "dxvk-long", "dxvk-dynamic", "all",
                     "dxvk-modern-baseline", "dxvk-modern-long"):
        coverage = get_d3d11_coverage(summary, run_suite, long_seconds)
    else:
        coverage = None
    coverage_pass = coverage is None or coverage["status"] == "PASS"
    host_summary = {
        "schemaVersion": 1,
        "runId": run_id,
        "suite": run_suite,
        "prefix": run_prefix,
        "perfProfile": perf_profile,
        "appStatus": summary.get("status"),
        "visualStatus": "SKIPPED" if not capture_visuals else ("PASS" if visual_pass else "FAIL"),
        "coverageStatus": "NOT_APPLICABLE" if coverage is None else coverage["status"],
        "visuals": captured,
        "coverage": coverage,
        "customBorderSelections": custom_border_selections,
        "status": "PASS" if (summary.get("status") == "PASS" and visual_pass and coverage_pass) else "FAIL",
    }
    write_json(run_directory / "host-summary.json", host_summary)
    return host_summary["status"] == "PASS"


def select_device(hdc: str, device_id: str) -> str:
    if device_id:
        return device_id
    result = subprocess.run([hdc, "list", "targets"], capture_output=True,
                            text=True, errors="replace")
    output = result.stdout
    targets = [line.strip() for line in output.splitlines()
               if line.strip() and not line.strip().startswith("[")]
    # Prefer a physical target when HDC also exposes the local
    # forwarding/emulator target. An ARM64 HAP is intentionally rejected by
    # the x86 localhost target.
    physical = [target for target in targets
                if not re.match(r"^(127\.0\.0\.1|localhost)(:|$)", target)]
    candidates = physical or targets
    if not candidates:
        raise RuntimeError("No HDC device is connected")
    return candidates[0]


def main() -> int:
    args = parse_args()
    hdc = resolve_hdc()
    if args.skip_build and not HAP_PATH.is_file():
        raise SystemExit("Signed HAP missing while --skip-build was requested")
    if args.runs < 1:
        raise SystemExit("--runs must be at least 1")

    device_id = select_device(hdc, args.device_id)
    archive_root = Path(args.archive_root or
                        os.environ.get("WINEHUA_ARCHIVE_ROOT") or
                        REPO_ROOT / "build/automation-logs")
    session_id = f"regression-{datetime.now():%Y%m%d-%H%M%S}"
    session_directory = archive_root / session_id
    session_directory.mkdir(parents=True, exist_ok=True)

    if not args.skip_build:
        invoke_build(session_directory / "build.log")
    artifact = get_artifact_metadata(session_directory)
    code, install_output = run_hdc_install(hdc, device_id, HAP_PATH)
    (session_directory / "install.log").write_text(install_output, encoding="utf-8")
    if code != 0 or "install bundle successfully" not in install_output:
        raise RuntimeError("HAP overwrite install did not report install bundle successfully")

    matrix: list[tuple[str, str]] = []
    if args.gate:
        matrix = [("core", "reuse")] * 3 + [("core", "clean")]
    else:
        matrix = [(args.suite, args.prefix)] * args.runs

    all_passed = True
    run_records: list[dict] = []
    for index, (run_suite, run_prefix) in enumerate(matrix, 1):
        run_id = f"{session_id}-{index:02d}-{run_suite}-{run_prefix}"
        try:
            passed = invoke_one_run(
                hdc, device_id, run_suite, run_prefix, run_id,
                session_directory, PRODUCT_PERF_PROFILE, args.long_seconds,
                args.timeout_minutes)
        except Exception as error:  # noqa: BLE001 - infrastructure errors are recorded, not fatal
            passed = False
            (session_directory / f"{run_id}-infrastructure-error.txt").write_text(
                str(error) + "\n", encoding="utf-8")
        finally:
            # Smoke uses the singleton EntryAbility with winehua.mode=smoke. If
            # it remains alive, a later icon launch is delivered through
            # onNewWant and can retain the automation page/window state instead
            # of rebuilding the normal Tablet desktop. Stop only our test app
            # after all result and screenshot collection for this run has
            # completed; the next run (or a normal user launch) will then start
            # from the correct mode boundary.
            run_hdc(hdc, device_id, "shell", "aa", "force-stop", BUNDLE)
        run_records.append({"runId": run_id, "suite": run_suite,
                            "prefix": run_prefix, "passed": passed})
        if not passed:
            all_passed = False

    write_json(session_directory / "automation-summary.json", {
        "schemaVersion": 1,
        "sessionId": session_id,
        "deviceId": device_id,
        "hapSha256": artifact["hapSha256"],
        "gate": bool(args.gate),
        "perfProfile": PRODUCT_PERF_PROFILE,
        "status": "PASS" if all_passed else "FAIL",
        "runs": run_records,
    })

    print(f"Automation {'PASS' if all_passed else 'FAIL'}: {session_directory}")
    return 0 if all_passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
