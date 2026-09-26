#!/usr/bin/env python3
"""WineHua smoke 自动化工具（host 侧）。

设计见 docs/engineering/testing-design.md。

    python3 automation/smoke.py build [--suite NAME] [--case NAME] [--out DIR]
    python3 automation/smoke.py push
    python3 automation/smoke.py run --suite NAME [--tests ID,ID] [--inline FILE]
    python3 automation/smoke.py check <run-dir>
    python3 automation/smoke.py gate

构建流程：扫描 smoke/tests/*/test.json（用例）与 smoke/suites/*.json（套件定义），
交叉编译/收集 exe，生成 build/smoke-payload/（suites.json + manifest.json）。
产物不进 HAP：由 host 经 hdc 推送到设备沙箱；设备端 SmokeHook.seed 按 manifest
的内容版本（suiteVersion）导入 C:\\smoke。判定在 host 侧（automation/checks/），
`check` 可对历史归档重跑，改判定规则不必重跑设备。
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
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

# 判定器包（automation/checks/）：判定与执行分离，check 子命令对归档重跑同一套
sys.path.insert(0, str(Path(__file__).resolve().parent))
from checks import evaluate as evaluate_checks, parse_checks  # noqa: E402

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
    checks: list = field(default_factory=list)

    @property
    def exe_stem(self) -> str:
        return self.exe[:-4] if self.exe.lower().endswith(".exe") else self.exe

    @property
    def declared_checks(self) -> list:
        return self.checks or ["result-json"]

    @property
    def needs_frame(self) -> bool:
        """需要固定帧截图的用例（声明了 visual:* 判定）。"""
        return any(name == "visual" for name, _ in parse_checks(self.declared_checks))


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
            checks=list(body.get("checks", [])),
        )
        if case.id in cases:
            die(f"duplicate case id {case.id}: {path}")
        cases[case.id] = case
    if not cases:
        die(f"no cases under {TESTS_DIR}")
    return cases


# 经 __env 下发后设备端读不到的键。声明了也没有任何效果，装载期直接拦下，
# 避免"写了以为开了"的静默失效。Wine 日志看 hilog 的 WineChild-stderr tag。
UNREACHABLE_ENV_KEYS = {
    # 设计上忽略: profile 选择要自己判断 exe 类型与覆盖来源, 通用覆盖会让它失效
    "WINEDEBUG": "设备端显式忽略（wine_child.cpp select_winedebug_profile）",
    # 时序缺陷: 该函数在 458 行读它, __env 覆盖在 479 行才应用 —— 属待修 bug,
    # 不是设计意图 (同处注释声称这条通道可用)
    "WINEHUA_WINEDEBUG": "设备端读取早于 __env 应用（wine_child.cpp:458 vs 479）",
}


def reject_unreachable_env(context: str, env: dict) -> None:
    for key in env or {}:
        reason = UNREACHABLE_ENV_KEYS.get(key)
        if reason:
            die(f"{context}: env {key} 不会生效 —— {reason}"
                f"; Wine 日志看 hilog 的 WineChild-stderr tag")


# native 契约（wine_exe.cpp/wine_env.cpp）只认完整档位值，其余值被静默丢弃、
# 引擎回落到无档位 env —— 表现为"该档 DLL 在位却不可用"。在这里拦死。
D3D_BACKEND_VALUES = {"wined3d", "dxvk_legacy", "dxvk_modern_2_6", "vkd3d_limited_500k"}
DXVK_BACKEND_VALUES = {"dxvk_legacy", "dxvk_modern_2_6"}


def reject_bad_backend(context: str, backend: dict) -> None:
    d3d = backend.get("d3d")
    if d3d and d3d not in D3D_BACKEND_VALUES:
        die(f"{context}: backend.d3d=\"{d3d}\" 不是契约值 —— "
            f"合法值 {sorted(D3D_BACKEND_VALUES)}（不带档位后缀的 \"dxvk\" 会被"
            f"native 静默丢弃）")
    dxvk = backend.get("dxvk")
    if dxvk and dxvk not in DXVK_BACKEND_VALUES:
        die(f"{context}: backend.dxvk=\"{dxvk}\" 不是契约值 —— "
            f"合法值 {sorted(DXVK_BACKEND_VALUES)}")


def load_suite(path: Path, cases: dict) -> tuple:
    body = json.loads(path.read_text())
    name = body["name"]
    entries = []
    for inst in body.get("tests", []):
        reject_unreachable_env(path.name, inst.get("env"))
        if inst.get("backend"):
            reject_bad_backend(f"{path.name}:{inst.get('id', inst['case'])}", inst["backend"])
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
    return name, {"title": body.get("title", name),
                  "checks": list(body.get("checks", [])), "tests": entries}


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


# hdc shell 的超时保护。设备端卡死时（测试进程进 D-state 等）hdc 调用可能永不
# 返回，没有 timeout 的话 subprocess.run 会永久阻塞，调用方的 deadline 检查也就
# 永远执行不到 —— 实测一轮 run 挂满 23 分钟直到手工 kill（设备端与 host 侧两层
# 超时全部失效）。默认值留够 aa start / snapshot_display 这类慢命令；轮询路径
# 用更短的值，让"设备端无响应"尽快暴露。
HDC_SHELL_TIMEOUT_S = 60
HDC_POLL_TIMEOUT_S = 20
# 等应用进程起来的上限（ensure_app_running）：aa start 返回后进程出现通常在
# 2~3s 内，冷启动建 prefix 时会久一些。
APP_START_TIMEOUT_S = 30


def hdc_shell(hdc: str, device: str, script: str,
              timeout: int = HDC_SHELL_TIMEOUT_S) -> tuple:
    """执行设备端 shell 命令；超时按失败返回 (-1, "")，不向上抛。"""
    try:
        result = subprocess.run([hdc, "-t", device, "shell", script],
                                capture_output=True, text=True,
                                timeout=timeout, errors="replace")
        return result.returncode, result.stdout
    except subprocess.TimeoutExpired:
        return -1, ""


def app_pid(hdc: str, device: str) -> str:
    """应用主进程 pid（未运行时为空串）。"""
    code, out = hdc_shell(hdc, device, f"pidof {BUNDLE} 2>/dev/null")
    return out.strip() if code == 0 else ""


def ensure_app_running(hdc: str, device: str, extra_start_args: str = "") -> None:
    """确保应用进程存在 —— 沙箱 -b 通道的前提。

    `-b bundlename` 要求目标应用是**调试证书签名**且**已在设备上启动**，否则
    报 E003001 Invalid bundle name（官方 hdc 文档列了三种原因：未安装 / 非调试
    签名 / 未启动）。全新安装或 force-stop 之后直接推送必然撞上 —— cmd_run 是
    先 push 再 aa start，指望不上后面那次启动。这里先补一次启动并等进程出现；
    能起来但推送仍失败，就说明设备上装的是非调试签名的包。

    extra_start_args: 附加 `--ps` 参数（如 winehua.desktopMode）。桌面模式是
    引擎级状态（applyModePolicy 只在冷启动的 init 链上判定），必须随冷启动
    携带；app 已在运行时不重启动，二次 want 改不了已启动引擎的模式。
    """
    if app_pid(hdc, device):
        if extra_start_args:
            die("app 已在运行：--desktop-mode 需要冷启动携带，先 `hdc shell "
                f"aa force-stop {BUNDLE}` 再重跑")
        return
    log(f"{BUNDLE} 未运行，先启动（-b 通道要求应用已启动）")
    hdc_shell(hdc, device, f"aa start -a {ABILITY} -b {BUNDLE} {extra_start_args}".rstrip())
    deadline = time.time() + APP_START_TIMEOUT_S
    while time.time() < deadline:
        if app_pid(hdc, device):
            return
        time.sleep(1)
    die(f"{BUNDLE} 启动后 {APP_START_TIMEOUT_S}s 内未见进程；"
        "设备上装的若是非调试签名包，-b 通道同样不可用")


def hdc_send(hdc: str, device: str, local: Path, remote: str) -> None:
    """推文件或目录到沙箱（remote 用沙箱视角路径）。"""
    result = subprocess.run(
        [hdc, "-t", device, "file", "send", "-b", BUNDLE, str(local), remote],
        capture_output=True, text=True, errors="replace")
    if result.returncode != 0 or "FileTransfer finish" not in result.stdout:
        detail = f"{result.stdout.strip()} {result.stderr.strip()}".strip()
        if "Invalid bundle name" in detail:
            detail += ("  ← E003001: 未安装 / 非调试签名 / 未启动三者之一，"
                       "沙箱 -b 通道要求调试签名且应用在运行")
        die(f"hdc file send failed rc={result.returncode}: {detail}")


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


def sandbox_texts(hdc: str, device: str, rel_paths: list) -> dict:
    """一次 hdc 调用读多个文件（分隔符切分）：固定帧窗口只有 2 秒，逐文件
    cat 的往返开销会把轮询周期撑到窗口之外。返回 {rel_path: 内容}。

    整个 hdc 调用失败（含超时）时返回**空 dict** —— 与"文件不存在"返回
    {path: ""} 区分开，调用方据此判断设备端是否还在响应。"""
    if not rel_paths:
        return {}
    marker = "@@@SMOKE@@@"
    script = f"; echo '{marker}'; ".join(
        f"cat '{REAL_FILES}/{path}' 2>/dev/null" for path in rel_paths)
    code, out = hdc_shell(hdc, device, script, timeout=HDC_POLL_TIMEOUT_S)
    if code == -1:
        return {}
    if code != 0:
        return {path: "" for path in rel_paths}
    chunks = out.split(marker)
    return {path: (chunks[index].strip() if index < len(chunks) else "")
            for index, path in enumerate(rel_paths)}


# ---------------------------------------------------------------------------
# 子命令：push / run
# ---------------------------------------------------------------------------

def ensure_payload(args) -> Path:
    payload = Path(args.payload).resolve()
    if not (payload / "suites.json").is_file():
        die(f"payload not built: {payload} (run: smoke.py build)")
    # 定义比载荷新 = 改了 smoke/tests 或 smoke/suites 却没重建：push 上去的
    # 仍是旧定义（实测踩坑：改套件档位后设备端 suites.json 还是旧 backend）
    definitions = list(TESTS_DIR.glob("*/test.json")) + list(SUITES_DIR.glob("*.json"))
    newest = max(path.stat().st_mtime for path in definitions)
    if newest > (payload / "suites.json").stat().st_mtime:
        die("payload 已过期（smoke/tests 或 smoke/suites 有更新），先跑: smoke.py build")
    return payload


def cmd_push(args: argparse.Namespace) -> int:
    payload = ensure_payload(args)
    hdc = resolve_hdc()
    device = resolve_device(hdc, args.device)
    mode = getattr(args, "desktop_mode", None)
    ensure_app_running(hdc, device,
                       f"--ps winehua.desktopMode {mode}" if mode else "")
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
        reject_unreachable_env("--env", overrides)
        params["env"] = overrides
    if args.d3d:
        if args.d3d not in D3D_BACKEND_VALUES:
            die(f"--d3d \"{args.d3d}\" 不是契约值 —— 合法值 {sorted(D3D_BACKEND_VALUES)}")
        params["d3dBackend"] = args.d3d
    if args.dxvk:
        if args.dxvk not in DXVK_BACKEND_VALUES:
            die(f"--dxvk \"{args.dxvk}\" 不是契约值 —— 合法值 {sorted(DXVK_BACKEND_VALUES)}")
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
        push_args = argparse.Namespace(payload=args.payload, device=args.device,
                                       desktop_mode=getattr(args, "desktop_mode", None))
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

    # 本地套件定义：给判定器提供用例的 checks 声明（inline 等未定义测试退回默认判定）
    cases = load_cases()
    suites = load_suites(cases)
    suite_tests = suites[args.suite]["tests"] if args.suite in suites else []
    if args.tests:
        wanted = {item.strip() for item in args.tests.split(",") if item.strip()}
        suite_tests = [entry for entry in suite_tests if entry.test_id in wanted]
    entries = {entry.test_id: entry for entry in suite_tests}
    frame_targets = {tid: entry for tid, entry in entries.items() if entry.case.needs_frame}

    summary_rel = f"{DRIVE_C_REL}/results/{run_id}/suite-summary.json"
    try:
        summary, frames = poll_run(hdc, device, archive, run_id, frame_targets,
                                   args.timeout_minutes, args.poll_seconds,
                                   start_command=start)
        if summary is None:
            die(f"suite summary not found within {args.timeout_minutes} min: "
                f"{REAL_FILES}/{summary_rel} "
                f"(引擎未就绪? 设备端 ready-degraded 时不会跑测试)")

        hdc_recv_dir(hdc, device, f"{DRIVE_C_REL}/results/{run_id}",
                     archive / "device-results")
        (archive / "suite-summary.json").write_text(
            json.dumps(summary, indent=2, ensure_ascii=False) + "\n")
        long_seconds = args.long_seconds or int(job.get("longSeconds", 0)) or 3600
        (archive / "artifact.json").write_text(json.dumps({
            "runId": run_id, "suite": args.suite, "prefix": args.prefix,
            "payloadVersion": manifest.get("suiteVersion"),
            "device": device, "longSeconds": long_seconds,
        }, indent=2, ensure_ascii=False) + "\n")
        # 判定层（判定与执行分离：check 子命令可对归档重跑同一套判定）
        host = judge_run(archive, entries, frames, summary.get("tests", []),
                         suites.get(args.suite), args.suite, long_seconds)
        (archive / "host-summary.json").write_text(
            json.dumps(host, indent=2, ensure_ascii=False) + "\n")
    finally:
        # 无论成败都停掉测试 App：常驻会让下次启动走 onNewWant 保留自动化
        # 窗口/页面状态，而不是重建正常形态
        hdc_shell(hdc, device, f"aa force-stop {BUNDLE}")

    for test in host["tests"]:
        log(f"  {test['testId']:<28} {test['status']:<10} "
            f"{test.get('stage', '')} {test.get('message', '')[:70]}")
    # 判定层是权威结论：设备端 status 是原始数据，其语义由判定器解释
    # （能力探针的 UNSUPPORTED 是合法答案，设备端 suite 汇总仍记 FAIL）。
    status = "PASS" if host["status"] == "PASS" else "FAIL"
    log(f"{status}: 设备端 {summary.get('status')} / 判定 {host['status']} "
        f"({host['passed']}/{host['total']}) → {archive}")
    return 0 if status == "PASS" else 1


def cmd_check(args: argparse.Namespace) -> int:
    """对历史归档重跑判定（判定规则迭代不必重跑设备）。"""
    archive = Path(args.run_dir).resolve()
    summary_path = archive / "suite-summary.json"
    if not summary_path.is_file():
        die(f"归档缺少 suite-summary.json: {archive}")
    summary = json.loads(summary_path.read_text())
    cases = load_cases()
    suites = load_suites(cases)
    suite = summary.get("suite", "")
    entries = {entry.test_id: entry for entry in suites[suite]["tests"]} if suite in suites else {}
    # 帧分组：<test_id>.jpeg 与重试帧 <test_id>-<n>.jpeg（testId 以 -x64/-x86 结尾，
    # 去掉末尾纯数字后缀即归属测试）
    frames = {}
    for path in sorted((archive / "frames").glob("*.jpeg")):
        frames.setdefault(re.sub(r"-\d+$", "", path.stem), []).append(path)
    artifact_path = archive / "artifact.json"
    long_seconds = 3600
    if artifact_path.is_file():
        long_seconds = int(json.loads(artifact_path.read_text()).get("longSeconds", 3600))
    host = judge_run(archive, entries, frames, summary.get("tests", []),
                     suites.get(suite), suite, long_seconds)
    (archive / "host-summary.json").write_text(
        json.dumps(host, indent=2, ensure_ascii=False) + "\n")
    for test in host["tests"]:
        log(f"  {test['testId']:<28} {test['status']:<10} "
            f"{test.get('stage', '')} {test.get('message', '')[:70]}")
    log(f"check {host['status']} ({host['passed']}/{host['total']}) → {archive}")
    return 0 if host["status"] == "PASS" else 1


def snapshot_frame(hdc: str, device: str, archive: Path, test_id: str,
                   index: int = 0) -> Path | None:
    """截取固定帧并回传本地归档（index>0 为同一测试的重试帧）。"""
    remote = f"/data/local/tmp/smoke-frame-{test_id}-{index}.jpeg"
    code, _ = hdc_shell(hdc, device, f"snapshot_display -f {remote}")
    if code != 0:
        return None
    name = f"{test_id}.jpeg" if index == 0 else f"{test_id}-{index}.jpeg"
    local = archive / "frames" / name
    local.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run([hdc, "-t", device, "file", "recv", remote, str(local)],
                   capture_output=True, text=True, errors="replace")
    return local if local.is_file() else None


def recent_log(hdc: str, device: str, seconds: int = 4) -> str:
    """抓一小段设备日志（流式 hilog，超时截断）。"""
    try:
        result = subprocess.run([hdc, "-t", device, "shell", "hilog"],
                                capture_output=True, text=True,
                                timeout=seconds, errors="replace")
        return result.stdout or ""
    except subprocess.TimeoutExpired as error:
        if error.stdout:
            return error.stdout.decode("utf-8", errors="replace")
        return ""


def poll_run(hdc: str, device: str, archive: Path, run_id: str,
             frame_targets: dict, timeout_minutes: int, poll_seconds: int,
             frame_attempts: int = 4, start_command: str = "",
             probe_after_s: int = 90) -> tuple:
    """轮询 suite-summary；期间对需要视觉判定的测试采集固定帧。

    截图时机沿用旧脚本语义：测试程序完成固定帧渲染后写结果文件（message 含
    "fixed-frame"），此时画面仍在 —— 轮询到该文件即采集。采集多帧（旧脚本
    capture_d3d11_frame 的 4 次尝试语义）：场景随动画相位波动，判定侧取任一
    通过即可，单帧采样会把瞬时相位判成失败。

    引擎自愈：桌面模式下 explorer 桌面根 15s 未就绪时设备端报告 ready-degraded，
    此时不放行任何程序（canLaunch 语义），盲等只会超时。启动后长时间无结果时
    抓日志确认，命中则重启 App 重试一次。
    """
    summary_rel = f"{DRIVE_C_REL}/results/{run_id}/suite-summary.json"
    deadline = time.time() + timeout_minutes * 60
    frames = {}
    summary = None
    started = time.time()
    retried = False
    consecutive_unresponsive = 0
    while time.time() < deadline:
        pending = [tid for tid in frame_targets if tid not in frames]
        time.sleep(0.15 if pending else poll_seconds)
        # 每轮一次 hdc 读全部待采帧测试 + suite summary（见 sandbox_texts）
        paths = [f"{DRIVE_C_REL}/results/{run_id}/{tid}.json" for tid in pending]
        texts = sandbox_texts(hdc, device, paths + [summary_rel])
        if not texts:
            # hdc 调用本身失败（超时）= 设备端可能卡死。卡住的测试进程不自愈，
            # 别耗满 deadline 才报 —— 连续几轮无响应就带现场退出。
            consecutive_unresponsive += 1
            if consecutive_unresponsive >= 5:
                die(f"设备端连续 {consecutive_unresponsive} 轮无响应"
                    f"（hdc shell 超时 {HDC_POLL_TIMEOUT_S}s）：测试进程可能卡死，"
                    f"设备端日志见 {SANDBOX_FILES}/temp/wine_stderr_*.log")
            continue
        consecutive_unresponsive = 0
        for test_id in pending:
            text = texts.get(f"{DRIVE_C_REL}/results/{run_id}/{test_id}.json", "")
            if '"fixed-frame"' in text and '"message"' in text:
                captured = []
                for attempt in range(frame_attempts):
                    if attempt:
                        time.sleep(0.6)
                    path = snapshot_frame(hdc, device, archive, test_id, attempt)
                    if path:
                        captured.append(path)
                if captured:
                    frames[test_id] = captured
                    log(f"  frame: {test_id} captured x{len(captured)}")
        text = texts.get(summary_rel, "")
        if text.strip().startswith("{"):
            try:
                summary = json.loads(text)
                break
            except json.JSONDecodeError:
                continue
        # 引擎 ready-degraded 自愈（见 docstring）
        if (not retried and start_command and not frames
                and time.time() - started > probe_after_s):
            probe = recent_log(hdc, device, 4)
            if "ready-degraded" in probe or "desktop root TIMEOUT" in probe:
                log("检测到 ready-degraded（桌面根未起）：重启 App 重试")
                hdc_shell(hdc, device, f"aa force-stop {BUNDLE}")
                time.sleep(3)
                hdc_shell(hdc, device, start_command)
                retried = True
                started = time.time()
    return summary, frames


def judge_run(archive: Path, entries: dict, frames: dict, device_tests: list = None,
              suite_def: dict = None, suite_name: str = "",
              long_seconds: int = 3600) -> dict:
    """对归档跑判定（per-test 判定 + suite 级判定）。

    entries: testId → TestEntry（套件定义；无定义的测试用默认判定）；
    device_tests: 设备端 summary 的 tests（inline 等未定义测试靠它补全，
    也是 suite 级判定 coverage 的数据源）。
    与 `smoke.py check` 共用：判定规则迭代后对历史归档重跑，不必重跑设备。
    """
    results = {}
    device_dir = archive / "device-results"
    test_ids = list(entries.keys())
    for item in device_tests or []:
        test_id = item.get("testId", "")
        if test_id and test_id not in entries:
            test_ids.append(test_id)
    collected = []
    for test_id in test_ids:
        entry = entries.get(test_id)
        result = None
        result_path = device_dir / f"{test_id}.json"
        if result_path.is_file():
            try:
                result = json.loads(result_path.read_text())
            except json.JSONDecodeError:
                result = None
        if result is None and device_tests:
            # 结果文件缺失：测试超时/崩溃时设备端只把结论写进 suite-summary
            # （SmokeRunner 的 failure() 是内存对象），不落结果文件。回退到
            # summary 条目以保住 stage/message —— 否则判定只剩一句"结果文件
            # 缺失"，超时原因整个丢掉（实测 --timeout-ms 1500 即如此）。
            result = next((item for item in device_tests
                           if item.get("testId") == test_id), None)
        if result is not None:
            collected.append(result)
        declared = entry.case.declared_checks if entry else ["result-json"]
        verdict = evaluate_checks(declared, {
            "run_dir": archive, "test_id": test_id,
            "test": entry.to_suite_json() if entry else {},
            "result": result, "frames": frames.get(test_id, []),
        })
        results[test_id] = verdict

    # suite 级判定（coverage 等）。数据源是各测试的结果文件 —— metrics 在结果
    # 文件里（设备端 suite-summary 条目不含 metrics，重建时按瘦解释器简化）
    suite_verdict = {"status": "SKIP", "verdicts": [], "stage": "", "message": ""}
    declared_suite = suite_def.get("checks", []) if suite_def else []
    if declared_suite:
        suite_verdict = evaluate_checks(declared_suite, {
            "run_dir": archive, "test_id": suite_name, "test": {},
            "result": None, "frames": [],
            "summary": {"tests": collected},
            "suite": suite_name, "long_seconds": long_seconds,
        })

    failed = [v for v in results.values() if v["status"] == "FAIL"]
    per_test = "PASS" if results and not failed else ("FAIL" if failed else "SKIP")
    if per_test == "FAIL" or suite_verdict["status"] == "FAIL":
        status = "FAIL"
    elif per_test == "PASS":
        status = "PASS"
    else:
        status = "SKIP"
    return {
        "schemaVersion": 1,
        "status": status,
        "passed": sum(1 for v in results.values() if v["status"] == "PASS"),
        "total": len(results),
        "tests": [{"testId": test_id, **verdict} for test_id, verdict in results.items()],
        "suiteVerdicts": suite_verdict.get("verdicts", []),
    }


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
    # --case: 单独构建未接入套件的用例（如手动调试工具 win32-driver），
    # 使其 exe 进入 payload，可用 --inline 组合出临时测试跑参数组合。
    for case_id in [item.strip() for item in (args.case or "").split(",") if item.strip()]:
        if case_id not in cases:
            die(f"unknown case {case_id} (have: {', '.join(sorted(cases))})")
        for arch in cases[case_id].arch:
            needed.add((case_id, arch))
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
    """与既有 payload 逐字节比对（比对忽略 PE 编译时间戳，见 pe_normalized）。"""
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


def cmd_gate(args: argparse.Namespace) -> int:
    """入口门禁：3×reuse core + 1×clean core —— 引擎健康与基础渲染的最小回归。

    载荷不变，只有第一次 run 推送；后续 --skip-push 复用设备端已有载荷。
    """
    parser = build_parser()
    plan = [("reuse", False), ("reuse", True), ("reuse", True), ("clean", True)]
    failed = []
    for index, (prefix, skip_push) in enumerate(plan, 1):
        log(f"gate {index}/{len(plan)}: core prefix={prefix}")
        argv = ["run", "--suite", "core", "--prefix", prefix,
                "--payload", args.payload, "--archive-root", args.archive_root,
                "--timeout-minutes", str(args.timeout_minutes)]
        if args.device:
            argv += ["--device", args.device]
        if skip_push:
            argv.append("--skip-push")
        if cmd_run(parser.parse_args(argv)) != 0:
            failed.append(f"{index}:{prefix}")
    log(f"gate {'PASS' if not failed else 'FAIL'} "
        f"({len(plan) - len(failed)}/{len(plan)}), 未通过: {', '.join(failed) or '无'}")
    return 0 if not failed else 1


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="WineHua smoke automation (host)")
    sub = parser.add_subparsers(dest="command", required=True)

    build = sub.add_parser("build", help="构建 payload 到 build/smoke-payload/")
    build.add_argument("--suite", default="", help="只构建该套件用到的用例（suites.json 仍全量）")
    build.add_argument("--case", default="", help="额外构建的用例 id（逗号分隔，可未接入套件）")
    build.add_argument("--out", default=str(DEFAULT_OUT))
    build.add_argument("--check", default="",
                       help="与既有 payload 目录逐字节比对")
    build.set_defaults(func=cmd_build)

    push = sub.add_parser("push", help="推送 payload 到设备沙箱（播种源）")
    push.add_argument("--payload", default=str(DEFAULT_OUT))
    push.add_argument("--device", default="")
    push.add_argument("--desktop-mode", choices=("virtual", "fusion"), default=None,
                      help="冷启动携带 winehua.desktopMode 覆盖（引擎级模式，仅冷启动生效）")
    push.set_defaults(func=cmd_push)

    run = sub.add_parser("run", help="跑一个套件：推送 + aa start + 轮询 + 归档")
    run.add_argument("--suite", required=True)
    run.add_argument("--prefix", choices=("reuse", "clean"), default="reuse")
    run.add_argument("--desktop-mode", choices=("virtual", "fusion"), default=None,
                     help="冷启动携带 winehua.desktopMode 覆盖（C 型注入用例需要"
                          "桌面合成模式的输入链；app 已运行时须先 force-stop）")
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
    run.add_argument("--d3d", default="", help="覆盖 d3d 后端（契约值: wined3d/dxvk_legacy/dxvk_modern_2_6/vkd3d_limited_500k）")
    run.add_argument("--dxvk", default="", help="覆盖 dxvk 后端（契约值: dxvk_legacy/dxvk_modern_2_6）")
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

    check = sub.add_parser("check", help="对历史归档重跑判定（不碰设备）")
    check.add_argument("run_dir", help="归档目录（含 suite-summary.json）")
    check.set_defaults(func=cmd_check)

    devices = sub.add_parser("devices", help="列出 hdc 设备")
    devices.set_defaults(func=cmd_devices)

    gate = sub.add_parser("gate", help="入口门禁：3×reuse core + 1×clean core")
    gate.add_argument("--payload", default=str(DEFAULT_OUT))
    gate.add_argument("--device", default="")
    gate.add_argument("--archive-root",
                      default=str(REPO_ROOT / "build/automation-logs"))
    gate.add_argument("--timeout-minutes", type=int, default=15)
    gate.set_defaults(func=cmd_gate)

    return parser


def main() -> int:
    args = build_parser().parse_args()
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
