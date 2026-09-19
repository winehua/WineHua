"""DXVK 系列套件的覆盖率判定（自 run_regression.get_d3d11_coverage 迁移）。

suite 级判定器：读整份设备端 summary，对含 rgba8SampleMatrix 指标的测试
（d3d11-smoke 系）提取 required 检查矩阵，全部通过才 PASS。视觉类测试
（dxvk-cube 等）证明建设备与呈现，但不重复穷举 feature 指标。
"""

from __future__ import annotations

POLICY = (
    "required API/object/RGBA8 Load-POINT-LINEAR PS-CS/descriptor/subresource "
    "array-mip-explicit-LOD-update/texture sampling/D24S8 "
    "2D-array-per-view-cube-cube-array-linear-border/present/readback coverage; "
    "ordinary R32_FLOAT comparison, optional MSAA resolve, and stencil query "
    "are reported separately"
)


def _test_entry(test: dict, suite: str, long_seconds: int) -> dict | None:
    metrics = test.get("metrics")
    if not metrics or metrics.get("rgba8SampleMatrix") is None:
        return None
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
        "dynamicConstantBuffer": (suite != "dxvk-dynamic" or bool(metrics.get("dynamicConstantBuffer"))),
        "dynamicConstantReadback": (suite != "dxvk-dynamic" or bool(metrics.get("dynamicConstantReadback"))),
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
        "longWallClock": (suite not in ("dxvk-long", "dxvk-modern-long") or
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
    return {
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
    }


def coverage(ctx: dict) -> dict:
    """suite 级覆盖率判定。ctx 需带 summary / suite / long_seconds。"""
    summary = ctx.get("summary") or {}
    suite = ctx.get("suite", "")
    long_seconds = int(ctx.get("long_seconds", 3600) or 3600)
    entries = []
    for test in summary.get("tests", []):
        entry = _test_entry(test, suite, long_seconds)
        if entry is not None:
            entries.append(entry)
    required_pass = bool(entries) and all(entry["requiredPass"] for entry in entries)
    report = {
        "schemaVersion": 1,
        "suite": suite,
        "status": "PASS" if required_pass else "FAIL",
        "tests": entries,
        "policy": POLICY,
    }
    if not entries:
        message = "无含 rgba8SampleMatrix 指标的测试（套件不含 d3d11 矩阵用例）"
    else:
        failed = [entry["testId"] for entry in entries if not entry["requiredPass"]]
        message = ("覆盖率矩阵全部通过" if required_pass
                   else f"未通过: {', '.join(failed)}")
    return {
        "status": report["status"],
        "stage": "coverage",
        "message": message,
        "metrics": report,
    }
