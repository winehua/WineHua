#!/usr/bin/env python3
"""Validate the Wine media/runtime closure in wine-data.zip or a signed HAP."""

import argparse
import hashlib
import io
import json
import struct
from dataclasses import dataclass
from pathlib import Path
from typing import FrozenSet, Iterable, Union
from zipfile import ZipFile

from runtime_library_guard import RuntimeLibraryError, validate_library, validate_thread_exit_metadata
from dxvk_runtime_contract import validate_modern_runtime
from wined3d_runtime_contract import validate_runtime as validate_wined3d_runtime
from package_texture_compat_runtime import validate as validate_texture_runtime
from runtime_core_contract import validate_core
from stamp_wine_builtin_identity import validate_archive as validate_builtin_identity
from wine_gecko import validate_payload as validate_gecko_payload


from gstreamer_inventory import (VERSION as GSTREAMER_VERSION, PLUGINS,
    REQUIRED_PLUGINS, REQUIRED_ELEMENTS, fingerprint, check_elf)
MONO_VERSION = "10.4.1"
MONO_SHA256 = "071f4b2887e1c97a11d791ff3d65be9429eed6dec4c2708888bfd546ba358e23"
DXVK_LEGACY_VERSION = "1.10.3"
DXVK_MODERN_VERSION = "2.6.2"
VKD3D_VERSION = "2.6"
EXPECTED_GSTREAMER_PLUGINS = frozenset(PLUGINS)
REQUIRED_UNIX_LIBRARIES = (
    "libglib-2.0.so.0",
    "libgstreamer-1.0.so.0",
    "libgstbase-1.0.so.0",
    "libgstvideo-1.0.so.0",
    "libgstaudio-1.0.so.0",
    "libgsttag-1.0.so.0",
    "libgstcodecparsers-1.0.so.0",
    "libgstmpegts-1.0.so.0",
    "libavcodec.so.60",
    "libavformat.so.60",
    "libavutil.so.58",
    "libintl.so",
)
REQUIRED_PLATFORM_UNIX_LIBRARIES = (
    "libfreetype.so.6",
    "libffi.so.8",
    "libwayland-client.so.0",
    "libwayland-egl.so.1",
    "libxkbcommon.so.0",
    "libxkbregistry.so.0",
    "libgnutls.so.30",
    "libnettle.so.8",
    "libhogweed.so.6",
    "libgmp.so.10",
    "libtasn1.so.6",
    "libunistring.so.5",
)
REQUIRED_XKB_DATA_FILES = (
    "share/X11/xkb/keycodes/evdev",
    "share/X11/xkb/rules/evdev",
    "share/X11/xkb/symbols/us",
    "share/X11/xkb/types/basic",
)
REQUIRED_WINDOWS_CORE_FILES = (
    "kernel32.dll",
    "ntdll.dll",
    "user32.dll",
)
REQUIRED_WINDOWS_MEDIA_FILES = (
    "winegstreamer.dll",
    "mf.dll",
    "mfplat.dll",
    "mfreadwrite.dll",
    "quartz.dll",
    "devenum.dll",
    "wmvcore.dll",
)
REQUIRED_WINDOWS_NETWORK_FILES = (
    "crypt32.dll",
    "dnsapi.dll",
    "schannel.dll",
    "winhttp.dll",
    "wininet.dll",
)
REQUIRED_PLATFORM_SMOKE_TESTS = {
    "platform-network": {
        "platform-network-x64": "x64/winehua_platform_network.exe",
        "platform-network-x86": "x86/winehua_platform_network.exe",
    },
    "platform-process": {
        "platform-process-x64": "x64/winehua_platform_process_smoke.exe",
        "platform-process-x86": "x86/winehua_platform_process_smoke.exe",
    },
    "steam-contract": {
        "contract-x86": "x86/contract_i386.exe",
        "contract-x64": "x64-fex/contract_amd64.exe",
    },
    "steam-font": {
        "font-x86": "x86/font_contract_i386.exe",
        "font-x64": "x64-fex/font_contract_amd64.exe",
    },
}
REQUIRED_ARM64_FEX_FILES = (
    "libarm64ecfex.dll",
    "libwow64fex.dll",
)
REQUIRED_WINE_MEDIA_UNIX_LIBRARY = "winegstreamer.so"
PT_LOAD = 1
PT_DYNAMIC = 2
DT_NULL = 0
DT_NEEDED = 1
DT_STRTAB = 5
DT_STRSZ = 10


class ValidationError(RuntimeError):
    pass


@dataclass(frozen=True)
class RuntimeInfo:
    wine_arch: str
    component_names: FrozenSet[str]


class Archive:
    def __init__(self, source: Union[str, Path, io.BytesIO]) -> None:
        self.zip = ZipFile(source)
        self.names = tuple(self.zip.namelist())
        self.name_set = frozenset(self.names)

    def close(self) -> None:
        self.zip.close()

    def require_file(self, name: str) -> None:
        if name not in self.name_set:
            raise ValidationError(f"missing required file: {name}")
        if self.zip.getinfo(name).file_size == 0:
            raise ValidationError(f"required file is empty: {name}")

    def read(self, name: str) -> bytes:
        self.require_file(name)
        return self.zip.read(name)


def require_equal(actual: object, expected: object, name: str) -> None:
    if actual != expected:
        raise ValidationError(f"{name}: expected {expected!r}, got {actual!r}")


def require_members(names: Iterable[str], required: Iterable[str], description: str) -> None:
    present = set(names)
    missing = sorted(set(required) - present)
    if missing:
        raise ValidationError(f"missing {description}: {', '.join(missing)}")


def is_shared_object(name: str) -> bool:
    base = name.rsplit("/", 1)[-1]
    return ".so" in base and not base.endswith(".json")


def elf_needed(label: str, data: bytes) -> tuple[str, ...]:
    """Return ELF DT_NEEDED names, or no names for non-ELF files."""
    if data[:4] != b"\x7fELF":
        return ()
    if len(data) < 64 or data[5] not in {1, 2}:
        raise ValidationError(f"invalid ELF header: {label}")
    endian = "<" if data[5] == 1 else ">"
    elf_class = data[4]
    try:
        if elf_class == 1:
            header = struct.unpack_from(endian + "HHIIIIIHHHHHH", data, 16)
            program_offset, program_size, program_count = header[4], header[8], header[9]
            program_format = endian + "IIIIIIII"
            dynamic_format = endian + "iI"
        elif elf_class == 2:
            header = struct.unpack_from(endian + "HHIQQQIHHHHHH", data, 16)
            program_offset, program_size, program_count = header[4], header[8], header[9]
            program_format = endian + "IIQQQQQQ"
            dynamic_format = endian + "qQ"
        else:
            raise ValidationError(f"unsupported ELF class in {label}")
    except struct.error as exc:
        raise ValidationError(f"truncated ELF header: {label}") from exc

    expected_program_size = struct.calcsize(program_format)
    if program_size != expected_program_size or program_offset + program_size * program_count > len(data):
        raise ValidationError(f"invalid ELF program headers: {label}")

    load_segments: list[tuple[int, int, int]] = []
    dynamic_segments: list[tuple[int, int]] = []
    for index in range(program_count):
        offset = program_offset + index * program_size
        values = struct.unpack_from(program_format, data, offset)
        if elf_class == 1:
            segment_type, file_offset, virtual_address, _physical, file_size, _memory, _flags, _align = values
        else:
            segment_type, _flags, file_offset, virtual_address, _physical, file_size, _memory, _align = values
        if file_offset + file_size > len(data):
            raise ValidationError(f"truncated ELF segment: {label}")
        if segment_type == PT_LOAD:
            load_segments.append((file_offset, virtual_address, file_size))
        elif segment_type == PT_DYNAMIC:
            dynamic_segments.append((file_offset, file_size))

    if not dynamic_segments:
        return ()
    entry_size = struct.calcsize(dynamic_format)
    string_address: int | None = None
    string_size: int | None = None
    needed_offsets: list[int] = []
    for file_offset, file_size in dynamic_segments:
        if file_size % entry_size:
            raise ValidationError(f"misaligned ELF dynamic segment: {label}")
        for offset in range(file_offset, file_offset + file_size, entry_size):
            tag, value = struct.unpack_from(dynamic_format, data, offset)
            if tag == DT_NULL:
                break
            if tag == DT_NEEDED:
                needed_offsets.append(value)
            elif tag == DT_STRTAB:
                string_address = value
            elif tag == DT_STRSZ:
                string_size = value
    if not needed_offsets:
        return ()
    if string_address is None or string_size is None:
        raise ValidationError(f"ELF DT_NEEDED has no string table: {label}")
    string_offset: int | None = None
    for file_offset, virtual_address, file_size in load_segments:
        if virtual_address <= string_address < virtual_address + file_size:
            string_offset = file_offset + string_address - virtual_address
            break
    if string_offset is None or string_offset + string_size > len(data):
        raise ValidationError(f"ELF dynamic string table is outside PT_LOAD: {label}")

    needed: list[str] = []
    for needed_offset in needed_offsets:
        start = string_offset + needed_offset
        end = data.find(b"\0", start, string_offset + string_size)
        if start >= string_offset + string_size or end < 0:
            raise ValidationError(f"invalid ELF DT_NEEDED string: {label}")
        needed.append(data[start:end].decode("ascii"))
    return tuple(needed)


def validate_media_dependency_closure(payload: Archive, hap: Archive, wine_arch: str) -> None:
    unix_prefix = f"bin/{wine_arch}-unix/"
    payload_libraries = [name for name in payload.names if name.startswith(unix_prefix) and is_shared_object(name)]
    hap_libraries = [name for name in hap.names if "/libs/" in f"/{name}" and is_shared_object(name)]
    provided = {name.rsplit("/", 1)[-1] for name in payload_libraries + hap_libraries}

    media_consumers = payload_libraries + [
        name for name in hap_libraries if name.rsplit("/", 1)[-1] == REQUIRED_WINE_MEDIA_UNIX_LIBRARY
        or name.rsplit("/", 1)[-1] in EXPECTED_GSTREAMER_PLUGINS
    ]
    if not any(name.rsplit("/", 1)[-1] == REQUIRED_WINE_MEDIA_UNIX_LIBRARY for name in media_consumers):
        raise ValidationError(f"missing HAP Wine media library: {REQUIRED_WINE_MEDIA_UNIX_LIBRARY}")

    missing: dict[str, list[str]] = {}
    for name in media_consumers:
        source = payload if name in payload.name_set else hap
        unresolved = sorted(set(elf_needed(name, source.read(name))) - provided)
        if unresolved:
            missing[name] = unresolved
    if missing:
        details = "; ".join(f"{name}: {', '.join(values)}" for name, values in sorted(missing.items()))
        raise ValidationError(f"unresolved Wine media ELF dependencies: {details}")


def validate_platform_smoke_suites(archive: Archive) -> None:
    manifest = json.loads(archive.read("smoke/manifest.json").decode("utf-8"))
    for path in ("x86/font_contract_i386.exe", "x64-fex/font_contract_amd64.exe"):
        require_equal(manifest.get("files", {}).get(path),
                      hashlib.sha256(archive.read(f"smoke/{path}")).hexdigest(),
                      f"font smoke checksum {path}")
    try:
        suites = json.loads(archive.read("smoke/suites.json").decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise ValidationError("smoke/suites.json is invalid") from exc
    # 套件文档由 automation/smoke.py 生成，版本键是 suiteVersion（含载荷内容
    # 哈希，形如 smoke-v2-<hash>），不再有独立的 schemaVersion
    if not isinstance(suites, dict) or \
            not str(suites.get("suiteVersion", "")).startswith("smoke-v2-"):
        raise ValidationError("smoke/suites.json has an invalid suite version")
    suite_definitions = suites.get("suites")
    if not isinstance(suite_definitions, dict):
        raise ValidationError("smoke/suites.json has no suites object")

    for suite_name, expected_tests in REQUIRED_PLATFORM_SMOKE_TESTS.items():
        suite = suite_definitions.get(suite_name)
        if not isinstance(suite, dict) or not isinstance(suite.get("tests"), list):
            raise ValidationError(f"missing platform smoke suite: {suite_name}")
        declared = {
            item.get("testId"): item
            for item in suite["tests"]
            if isinstance(item, dict) and isinstance(item.get("testId"), str)
        }
        for test_id, executable in expected_tests.items():
            test = declared.get(test_id)
            if not isinstance(test, dict):
                raise ValidationError(f"missing platform smoke test: {test_id}")
            require_equal(test.get("exe"), executable, f"platform smoke executable {test_id}")
            archive.require_file(f"smoke/{executable}")

    for executable, machine in (("x86/contract_i386.exe", 0x014c),
                                ("x86/font_contract_i386.exe", 0x014c),
                                ("x64-fex/font_contract_amd64.exe", 0x8664),
                                ("x64-fex/contract_amd64.exe", 0x8664)):
        data = archive.read(f"smoke/{executable}")
        if data[:2] != b"MZ" or len(data) < 64:
            raise ValidationError(f"{executable} is not a PE executable")
        pe_offset = struct.unpack_from("<I", data, 0x3c)[0]
        if pe_offset + 6 > len(data) or data[pe_offset:pe_offset + 4] != b"PE\0\0" or \
                struct.unpack_from("<H", data, pe_offset + 4)[0] != machine:
            raise ValidationError(f"{executable} has the wrong PE Machine")

    amd64_executable = "x64-fex/steam_process_amd64.exe"
    archive.require_file(f"smoke/{amd64_executable}")
    amd64_data = archive.read(f"smoke/{amd64_executable}")
    if amd64_data[:2] != b"MZ" or len(amd64_data) < 64:
        raise ValidationError("Steam AMD64 smoke is not a PE executable")
    pe_offset = struct.unpack_from("<I", amd64_data, 0x3c)[0]
    if pe_offset + 6 > len(amd64_data) or amd64_data[pe_offset:pe_offset + 4] != b"PE\0\0" or \
            struct.unpack_from("<H", amd64_data, pe_offset + 4)[0] != 0x8664:
        raise ValidationError("Steam AMD64 smoke has the wrong PE Machine")
    compare_suite = suite_definitions.get("steam-arch-compare")
    if not isinstance(compare_suite, dict) or not isinstance(compare_suite.get("tests"), list):
        raise ValidationError("missing Steam architecture comparison suite")
    compare_tests = {test.get("testId"): test for test in compare_suite["tests"] if isinstance(test, dict)}
    # 两个方向的 peer 都是固定的真 x86_64 PE / i386 PE（steam_process_amd64.exe
    # 无论哪个构建都用 llvm-x86_64 编），故 peer 架构是常量
    for test_id, executable, peer_exe, peer_arch in (
        ("steam-process-amd64-x64", amd64_executable,
         "C:/smoke/x86/winehua_platform_process_smoke.exe", "x86"),
        ("steam-process-i386-x86", "x86/winehua_platform_process_smoke.exe",
         "C:/smoke/x64-fex/steam_process_amd64.exe", "x86_64"),
    ):
        test = compare_tests.get(test_id)
        if not isinstance(test, dict):
            raise ValidationError(f"missing Steam architecture test: {test_id}")
        require_equal(test.get("exe"), executable, f"Steam architecture executable {test_id}")
        environment = test.get("env")
        if not isinstance(environment, dict):
            raise ValidationError(f"Steam architecture test has no environment: {test_id}")
        require_equal(environment.get("WINEHUA_PEER_EXE"), peer_exe,
                      f"Steam architecture peer executable {test_id}")
        require_equal(environment.get("WINEHUA_PEER_ARCH"), peer_arch,
                      f"Steam architecture peer architecture {test_id}")

    process_suite = suite_definitions["platform-process"]
    for test_id, peer_exe, peer_arch in (
        ("platform-process-x64", "C:/smoke/x86/winehua_platform_process_smoke.exe", "x86"),
        ("platform-process-x86", "C:/smoke/x64/winehua_platform_process_smoke.exe", None),
    ):
        test = next(item for item in process_suite["tests"] if item.get("testId") == test_id)
        environment = test.get("env")
        if not isinstance(environment, dict):
            raise ValidationError(f"platform process smoke has no environment: {test_id}")
        require_equal(environment.get("WINEHUA_PEER_EXE"), peer_exe,
                      f"platform process peer executable {test_id}")
        if peer_arch is not None:
            require_equal(environment.get("WINEHUA_PEER_ARCH"), peer_arch,
                          f"platform process peer architecture {test_id}")
        elif environment.get("WINEHUA_PEER_ARCH") not in {"arm64", "x86_64"}:
            raise ValidationError(f"invalid platform process peer architecture: {test_id}")


def validate_graphics_runtime_payload(archive: Archive, wine_arch: str, candidate=None) -> None:
    """Check that all packaged graphics profiles match the product policy."""
    required_files = [
        "vkd3d/limited-500k/x64/d3d12.dll",
    ]
    for profile in ("legacy", "modern-2.6"):
        for architecture in ("x86", "x64"):
            for filename in ("d3d11.dll", "dxgi.dll"):
                required_files.append(f"dxvk/{profile}/{architecture}/{filename}")
    if wine_arch == "aarch64":
        for profile in ("legacy", "modern-2.6"):
            for filename in ("d3d11.dll", "dxgi.dll"):
                required_files.append(f"dxvk/{profile}/arm64x/{filename}")
    require_members(archive.names, required_files, "DXVK/VKD3D graphics runtime files")

    try:
        dxvk = json.loads(archive.read("dxvk/manifest.json").decode("utf-8"))
        vkd3d = json.loads(archive.read("vkd3d/manifest.json").decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise ValidationError("graphics runtime manifest is invalid") from exc
    if not isinstance(dxvk, dict) or not isinstance(vkd3d, dict):
        raise ValidationError("graphics runtime manifests must contain objects")

    require_equal(dxvk.get("schemaVersion"), 2, "DXVK manifest schema version")
    require_equal(dxvk.get("backend"), "dxvk", "DXVK backend")
    require_equal(dxvk.get("defaultProfile"), "legacy", "DXVK default profile")
    runtimes = dxvk.get("runtimes")
    if not isinstance(runtimes, dict):
        raise ValidationError("DXVK manifest has no runtimes object")
    for profile, version, state in (
        ("legacy", DXVK_LEGACY_VERSION, "stable"),
        ("modern-2.6", DXVK_MODERN_VERSION, "adapted-game-validated-capability-gated"),
    ):
        runtime = runtimes.get(profile)
        if not isinstance(runtime, dict):
            raise ValidationError(f"DXVK manifest has no {profile} runtime")
        require_equal(runtime.get("version"), version, f"DXVK {profile} version")
        require_equal(runtime.get("state"), state, f"DXVK {profile} state")

    if wine_arch == 'aarch64':
        try:
            validate_modern_runtime(archive, candidate=candidate)
        except (KeyError, ValueError) as exc:
            raise ValidationError(str(exc)) from exc

    require_equal(vkd3d.get("schemaVersion"), 1, "VKD3D manifest schema version")
    require_equal(vkd3d.get("profile"), "limited-500k", "VKD3D profile")
    require_equal(vkd3d.get("version"), VKD3D_VERSION, "VKD3D version")
    require_equal(vkd3d.get("defaultEnabled"), False, "VKD3D product enablement")
    files = vkd3d.get("files")
    if not isinstance(files, dict):
        raise ValidationError("VKD3D manifest has no files object")
    expected_sha256 = files.get("x64/d3d12.dll")
    if not isinstance(expected_sha256, str) or len(expected_sha256) != 64:
        raise ValidationError("VKD3D manifest has no d3d12 checksum")
    actual_sha256 = hashlib.sha256(archive.read("vkd3d/limited-500k/x64/d3d12.dll")).hexdigest()
    require_equal(actual_sha256, expected_sha256, "VKD3D d3d12 payload checksum")


def validate_gstreamer(payload: Archive, wine_arch: str, hap: Archive | None = None) -> None:
    manifest = json.loads(payload.read("runtime-components.json"))
    require_equal(manifest.get("schemaVersion"), 2, "component schema version")
    gst = manifest.get("gstreamer")
    if not isinstance(gst, dict):
        raise ValidationError("component manifest has no gstreamer object")
    native_arch = gst.get("nativeArch")
    if native_arch not in {"arm64-v8a", "x86_64"} or (wine_arch == "aarch64" and native_arch != "arm64-v8a"):
        raise ValidationError(f"invalid GStreamer native architecture: {native_arch!r}")
    box64 = wine_arch == "x86_64" and native_arch == "arm64-v8a"
    require_equal(gst.get("version"), GSTREAMER_VERSION, "GStreamer version")
    require_equal(gst.get("storage"), "payload" if box64 else "native-libs", "GStreamer plugin storage")
    directory = f"bin/{wine_arch}-unix/gstreamer-1.0" if box64 else f"libs/{native_arch}"
    require_equal(gst.get("pluginDirectory"), directory, "GStreamer plugin directory")
    require_equal(gst.get("pluginCount"), len(PLUGINS), "GStreamer plugin count")
    require_equal(gst.get("requiredPlugins"), list(REQUIRED_PLUGINS), "required GStreamer plugins")
    require_equal(gst.get("requiredElements"), list(REQUIRED_ELEMENTS), "required GStreamer elements")
    files = gst.get("files")
    if not isinstance(files, dict):
        raise ValidationError("GStreamer manifest has no plugin checksums")
    require_equal(set(files), set(PLUGINS), "GStreamer plugin inventory")
    for name, digest in files.items():
        if not isinstance(digest, str) or len(digest) != 64 or any(c not in "0123456789abcdef" for c in digest):
            raise ValidationError(f"invalid GStreamer checksum: {name}")
    require_equal(gst.get("inventorySha256"), fingerprint(files), "GStreamer inventory fingerprint")
    if not box64:
        stale = [name for name in payload.names if name.startswith(f"bin/{wine_arch}-unix/gstreamer-1.0/") and name.endswith(".so")]
        if stale:
            raise ValidationError("native GStreamer plugins must be in HAP native libs, found el2 payload copies")
    source = payload if box64 else hap
    if source is None:
        return  # Native files are outside wine-data.zip; validate_hap must check them.
    packaged = {name.rsplit("/", 1)[-1] for name in source.names
                if name.startswith(directory + "/") and name.rsplit("/", 1)[-1] in PLUGINS}
    require_equal(packaged, set(PLUGINS), "packaged GStreamer plugin inventory")
    for name in PLUGINS:
        path = f"{directory}/{name}"
        data = source.read(path)
        require_equal(hashlib.sha256(data).hexdigest(), files[name], f"GStreamer plugin checksum {path}")
        try:
            check_elf(data, wine_arch, path)
        except ValueError as error:
            raise ValidationError(str(error)) from error
        if any("/" in dependency for dependency in elf_needed(path, data)):
            raise ValidationError(f"GStreamer plugin has an absolute DT_NEEDED: {path}")
    if not box64:
        # A rawfile copy cannot satisfy a native plugin's DT_NEEDED dependency.
        pending = list(PLUGINS)
        visited = set()
        while pending:
            name = pending.pop()
            if name in visited:
                continue
            visited.add(name)
            path = f"{directory}/{name}"
            data = hap.read(path)
            try:
                check_elf(data, wine_arch, path)
            except ValueError as error:
                raise ValidationError(str(error)) from error
            needed = elf_needed(path, data)
            if any("/" in dependency for dependency in needed):
                raise ValidationError(f"native media library has an absolute DT_NEEDED: {path}")
            pending.extend(needed)
        marker = ("winehua-gst-inventory:" + gst["inventorySha256"]).encode()
        loader_path = f"libs/{native_arch}/winegstreamer.so"
        loader = hap.read(loader_path)
        try:
            check_elf(loader, wine_arch, loader_path)
        except ValueError as error:
            raise ValidationError(str(error)) from error
        if marker not in loader:
            raise ValidationError("winegstreamer.so does not contain the packaged plugin inventory fingerprint")


def validate_runtime_payload(archive: Archive, candidate=None) -> RuntimeInfo:
    manifest_name = "runtime-components.json"
    manifest = json.loads(archive.read(manifest_name).decode("utf-8"))
    if not isinstance(manifest, dict):
        raise ValidationError("runtime-components.json must contain an object")

    require_equal(manifest.get("schemaVersion"), 2, "component schema version")
    wine_arch = manifest.get("wineArch")
    if wine_arch not in {"aarch64", "x86_64"}:
        raise ValidationError(f"unsupported wine architecture in component manifest: {wine_arch!r}")
    pe_arch = "aarch64-windows" if wine_arch == "aarch64" else "x86_64-windows"

    validate_gstreamer(archive, wine_arch)

    for windows_arch in (pe_arch, "i386-windows"):
        for filename in REQUIRED_WINDOWS_CORE_FILES:
            archive.require_file(f"bin/{windows_arch}/{filename}")
        for filename in REQUIRED_WINDOWS_MEDIA_FILES:
            archive.require_file(f"bin/{windows_arch}/{filename}")
        for filename in REQUIRED_WINDOWS_NETWORK_FILES:
            archive.require_file(f"bin/{windows_arch}/{filename}")
    validate_graphics_runtime_payload(archive, wine_arch, candidate)
    if wine_arch == 'aarch64':
        try:
            validate_wined3d_runtime(json.loads(archive.read('wined3d-graphics.json')), archive.read)
            validate_texture_runtime(archive, archive.read('bin/guest_vulkan/lib/libvulkan_virtio.so'), candidate=candidate)
            validate_core(archive.read, 'payload', candidate)
            if candidate is not None:
                graphics = json.loads(archive.read('wined3d-graphics.json'))
                require_equal({k: graphics[k] for k in ('patches', 'recipe', 'sourceFiles')},
                              candidate['sources']['wineD3D'], 'candidate effective WineD3D sources')
            validate_builtin_identity(archive.zip)
        except (KeyError, ValueError, TypeError) as error:
            raise ValidationError('Runtime compatibility contract: ' + str(error)) from error
    if wine_arch == "aarch64":
        for filename in REQUIRED_ARM64_FEX_FILES:
            archive.require_file(f"bin/{pe_arch}/{filename}")

    if "fex-simd-zero.json" in archive.name_set:
        from fex_simd_runtime import validate as validate_fex_simd
        try:
            validate_fex_simd(json.loads(archive.read("fex-simd-zero.json")), archive.read)
        except (ValueError, KeyError, TypeError, IndexError, struct.error) as error:
            raise ValidationError("FEX SIMD repair identity invalid: " + str(error)) from error

    if "icu-compat.json" in archive.name_set:
        from icu_compat_runtime import validate as validate_icu_compat
        try:
            validate_icu_compat(json.loads(archive.read("icu-compat.json")), archive.read)
        except (ValueError, KeyError, TypeError) as error:
            raise ValidationError("ICU runtime identity invalid: " + str(error)) from error

    mono = manifest.get("mono")
    if not isinstance(mono, dict):
        raise ValidationError("component manifest has no mono object")
    mono_path = f"share/wine/mono/wine-mono-{MONO_VERSION}-x86.msi"
    require_equal(mono.get("version"), MONO_VERSION, "Wine Mono version")
    require_equal(mono.get("msi"), mono_path, "Wine Mono path")
    mono_status = mono.get("status")
    if mono_status == "bundled":
        require_equal(mono.get("sha256"), MONO_SHA256, "Wine Mono manifest checksum")
        actual_mono_sha256 = hashlib.sha256(archive.read(mono_path)).hexdigest()
        require_equal(actual_mono_sha256, MONO_SHA256, "Wine Mono payload checksum")
        for windows_arch in {pe_arch, "i386-windows"}:
            appwiz_path = f"bin/{windows_arch}/appwiz.cpl"
            mscoree_path = f"bin/{windows_arch}/mscoree.dll"
            archive.require_file(appwiz_path)
            archive.require_file(mscoree_path)
            filename = f"wine-mono-{MONO_VERSION}-x86.msi".encode("utf-16le")
            if filename not in archive.read(appwiz_path):
                raise ValidationError(f"{appwiz_path} expects a different Wine Mono MSI")
            data = archive.read(mscoree_path)
            if MONO_VERSION.encode() not in data and MONO_VERSION.encode("utf-16le") not in data:
                raise ValidationError(f"{mscoree_path} expects a different Wine Mono version")
    elif mono_status == "disabled":
        require_equal(mono.get("sha256"), "", "disabled Wine Mono checksum")
        if mono_path in archive.name_set:
            raise ValidationError("Wine Mono MSI is present although component manifest marks it disabled")
        for windows_arch in (pe_arch, "i386-windows"):
            if f"bin/{windows_arch}/appwiz.cpl" in archive.name_set:
                raise ValidationError(
                    "appwiz.cpl is present although component manifest marks Wine Mono disabled"
                )
    else:
        raise ValidationError(f"invalid Wine Mono status: {mono_status!r}")

    fonts = manifest.get("fonts")
    if not isinstance(fonts, dict):
        raise ValidationError("component manifest has no fonts object")
    font_names = tuple(
        name for name in archive.names
        if name.startswith("share/wine/fonts/") and name.endswith(".ttf")
    )
    if not font_names:
        raise ValidationError("Wine font payload is empty")
    require_equal(fonts.get("ttfCount"), len(font_names), "Wine font count")
    require_members(archive.names, REQUIRED_XKB_DATA_FILES, "XKB keyboard data")

    gecko = manifest.get("gecko")
    if not isinstance(gecko, dict):
        raise ValidationError("component manifest has no gecko object")
    try:
        validate_gecko_payload(archive.name_set, archive.read, gecko)
    except (ValueError, KeyError) as error:
        raise ValidationError("Wine Gecko runtime invalid: " + str(error)) from error

    return RuntimeInfo(wine_arch=wine_arch, component_names=archive.name_set)


def find_payload_name(hap: Archive) -> str:
    candidates = [name for name in hap.names if name.endswith("resources/rawfile/wine-data.zip")]
    if len(candidates) != 1:
        raise ValidationError(f"expected one HAP wine-data payload, found {len(candidates)}")
    return candidates[0]


def find_runtime_manifest_name(hap: Archive) -> str:
    candidates = [name for name in hap.names if name.endswith("resources/rawfile/wine-runtime-manifest.json")]
    if len(candidates) != 1:
        raise ValidationError(f"expected one HAP runtime manifest, found {len(candidates)}")
    return candidates[0]


def validate_hap(path: Path, source_manifest=None) -> RuntimeInfo:
    hap = Archive(path)
    try:
        try:
            validate_thread_exit_metadata(hap.zip, str(path))
        except RuntimeLibraryError as error:
            raise ValidationError(str(error)) from error
        payload_name = find_payload_name(hap)
        payload_bytes = hap.read(payload_name)
        runtime_manifest = json.loads(hap.read(find_runtime_manifest_name(hap)).decode("utf-8"))
        if not isinstance(runtime_manifest, dict):
            raise ValidationError("HAP runtime manifest must contain an object")
        require_equal(runtime_manifest.get("schemaVersion"), 1, "HAP runtime manifest schema version")
        require_equal(runtime_manifest.get("payload"), "wine-data.zip", "HAP runtime manifest payload")
        require_equal(
            runtime_manifest.get("payloadSha256"),
            hashlib.sha256(payload_bytes).hexdigest(),
            "HAP runtime manifest payload checksum",
        )

        payload = Archive(io.BytesIO(payload_bytes))
        try:
            candidate = None
            if source_manifest is not None:
                from source_build_contract import load
                candidate = load(payload.read, source_manifest)
                from source_build_contract import validate_member
                for name in candidate['libraries']:
                    validate_member(candidate, 'libraries', name, hap.read(name))
                require_equal(runtime_manifest.get('validationPolicy'), 'source-build-candidate',
                              'candidate validation policy')
            info = validate_runtime_payload(payload, candidate)
            if info.wine_arch == 'aarch64':
                require_equal(runtime_manifest.get('wineD3DGraphics'),
                              json.loads(payload.read('wined3d-graphics.json')),
                              'HAP WineD3D graphics contract')
                try:
                    validate_texture_runtime(payload, hap.read('libs/arm64-v8a/libvulkan_virtio.so'), candidate=candidate)
                    validate_core(hap.read, candidate=candidate)
                    dxvk = json.loads(payload.read('dxvk/manifest.json'))
                    require_equal(dxvk['venusRuntime']['hostVirglrendererSha256'],
                                  candidate['libraries']['libs/arm64-v8a/libvirglrenderer.so.1']['sha256']
                                  if candidate else hashlib.sha256(hap.read('libs/arm64-v8a/libvirglrenderer.so.1')).hexdigest(),
                                  'Venus host renderer identity')
                except (KeyError, ValueError, TypeError) as error:
                    raise ValidationError('Runtime compatibility contract: ' + str(error)) from error
            validate_gstreamer(payload, info.wine_arch, hap)
            validate_media_dependency_closure(payload, hap, info.wine_arch)
            audio_sources = [(archive, name) for archive in (payload, hap) for name in archive.names
                             if Path(name).name == 'wineohos.so']
            if not audio_sources:
                raise ValidationError('missing Wine audio driver wineohos.so')
            for archive, name in audio_sources:
                try:
                    validate_library(archive.read(name), name)
                except RuntimeLibraryError as error:
                    raise ValidationError(str(error)) from error
        finally:
            payload.close()

        all_library_names = set(info.component_names)
        all_library_names.update(Path(name).name for name in hap.names if "/libs/" in f"/{name}")
        require_members(all_library_names, REQUIRED_UNIX_LIBRARIES, "HAP GStreamer/FFmpeg runtime libraries")
        require_members(all_library_names, REQUIRED_PLATFORM_UNIX_LIBRARIES,
                        "HAP desktop/input/TLS runtime libraries")
        return info
    finally:
        hap.close()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--payload", type=Path, help="wine-data.zip to validate")
    source.add_argument("--hap", type=Path, help="signed HAP to validate")
    parser.add_argument('--source-build-manifest', type=Path,
                        help='Explicit source-candidate policy; requires the pre-build clean-checkout manifest')
    args = parser.parse_args()

    try:
        source_manifest = json.loads(args.source_build_manifest.read_text()) if args.source_build_manifest else None
        if args.payload:
            archive = Archive(args.payload)
            try:
                candidate = None
                if source_manifest is not None:
                    from source_build_contract import load
                    candidate = load(archive.read, source_manifest, require_native=False)
                info = validate_runtime_payload(archive, candidate)
            finally:
                archive.close()
            print(f"runtime payload PASS: wineArch={info.wine_arch}; native plugins require --hap verification")
        else:
            info = validate_hap(args.hap, source_manifest)
            print(f"HAP runtime closure PASS: wineArch={info.wine_arch}")
        if source_manifest is not None:
            print('Source-built candidate only: deviceValidated=false; pinned release acceptance not granted')
    except (OSError, ValueError, ValidationError) as exc:
        raise SystemExit(f"runtime component validation failed: {exc}") from exc


if __name__ == "__main__":
    main()
