import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path


def load_json5_like(path: Path) -> dict:
    content = path.read_text(encoding="utf-8")
    content = re.sub(r",\s*([}\]])", r"\1", content)
    content = re.sub(r"//.*$", "", content, flags=re.MULTILINE)
    return json.loads(content)


def detect_host_adapter() -> str:
    host_shell = os.environ.get("HOST_SHELL", "").strip().lower()
    if host_shell == "msys2" and shutil.which("cygpath"):
        return "msys2"
    if host_shell == "wsl" and shutil.which("wslpath"):
        return "wsl"
    if os.environ.get("MSYSTEM") and shutil.which("cygpath"):
        return "msys2"
    if shutil.which("wslpath"):
        return "wsl"
    return "posix"


HOST_ADAPTER = detect_host_adapter()


def maybe_windows_path(path: Path) -> str:
    if HOST_ADAPTER == "wsl":
        return subprocess.check_output(["wslpath", "-w", str(path)], encoding="utf-8").strip()
    if HOST_ADAPTER == "msys2":
        return subprocess.check_output(["cygpath", "-w", str(path)], encoding="utf-8").strip()
    return str(path)


def normalize_host_path(base_dir: Path, raw_path: str) -> Path:
    value = raw_path.strip()
    if re.fullmatch(r"[A-Za-z]:[\\/].*", value):
        if HOST_ADAPTER == "wsl" and shutil.which("wslpath"):
            converted = subprocess.check_output(["wslpath", "-u", value], encoding="utf-8").strip()
            return Path(converted).resolve()
        if HOST_ADAPTER == "msys2" and shutil.which("cygpath"):
            converted = subprocess.check_output(["cygpath", "-u", value], encoding="utf-8").strip()
            return Path(converted).resolve()
        return Path(value)

    if HOST_ADAPTER == "msys2":
        match = re.fullmatch(r"/mnt/([A-Za-z])/(.*)", value)
        if match:
            value = f"/{match.group(1).lower()}/{match.group(2)}"

    path = Path(value)
    if not path.is_absolute():
        path = base_dir / path
    return path.resolve()


def resolve_material_root(base_dir: Path, cert_path: Path, store_path: Path, profile_file: Path) -> Path:
    candidates = []
    for env_name in ("SIGN_MATERIAL_ROOT", "OHOS_MATERIAL_ROOT"):
        value = os.environ.get(env_name)
        if value:
            candidates.append(Path(value).resolve())

    candidates.extend(
        [
            cert_path.parent,
            cert_path.parent.parent,
            store_path.parent,
            profile_file.parent,
            profile_file.parent.parent,
            base_dir / ".ohos",
            base_dir,
        ]
    )

    seen = set()
    for candidate in candidates:
        candidate = candidate.resolve()
        candidate_key = str(candidate)
        if candidate_key in seen:
            continue
        seen.add(candidate_key)
        if (candidate / "material").is_dir():
            return candidate

    return store_path.parent


def decode_password(field_name: str, config: dict, material_dir: Path, node_bin: str, script_path: Path) -> str:
    plain_field = f"{field_name}Plain"
    if config.get(plain_field):
        return config[plain_field]

    encrypted = config[field_name]
    if not encrypted:
        return ""
    if (
        not re.fullmatch(r"[0-9a-fA-F]+", encrypted)
        or len(encrypted) % 2 != 0
        or len(encrypted) < 32
    ):
        return encrypted

    return subprocess.check_output(
        [
            node_bin,
            maybe_windows_path(script_path),
            maybe_windows_path(material_dir),
            encrypted,
        ],
        encoding="utf-8",
    ).strip()


in_file = Path(sys.argv[1]).resolve()
out_file = Path(sys.argv[2]).resolve()
profile_path = Path("build-profile.json5").resolve()
profile = load_json5_like(profile_path)

config = profile["app"]["signingConfigs"][0]["material"]
base_dir = profile_path.parent
cert_path = normalize_host_path(base_dir, config["certpath"])
store_path = normalize_host_path(base_dir, config["storeFile"])
profile_file = normalize_host_path(base_dir, config["profile"])
material_dir = resolve_material_root(base_dir, cert_path, store_path, profile_file)

node_bin = os.environ.get("NODE_BIN") or shutil.which("node")
java_bin = os.environ.get("JAVA_BIN") or shutil.which("java")

if not node_bin:
    raise RuntimeError("NODE_BIN or node is required for sign.py")
if not java_bin:
    raise RuntimeError("JAVA_BIN or java is required for sign.py")

if os.environ.get("HAP_SIGN_TOOL_JAR"):
    jar_path = Path(os.environ["HAP_SIGN_TOOL_JAR"]).resolve()
else:
    jar_path = Path(os.environ["OHOS_SDK"]).resolve() / "toolchains" / "lib" / "hap-sign-tool.jar"

sign_js = Path("sign.js").resolve()
key_pwd = decode_password("keyPassword", config, material_dir, node_bin, sign_js)
keystore_pwd = decode_password("storePassword", config, material_dir, node_bin, sign_js)

cmd = [
    java_bin,
    "-jar",
    maybe_windows_path(jar_path),
    "sign-app",
    "-keyAlias",
    config["keyAlias"],
    "-signAlg",
    config["signAlg"],
    "-mode",
    "localSign",
    "-appCertFile",
    maybe_windows_path(cert_path),
    "-profileFile",
    maybe_windows_path(profile_file),
    "-inFile",
    maybe_windows_path(in_file),
    "-keystoreFile",
    maybe_windows_path(store_path),
    "-outFile",
    maybe_windows_path(out_file),
    "-keyPwd",
    key_pwd,
    "-keystorePwd",
    keystore_pwd,
]

print(" ".join(cmd))
subprocess.run(cmd, check=True)
