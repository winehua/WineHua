#!/usr/bin/env python3
"""WineHua smoke 自动化工具（host 侧）。

设计见 docs/SMOKE_V2_DESIGN.md。当前实现 P1：payload 本地构建。

    python3 automation/smoke.py build [--suite NAME] [--out DIR] [--check DIR]

构建流程：扫描 smoke/tests/*/test.json（用例）与 smoke/suites/*.json（套件定义），
交叉编译/收集 exe，生成 build/smoke-payload/（suites.json + manifest.json）。
产物不进 HAP：由 host 经 hdc 推送到设备沙箱（P2 起）。
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
TESTS_DIR = REPO_ROOT / "smoke/tests"
SUITES_DIR = REPO_ROOT / "smoke/suites"
DEFAULT_OUT = REPO_ROOT / "build/smoke-payload"

MINGW = {"x64": "x86_64-w64-mingw32-gcc", "x86": "i686-w64-mingw32-gcc"}
# vulkan-1 PE 导入库所在子目录（wine 构建 --enable-archs=i386,x86_64 的产物）
WINE_ARCH_DIR = {"x64": "x86_64-windows", "x86": "i386-windows"}


def log(message: str) -> None:
    print(f"[smoke] {message}", flush=True)


def die(message: str) -> "None":
    print(f"[smoke][ERROR] {message}", file=sys.stderr, flush=True)
    sys.exit(1)


# ---------------------------------------------------------------------------
# 构建环境（路径知识的唯一来源是 scripts/env.sh，不在此重复）
# ---------------------------------------------------------------------------

_ENV_KEYS = ("BUILD_DIR", "DXVK_SRC", "VKD3D_PROTON_BUILD_ROOT")


def load_build_env() -> dict:
    script = (
        "set -e; source scripts/env.sh >/dev/null 2>&1 || true; "
        + "; ".join(f'printf "%s=%s\\n" {k} "${{{k}:-}}"' for k in _ENV_KEYS)
    )
    result = subprocess.run(["bash", "-c", script], cwd=REPO_ROOT,
                            capture_output=True, text=True, errors="replace")
    if result.returncode != 0:
        die(f"source scripts/env.sh failed: {result.stderr.strip()}")
    env = {}
    for line in result.stdout.splitlines():
        if "=" in line:
            key, _, value = line.partition("=")
            env[key] = value
    missing = [k for k in _ENV_KEYS if not env.get(k)]
    if missing:
        die(f"scripts/env.sh did not provide: {', '.join(missing)}")
    return env


# ---------------------------------------------------------------------------
# 定义模型
# ---------------------------------------------------------------------------

@dataclass
class Case:
    """用例：一个 exe 的构建来源（源码交叉编译 / wine 产物 / vkd3d 产物）。"""

    id: str
    exe: str
    arch: list
    title: str = ""
    build: dict = field(default_factory=dict)
    from_wine: str = ""
    from_vkd3d: str = ""

    @property
    def exe_stem(self) -> str:
        return self.exe[:-4] if self.exe.lower().endswith(".exe") else self.exe


@dataclass
class TestEntry:
    """套件内的一个测试实例（已按 arch 展开）。"""

    test_id: str
    case: Case
    arch: str
    params: dict

    def to_suite_json(self) -> dict:
        """设备端 suites.json 条目（扁平字段，与历史 schema 及 SmokeRunner 读取一致）。"""
        params = dict(self.params)
        backend = params.pop("backend", None) or {}
        payload = {"testId": self.test_id, "exe": f"{self.arch}/{self.case.exe}"}
        payload.update(params)
        if backend.get("d3d"):
            payload["d3dBackend"] = backend["d3d"]
        if backend.get("dxvk"):
            payload["dxvkBackend"] = backend["dxvk"]
        return payload


def load_cases() -> dict:
    cases = {}
    for path in sorted(TESTS_DIR.glob("*/test.json")):
        body = json.loads(path.read_text())
        case = Case(
            id=body["id"],
            exe=body["exe"],
            arch=list(body.get("arch", ["x64"])),
            title=body.get("title", ""),
            build=body.get("build", {}),
            from_wine=body.get("from_wine", ""),
            from_vkd3d=body.get("from_vkd3d", ""),
        )
        if case.id in cases:
            die(f"duplicate case id {case.id}: {path}")
        cases[case.id] = case
    if not cases:
        die(f"no cases under {TESTS_DIR}")
    return cases


def load_suite(path: Path, cases: dict) -> tuple:
    body = json.loads(path.read_text())
    name = body["name"]
    entries = []
    for inst in body.get("tests", []):
        case_id = inst["case"]
        if case_id not in cases:
            die(f"{path.name}: unknown case {case_id}")
        case = cases[case_id]
        archs = inst.get("arch", case.arch)
        # testId: 显式 testId 原样用（单实例语义）；否则 <id>-<arch>
        params = {k: v for k, v in inst.items() if k not in ("case", "id", "testId", "arch")}
        for arch in archs:
            test_id = inst["testId"] if "testId" in inst else f"{inst['id']}-{arch}"
            entries.append(TestEntry(test_id=test_id, case=case, arch=arch, params=params))
    return name, {"title": body.get("title", name), "tests": entries}


def load_suites(cases: dict) -> dict:
    suites = {}
    for path in sorted(SUITES_DIR.glob("*.json")):
        name, suite = load_suite(path, cases)
        if name in suites:
            die(f"duplicate suite name {name}: {path}")
        suites[name] = suite
    if not suites:
        die(f"no suites under {SUITES_DIR}")
    return suites


# ---------------------------------------------------------------------------
# 产物收集与编译
# ---------------------------------------------------------------------------

def resolve_wine_program(env: dict, case: Case, arch: str) -> Path:
    """wine 构建系统的 programs/<name> 产物；i386 优先 wine-ohos（wine-i386-pe 已废弃）。"""
    program = case.from_wine
    build_dir = Path(env["BUILD_DIR"])
    candidates = [
        build_dir / f"wine-ohos/programs/{program}/{WINE_ARCH_DIR[arch]}/{program}.exe",
        build_dir / f"wine-i386-pe/programs/{program}/{WINE_ARCH_DIR[arch]}/{program}.exe",
    ]
    for path in candidates:
        if path.is_file():
            return path
    return candidates[0]


def resolve_vkd3d_artifact(env: dict, case: Case, arch: str) -> Path:
    return Path(env["VKD3D_PROTON_BUILD_ROOT"]) / case.from_vkd3d


def build_case(env: dict, case: Case, arch: str, dest: Path) -> None:
    """把一个用例的一个架构构建/拷贝到 dest。"""
    dest.parent.mkdir(parents=True, exist_ok=True)
    if case.from_wine:
        source = resolve_wine_program(env, case, arch)
        if not source.is_file():
            die(f"wine program missing: {source} (run make wine first)")
        shutil.copyfile(source, dest)
        log(f"  {case.id}/{arch}: {source.relative_to(REPO_ROOT)} → {dest.name}")
        return
    if case.from_vkd3d:
        source = resolve_vkd3d_artifact(env, case, arch)
        if not source.is_file():
            die(f"vkd3d artifact missing: {source}")
        shutil.copyfile(source, dest)
        log(f"  {case.id}/{arch}: vkd3d artifact → {dest.name}")
        return
    spec = case.build
    sources = [str(REPO_ROOT / src) for src in spec.get("sources", [])]
    if not sources:
        die(f"case {case.id}: no build.sources and no from_wine/from_vkd3d")
    for src in sources:
        if not Path(src).is_file():
            die(f"case {case.id}: source missing: {src}")
    command = [MINGW[arch], *spec.get("cflags", ["-O2", "-s"]), "-o", str(dest), *sources]
    if spec.get("dxvk_include"):
        command.append(f"-I{env['DXVK_SRC']}/include")
    if spec.get("vulkan_import"):
        import_lib = (Path(env["BUILD_DIR"]) /
                      f"wine-ohos/dlls/vulkan-1/{WINE_ARCH_DIR[arch]}/libvulkan-1.a")
        if not import_lib.is_file():
            die(f"wine Vulkan import library missing: {import_lib}")
        command.append(str(import_lib))
    command.extend(spec.get("libs", []))
    result = subprocess.run(command, capture_output=True, text=True, errors="replace")
    if result.returncode != 0:
        print(result.stdout + result.stderr, file=sys.stderr)
        die(f"compile failed: {case.id}/{arch}")
    log(f"  {case.id}/{arch}: compiled → {dest.name}")


def copy_assets(env: dict, out_dir: Path) -> None:
    """venus smoke shader：guest_vulkan 构建产物，随载荷版本化。"""
    guest_arch = "x86_64"
    root = Path(env["BUILD_DIR"]) / f"guest_vulkan/{guest_arch}/share/winehua"
    names = ("venus_storage_write", "venus_storage_read", "venus_image_fetch",
             "venus_combined_sample", "venus_separated_sample")
    assets = out_dir / "assets"
    assets.mkdir(parents=True, exist_ok=True)
    for name in names:
        source = root / f"{name}.spv"
        if not source.is_file():
            die(f"guest shader missing: {source}")
        shutil.copyfile(source, assets / f"{name}.spv")
    log(f"  assets: {len(names)} spv")


# ---------------------------------------------------------------------------
# payload 输出
# ---------------------------------------------------------------------------

def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def pe_normalized(path: Path) -> bytes:
    """PE 内容归一化：清零编译器写入的非确定性字段（COFF TimeDateStamp 与
    Optional header CheckSum，即编译时间），其余字节原样。同名源码同参数编译
    的产物只在两处字段上不同（实测 6 字节），比对时必须忽略，否则每次比对都假失败。
    """
    data = bytearray(path.read_bytes())
    if len(data) >= 0x40 and data[:2] == b"MZ":
        e_lfanew = int.from_bytes(data[0x3C:0x40], "little")
        if data[e_lfanew:e_lfanew + 4] == b"PE\0\0":
            for offset in (e_lfanew + 8, e_lfanew + 24 + 64):
                if offset + 4 <= len(data):
                    data[offset:offset + 4] = b"\0\0\0\0"
    return bytes(data)


def pe_sha256(path: Path) -> str:
    return hashlib.sha256(pe_normalized(path)).hexdigest()


def suite_version(suites_json: Path, exes: list) -> str:
    """载荷版本 = 定义与产物的内容哈希（改定义或改 exe 都会变化）。"""
    digest = hashlib.sha256(suites_json.read_bytes())
    for path in sorted(exes):
        digest.update(path.read_bytes())
    return f"smoke-v2-{digest.hexdigest()[:12]}"


def write_payload(out_dir: Path, suites: dict, cases: dict, env: dict) -> None:
    # suites.json：设备端格式（与历史 schema 兼容，SmokeRunner 直接可读）
    suites_doc = {
        "schemaVersion": 1,
        "suites": {
            name: {"tests": [entry.to_suite_json() for entry in suite["tests"]]}
            for name, suite in suites.items()
        },
    }
    suites_json = out_dir / "suites.json"
    suites_json.write_text(json.dumps(suites_doc, indent=2, ensure_ascii=False) + "\n")

    exes = sorted(p for p in out_dir.rglob("*.exe"))
    if not exes:
        die("payload has no .exe")
    version = suite_version(suites_json, exes)
    suites_doc["suiteVersion"] = version
    suites_json.write_text(json.dumps(suites_doc, indent=2, ensure_ascii=False) + "\n")

    files = {str(p.relative_to(out_dir)): sha256(p) for p in exes}
    manifest = {"schemaVersion": 1, "suiteVersion": version, "files": files}
    (out_dir / "manifest.json").write_text(
        json.dumps(manifest, indent=2, ensure_ascii=False) + "\n")
    log(f"payload: {len(exes)} exe, version {version}")


# ---------------------------------------------------------------------------
# 设备与传输
# ---------------------------------------------------------------------------

BUNDLE = "app.hackeris.winehua"
ABILITY = "EntryAbility"
# 沙箱路径有两个视角，用途不同（2026-09-16 实测，不可混用）：
#   hdc file send -b <bundle>：remote 必须写沙箱视角，写真实路径会落到不存在的相对位置
#   hdc shell：只认真实路径；对沙箱视角路径的 rm -rf 会静默返回 0 而实际不删
SANDBOX_FILES = "/data/storage/el2/base/files"
REAL_FILES = f"/data/app/el2/100/base/{BUNDLE}/files"
# 载荷推送源（相对 files/）：设备端 SmokeHook.seed 的优先源，按 manifest 版本
# 比对后导入 C:\smoke（HAP rawfile 树 files/wine/smoke 为兜底，host 不写）。
PAYLOAD_REL = "smoke-payload"
# 当前 prefix 的实际载荷：二次 Want 不触发设备端 seed，必须直接更新这里
DRIVE_C_REL = ".wine/drive_c/smoke"
# job 文件（host 生成）：debug 参数组合 / 选测 / 内联临时用例走它下发
JOB_REL = "smoke-job.json"


def resolve_hdc() -> str:
    env_hdc = os.environ.get("WINEHUA_HDC")
    if env_hdc:
        if not Path(env_hdc).is_file():
            die(f"WINEHUA_HDC points to a missing file: {env_hdc}")
        return env_hdc
    found = shutil.which("hdc")
    if not found:
        die("hdc not found: set WINEHUA_HDC or add hdc to PATH")
    return found


def resolve_device(hdc: str, explicit: str) -> str:
    target = explicit or os.environ.get("WINEHUA_DEVICE", "")
    if target:
        return target
    result = subprocess.run([hdc, "list", "targets"], capture_output=True,
                            text=True, errors="replace")
    devices = [line.strip() for line in result.stdout.splitlines()
               if line.strip() and "Empty" not in line]
    if len(devices) == 1:
        return devices[0]
    if not devices:
        die("no hdc device found (connect the device or pass --device)")
    die("multiple devices; pass --device (or WINEHUA_DEVICE): " + ", ".join(devices))


def hdc_shell(hdc: str, device: str, script: str) -> tuple:
    result = subprocess.run([hdc, "-t", device, "shell", script],
                            capture_output=True, text=True, errors="replace")
    return result.returncode, result.stdout


def hdc_send(hdc: str, device: str, local: Path, remote: str) -> None:
    """推文件或目录到沙箱（remote 用沙箱视角路径）。"""
    result = subprocess.run(
        [hdc, "-t", device, "file", "send", "-b", BUNDLE, str(local), remote],
        capture_output=True, text=True, errors="replace")
    if result.returncode != 0 or "FileTransfer finish" not in result.stdout:
        die(f"hdc file send failed rc={result.returncode}: "
            f"{result.stdout.strip()} {result.stderr.strip()}")


def hdc_recv_dir(hdc: str, device: str, rel_path: str, local_dir: Path) -> None:
    """把 files/ 下的一个目录拉回本地（remote 同样是沙箱视角）。"""
    local_dir.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run([hdc, "-t", device, "file", "recv", "-b", BUNDLE,
                    f"{SANDBOX_FILES}/{rel_path}", str(local_dir)],
                   capture_output=True, text=True, errors="replace")


def remove_sandbox_path(hdc: str, device: str, rel_path: str) -> None:
    """删除 files/ 下的一条路径（相对 files/）。用真实路径 + 事后校验：
    `rm -rf` 对无权限路径会静默成功，不校验会留下旧载荷导致跑的还是旧内容。"""
    real = f"{REAL_FILES}/{rel_path}"
    hdc_shell(hdc, device, f"rm -rf '{real}'")
    code, out = hdc_shell(hdc, device, f"ls -d '{real}' 2>/dev/null")
    if code == 0 and out.strip():
        die(f"remove verification failed (still exists): {real}")


def sandbox_text(hdc: str, device: str, rel_path: str) -> str:
    """读 files/ 下的文本文件（真实路径，空/不存在返回空串）。"""
    code, out = hdc_shell(hdc, device, f"cat '{REAL_FILES}/{rel_path}' 2>/dev/null")
    return out if code == 0 else ""


# ---------------------------------------------------------------------------
# 子命令：push / run
# ---------------------------------------------------------------------------

def ensure_payload(args) -> Path:
    payload = Path(args.payload).resolve()
    if not (payload / "suites.json").is_file():
        die(f"payload not built: {payload} (run: smoke.py build)")
    return payload


def cmd_push(args: argparse.Namespace) -> int:
    payload = ensure_payload(args)
    hdc = resolve_hdc()
    device = resolve_device(hdc, args.device)
    log(f"push {payload} → {device}")
    # 推两处（目标都必须先删：file send 对已存在目录会把源目录嵌套为子目录）：
    # 1) 推送源：设备端 seed 的来源（冷启动 / clean 清盘后按版本比对导入）
    remove_sandbox_path(hdc, device, PAYLOAD_REL)
    hdc_send(hdc, device, payload, f"{SANDBOX_FILES}/{PAYLOAD_REL}")
    # 2) 当前 prefix 的 C:\smoke：立即生效。二次 Want 不触发 seed（seed 只在
    #    引擎 ready 链上跑），只更新推送源会导致本次会话仍读旧载荷。
    code, out = hdc_shell(hdc, device, f"ls -d '{REAL_FILES}/.wine/drive_c' 2>/dev/null")
    if code == 0 and out.strip():
        remove_sandbox_path(hdc, device, DRIVE_C_REL)
        hdc_send(hdc, device, payload, f"{SANDBOX_FILES}/{DRIVE_C_REL}")
    else:
        log("prefix 未创建：仅更新推送源，C:\\smoke 由设备端 seed 播种")
    for probe in ("suites.json", "x64/winehua_graphics_smoke.exe", "x86/winehua_graphics_smoke.exe"):
        code, out = hdc_shell(hdc, device, f"ls '{REAL_FILES}/{PAYLOAD_REL}/{probe}' 2>/dev/null")
        if code != 0 or not out.strip():
            die(f"push verification failed: missing {probe}")
    log("push done")
    return 0


def build_job(args: argparse.Namespace) -> dict:
    """host 侧的运行描述（设备端 SmokeHook.applyWant 解析）。"""
    job = {"suite": args.suite, "prefix": args.prefix}
    if args.tests:
        job["tests"] = [item.strip() for item in args.tests.split(",") if item.strip()]
    if args.inline:
        job["inline"] = json.loads(Path(args.inline).read_text())
    if args.long_seconds:
        job["longSeconds"] = args.long_seconds
    params = {}
    if args.env:
        overrides = {}
        for item in args.env:
            key, sep, value = item.partition("=")
            if not sep:
                die(f"--env 需要 KEY=VALUE 形式: {item}")
            overrides[key] = value
        params["env"] = overrides
    if args.d3d:
        params["d3dBackend"] = args.d3d
    if args.dxvk:
        params["dxvkBackend"] = args.dxvk
    if args.seconds is not None:
        params["seconds"] = args.seconds
    if args.timeout_ms is not None:
        params["timeoutMs"] = args.timeout_ms
    if params:
        job["params"] = params
    return job


def cmd_run(args: argparse.Namespace) -> int:
    payload = ensure_payload(args)
    hdc = resolve_hdc()
    device = resolve_device(hdc, args.device)
    if not args.skip_push:
        push_args = argparse.Namespace(payload=args.payload, device=args.device)
        if cmd_push(push_args) != 0:
            return 1
    manifest = json.loads((payload / "manifest.json").read_text())
    run_id = args.run_id or time.strftime("r%Y%m%d-%H%M%S")
    archive = Path(args.archive_root).resolve() / f"{args.suite}-{run_id}"
    archive.mkdir(parents=True, exist_ok=True)

    # job 文件：选测 / 参数覆盖 / 内联用例走它；5 键仍然带，兼容未升级的设备端
    job = build_job(args)
    job_path = archive / "job.json"
    job_path.write_text(json.dumps(job, indent=2, ensure_ascii=False) + "\n")
    remove_sandbox_path(hdc, device, JOB_REL)
    hdc_send(hdc, device, job_path, f"{SANDBOX_FILES}/{JOB_REL}")

    start = (f"aa start -a {ABILITY} -b {BUNDLE} "
             f"--ps winehua.mode smoke "
             f"--ps winehua.job_file {SANDBOX_FILES}/{JOB_REL} "
             f"--ps winehua.suite {args.suite} "
             f"--ps winehua.run_id {run_id} --ps winehua.prefix {args.prefix}")
    if args.long_seconds:
        start += f" --ps winehua.long_seconds {args.long_seconds}"
    log(f"run {args.suite} (runId={run_id}, prefix={args.prefix}, "
        f"job={json.dumps(job, ensure_ascii=False)})")
    code, out = hdc_shell(hdc, device, start)
    if code != 0:
        die(f"aa start failed: {out.strip()}")

    summary_rel = f"{DRIVE_C_REL}/results/{run_id}/suite-summary.json"
    try:
        summary = wait_for_summary(hdc, device, summary_rel,
                                   args.timeout_minutes, args.poll_seconds)
        if summary is None:
            die(f"suite summary not found within {args.timeout_minutes} min: "
                f"{REAL_FILES}/{summary_rel} "
                f"(引擎未就绪? 设备端 ready-degraded 时不会跑测试)")

        hdc_recv_dir(hdc, device, f"{DRIVE_C_REL}/results/{run_id}",
                     archive / "device-results")
        (archive / "suite-summary.json").write_text(
            json.dumps(summary, indent=2, ensure_ascii=False) + "\n")
        (archive / "artifact.json").write_text(json.dumps({
            "runId": run_id, "suite": args.suite, "prefix": args.prefix,
            "payloadVersion": manifest.get("suiteVersion"),
            "device": device,
        }, indent=2, ensure_ascii=False) + "\n")
    finally:
        # 无论成败都停掉测试 App：常驻会让下次启动走 onNewWant 保留自动化
        # 窗口/页面状态，而不是重建正常形态（旧 run_regression.py 的既有经验）
        hdc_shell(hdc, device, f"aa force-stop {BUNDLE}")

    tests = summary.get("tests", [])
    for test in tests:
        log(f"  {test.get('testId'):<28} {test.get('status'):<10} "
            f"{test.get('stage', '')} {test.get('message', '')[:80]}")
    status = summary.get("status", "FAIL")
    log(f"{status}: {sum(1 for t in tests if t.get('status') == 'PASS')}/{len(tests)} "
        f"→ {archive}")
    return 0 if status == "PASS" else 1


def wait_for_summary(hdc: str, device: str, summary_rel: str,
                     timeout_minutes: int, poll_seconds: int) -> dict | None:
    deadline = time.time() + timeout_minutes * 60
    while time.time() < deadline:
        time.sleep(poll_seconds)
        text = sandbox_text(hdc, device, summary_rel)
        if text.strip().startswith("{"):
            try:
                return json.loads(text)
            except json.JSONDecodeError:
                continue
    return None


def cmd_install(args: argparse.Namespace) -> int:
    hap = Path(args.hap).resolve()
    if not hap.is_file():
        die(f"HAP not found: {hap} (run make NATIVE_ARCH=arm64-v8a hap first)")
    hdc = resolve_hdc()
    device = resolve_device(hdc, args.device)
    log(f"install {hap.name} → {device}")
    result = subprocess.run([hdc, "-t", device, "install", "-r", str(hap)],
                            capture_output=True, text=True, errors="replace")
    output = (result.stdout + result.stderr).strip()
    print(output)
    if result.returncode != 0 or "successfully" not in output.lower():
        die("install failed. 降级被拒时先在设备上卸载: "
            f"hdc -t {device} uninstall {BUNDLE}")
    log("install ok")
    return 0


def cmd_devices(args: argparse.Namespace) -> int:
    hdc = resolve_hdc()
    result = subprocess.run([hdc, "list", "targets"], capture_output=True,
                            text=True, errors="replace")
    print(result.stdout.strip())
    return 0


# ---------------------------------------------------------------------------
# 子命令
# ---------------------------------------------------------------------------

def cmd_build(args: argparse.Namespace) -> int:
    env = load_build_env()
    cases = load_cases()
    suites = load_suites(cases)

    selected = suites
    if args.suite:
        if args.suite not in suites:
            die(f"unknown suite {args.suite} (have: {', '.join(sorted(suites))})")
        selected = {args.suite: suites[args.suite]}

    needed = {(entry.case.id, entry.arch) for suite in selected.values()
              for entry in suite["tests"]}
    log(f"build begin: {len(needed)} case/arch from "
        f"{len(selected)} suite(s), out={args.out}")

    out_dir = Path(args.out).resolve()
    if out_dir.exists():
        shutil.rmtree(out_dir)
    out_dir.mkdir(parents=True)

    for case_id, arch in sorted(needed):
        case = cases[case_id]
        dest = out_dir / arch / case.exe
        build_case(env, case, arch, dest)
        if not dest.is_file() or dest.stat().st_size == 0:
            die(f"artifact assertion failed: {dest}")
    copy_assets(env, out_dir)
    write_payload(out_dir, suites, cases, env)

    if args.check:
        return compare_with(args.check, out_dir)
    return 0


def compare_with(check_dir: str, out_dir: Path) -> int:
    """与既有 payload 逐字节比对（P1 迁移验证：产物必须与现设施一致）。"""
    reference = Path(check_dir).resolve()
    mismatches = []
    for produced in sorted(out_dir.rglob("*.exe")):
        rel = produced.relative_to(out_dir)
        other = reference / rel
        if not other.is_file():
            mismatches.append(f"missing in reference: {rel}")
            continue
        if pe_sha256(produced) != pe_sha256(other):
            mismatches.append(f"content differs: {rel}")
    log(f"compare with {reference}: {len(mismatches)} mismatch(es)")
    for item in mismatches:
        print(f"  [DIFF] {item}")
    return 1 if mismatches else 0


def main() -> int:
    parser = argparse.ArgumentParser(description="WineHua smoke automation (host)")
    sub = parser.add_subparsers(dest="command", required=True)

    build = sub.add_parser("build", help="构建 payload 到 build/smoke-payload/")
    build.add_argument("--suite", default="", help="只构建该套件用到的用例（suites.json 仍全量）")
    build.add_argument("--out", default=str(DEFAULT_OUT))
    build.add_argument("--check", default="",
                       help="与既有 payload 目录逐字节比对（迁移验证）")
    build.set_defaults(func=cmd_build)

    push = sub.add_parser("push", help="推送 payload 到设备沙箱（播种源）")
    push.add_argument("--payload", default=str(DEFAULT_OUT))
    push.add_argument("--device", default="")
    push.set_defaults(func=cmd_push)

    run = sub.add_parser("run", help="跑一个套件：推送 + aa start + 轮询 + 归档")
    run.add_argument("--suite", required=True)
    run.add_argument("--prefix", choices=("reuse", "clean"), default="reuse")
    run.add_argument("--payload", default=str(DEFAULT_OUT))
    run.add_argument("--device", default="")
    run.add_argument("--run-id", default="")
    run.add_argument("--long-seconds", type=int, default=0)
    run.add_argument("--skip-push", action="store_true")
    run.add_argument("--tests", default="",
                     help="逗号分隔的 testId 选测（suite 子集）")
    run.add_argument("--inline", default="",
                     help="内联测试定义 JSON 文件（临时用例；exe 须已在 C:\\smoke）")
    run.add_argument("--env", action="append", default=[],
                     help="KEY=VALUE 覆盖选中测试的 env（可重复）")
    run.add_argument("--d3d", default="", help="覆盖 d3d 后端（如 dxvk_modern_2_6）")
    run.add_argument("--dxvk", default="", help="覆盖 dxvk 后端")
    run.add_argument("--seconds", type=int, default=None)
    run.add_argument("--timeout-ms", type=int, default=None, dest="timeout_ms")
    run.add_argument("--archive-root",
                     default=str(REPO_ROOT / "build/automation-logs"))
    run.add_argument("--timeout-minutes", type=int, default=15)
    run.add_argument("--poll-seconds", type=int, default=5)
    run.set_defaults(func=cmd_run)

    install = sub.add_parser("install", help="安装当前 HAP 到设备")
    install.add_argument("--hap", default=str(REPO_ROOT / "entry/build/default/outputs/default/entry-default-signed.hap"))
    install.add_argument("--device", default="")
    install.set_defaults(func=cmd_install)

    devices = sub.add_parser("devices", help="列出 hdc 设备")
    devices.set_defaults(func=cmd_devices)

    args = parser.parse_args()
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
