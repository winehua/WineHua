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


GSTREAMER_VERSION = "1.24.4"
MONO_VERSION = "11.1.0"
MONO_SHA256 = "deb0341431f8260b209fff6bc79ddcc5414b97f8e9236ab9fbdca4ce59e0a9b9"
DXVK_LEGACY_VERSION = "1.10.3"
DXVK_MODERN_VERSION = "2.6.2"
VKD3D_VERSION = "2.6"
REQUIRED_PLUGINS = (
    "libgstcoreelements.so",
    "libgsttypefindfunctions.so",
    "libgstplayback.so",
    "libgstisomp4.so",
    "libgstmatroska.so",
    "libgstasf.so",
    "libgstvideoparsersbad.so",
    "libgstlibav.so",
)
# The locked GStreamer 1.24.4 build supplies this complete plugin inventory.
# Checking only the playback baseline above would allow a partially staged
# sysroot to pass while removing formats a game may discover at runtime.
EXPECTED_GSTREAMER_PLUGINS = frozenset((
    "libgstadaptivedemux2.so",
    "libgstadder.so",
    "libgstaiff.so",
    "libgstalaw.so",
    "libgstalpha.so",
    "libgstalphacolor.so",
    "libgstapetag.so",
    "libgstapp.so",
    "libgstasf.so",
    "libgstasfmux.so",
    "libgstaudioconvert.so",
    "libgstaudiofx.so",
    "libgstaudiomixer.so",
    "libgstaudioparsers.so",
    "libgstaudiorate.so",
    "libgstaudioresample.so",
    "libgstaudiotestsrc.so",
    "libgstauparse.so",
    "libgstautodetect.so",
    "libgstavi.so",
    "libgstcoreelements.so",
    "libgstcutter.so",
    "libgstdebug.so",
    "libgstdeinterlace.so",
    "libgstdsd.so",
    "libgstdtmf.so",
    "libgsteffectv.so",
    "libgstequalizer.so",
    "libgstflv.so",
    "libgstflxdec.so",
    "libgstgio.so",
    "libgstgoom.so",
    "libgstgoom2k1.so",
    "libgsticydemux.so",
    "libgstid3demux.so",
    "libgstimagefreeze.so",
    "libgstinterleave.so",
    "libgstisomp4.so",
    "libgstlevel.so",
    "libgstlibav.so",
    "libgstmatroska.so",
    "libgstmidi.so",
    "libgstmonoscope.so",
    "libgstmpegpsdemux.so",
    "libgstmpegtsdemux.so",
    "libgstmulaw.so",
    "libgstmultifile.so",
    "libgstmultipart.so",
    "libgstnavigationtest.so",
    "libgstpbtypes.so",
    "libgstplayback.so",
    "libgstrawparse.so",
    "libgstreplaygain.so",
    "libgstrtp.so",
    "libgstrtpmanager.so",
    "libgstrtsp.so",
    "libgstshapewipe.so",
    "libgstsmpte.so",
    "libgstspectrum.so",
    "libgstsubparse.so",
    "libgsttcp.so",
    "libgsttypefindfunctions.so",
    "libgstudp.so",
    "libgstvideobox.so",
    "libgstvideoconvertscale.so",
    "libgstvideocrop.so",
    "libgstvideofilter.so",
    "libgstvideomixer.so",
    "libgstvideoparsersbad.so",
    "libgstvideorate.so",
    "libgstvideotestsrc.so",
    "libgstvolume.so",
    "libgstwavenc.so",
    "libgstwavparse.so",
    "libgstxingmux.so",
    "libgsty4menc.so",
))
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
        "contract-i386": "x86/contract-i386.exe",
        "contract-amd64": "x64-fex/contract-amd64.exe",
    },
    "steam-font": {
        "font-i386": "x86/font-contract-i386.exe",
        "font-amd64": "x64-fex/font-contract-amd64.exe",
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
    for path in ("x86/font-contract-i386.exe", "x64-fex/font-contract-amd64.exe"):
        require_equal(manifest.get("files", {}).get(path),
                      hashlib.sha256(archive.read(f"smoke/{path}")).hexdigest(),
                      f"font smoke checksum {path}")
    try:
        suites = json.loads(archive.read("smoke/suites.json").decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise ValidationError("smoke/suites.json is invalid") from exc
    if not isinstance(suites, dict) or suites.get("schemaVersion") != 1:
        raise ValidationError("smoke/suites.json has an invalid schema version")
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

    for executable, machine in (("x86/contract-i386.exe", 0x014c),
                                ("x86/font-contract-i386.exe", 0x014c),
                                ("x64-fex/font-contract-amd64.exe", 0x8664),
                                ("x64-fex/contract-amd64.exe", 0x8664)):
        data = archive.read(f"smoke/{executable}")
        if data[:2] != b"MZ" or len(data) < 64:
            raise ValidationError(f"{executable} is not a PE executable")
        pe_offset = struct.unpack_from("<I", data, 0x3c)[0]
        if pe_offset + 6 > len(data) or data[pe_offset:pe_offset + 4] != b"PE\0\0" or \
                struct.unpack_from("<H", data, pe_offset + 4)[0] != machine:
            raise ValidationError(f"{executable} has the wrong PE Machine")

    amd64_executable = "x64-fex/winehua_platform_process_smoke.exe"
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
    for test_id, executable, peer_exe, peer_arch in (
        ("steam-process-amd64-fex", amd64_executable,
         "C:/smoke/x86/winehua_platform_process_smoke.exe", "x86"),
        ("steam-process-i386", "x86/winehua_platform_process_smoke.exe",
         "C:/smoke/x64-fex/winehua_platform_process_smoke.exe", "x86_64"),
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


def validate_graphics_runtime_payload(archive: Archive, wine_arch: str) -> None:
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


def validate_runtime_payload(archive: Archive) -> RuntimeInfo:
    manifest_name = "runtime-components.json"
    manifest = json.loads(archive.read(manifest_name).decode("utf-8"))
    if not isinstance(manifest, dict):
        raise ValidationError("runtime-components.json must contain an object")

    require_equal(manifest.get("schemaVersion"), 1, "component schema version")
    wine_arch = manifest.get("wineArch")
    if wine_arch not in {"aarch64", "x86_64"}:
        raise ValidationError(f"unsupported wine architecture in component manifest: {wine_arch!r}")
    pe_arch = "aarch64-windows" if wine_arch == "aarch64" else "x86_64-windows"

    gstreamer = manifest.get("gstreamer")
    if not isinstance(gstreamer, dict):
        raise ValidationError("component manifest has no gstreamer object")
    require_equal(gstreamer.get("version"), GSTREAMER_VERSION, "GStreamer version")
    plugin_directory = f"bin/{wine_arch}-unix/gstreamer-1.0"
    require_equal(gstreamer.get("pluginDirectory"), plugin_directory, "GStreamer plugin directory")
    plugin_prefix = f"{plugin_directory}/"
    packaged_plugins = frozenset(
        name[len(plugin_prefix):]
        for name in archive.names
        if name.startswith(plugin_prefix) and name.endswith(".so")
    )
    require_equal(gstreamer.get("pluginCount"), len(packaged_plugins), "GStreamer plugin count")
    require_equal(packaged_plugins, EXPECTED_GSTREAMER_PLUGINS,
                  "GStreamer plugin inventory")
    declared_plugins = gstreamer.get("requiredPlugins")
    if not isinstance(declared_plugins, list):
        raise ValidationError("GStreamer requiredPlugins must be an array")
    require_members(declared_plugins, REQUIRED_PLUGINS, "declared baseline GStreamer plugins")
    require_members(packaged_plugins, REQUIRED_PLUGINS, "packaged baseline GStreamer plugins")
    for plugin in packaged_plugins:
        archive.require_file(f"{plugin_prefix}{plugin}")

    for windows_arch in (pe_arch, "i386-windows"):
        for filename in REQUIRED_WINDOWS_CORE_FILES:
            archive.require_file(f"bin/{windows_arch}/{filename}")
        for filename in REQUIRED_WINDOWS_MEDIA_FILES:
            archive.require_file(f"bin/{windows_arch}/{filename}")
        for filename in REQUIRED_WINDOWS_NETWORK_FILES:
            archive.require_file(f"bin/{windows_arch}/{filename}")
    validate_platform_smoke_suites(archive)
    validate_graphics_runtime_payload(archive, wine_arch)
    if wine_arch == "aarch64":
        for filename in REQUIRED_ARM64_FEX_FILES:
            archive.require_file(f"bin/{pe_arch}/{filename}")

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
        for windows_arch in (pe_arch, "i386-windows"):
            archive.require_file(f"bin/{windows_arch}/appwiz.cpl")
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
    require_equal(gecko.get("status"), "not-bundled", "Wine Gecko status")
    if not isinstance(gecko.get("reason"), str) or not gecko["reason"]:
        raise ValidationError("Wine Gecko omission must have a reason")

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


def validate_hap(path: Path) -> RuntimeInfo:
    hap = Archive(path)
    try:
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
        if not isinstance(runtime_manifest.get("smokeSuiteVersion"), str) or not runtime_manifest["smokeSuiteVersion"]:
            raise ValidationError("HAP runtime manifest has no smoke suite version")

        payload = Archive(io.BytesIO(payload_bytes))
        try:
            info = validate_runtime_payload(payload)
            validate_media_dependency_closure(payload, hap, info.wine_arch)
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
    args = parser.parse_args()

    try:
        if args.payload:
            archive = Archive(args.payload)
            try:
                info = validate_runtime_payload(archive)
            finally:
                archive.close()
            print(f"runtime payload PASS: wineArch={info.wine_arch}")
        else:
            info = validate_hap(args.hap)
            print(f"HAP runtime closure PASS: wineArch={info.wine_arch}")
    except (OSError, ValueError, ValidationError) as exc:
        raise SystemExit(f"runtime component validation failed: {exc}") from exc


if __name__ == "__main__":
    main()
