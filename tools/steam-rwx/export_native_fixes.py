"""Export reviewed fixes separately from the temporary v16 native probe.

Does not change the active Wine source. The clean files and reversible probe
patch allow a subsequent build to select fixes with or without diagnostics.
"""
from pathlib import Path
import difflib

root = Path(__file__).resolve().parents[2]
unix = root / "thirdparty/wine-valve/dlls/ntdll/unix"
artifacts = root / "artifacts/fex-rwx-probe"
before = artifacts / "v15-source-before"
clean = artifacts / "native-fixes-source"
clean.mkdir(exist_ok=True)
patches = root / "patches/wine"


def diff(old, new, name):
    return "".join(difflib.unified_diff(old.splitlines(True), new.splitlines(True),
                                      "a/dlls/ntdll/unix/" + name,
                                      "b/dlls/ntdll/unix/" + name))


source = (unix / "ohos_virtual.c").read_text()
start = source.index("    /* Temporary probe: saved registers only,")
end = source.index("    ohos_smc_log_enter(", start)
source = source[:start] + source[end:]
start = source.find("    /* Temporary candidate-only direct stderr:")
if start != -1:
    end = source.index("    ohos_diag_quiet =", start)
    source = source[:start] + source[end:]
source = source.replace("    /* Temporary candidate-only raw signal control, using the existing fallback. */\n"
                        "    if (getenv( \"WINEHUA_CEF_NO_CRASH_HANDLER\" )) add_special = NULL;\n", "")
(clean / "ohos_virtual.c").write_text(source)
(patches / "0003-ntdll-ohos-serialize-fault-maps.patch").write_text(
    diff((before / "ohos_virtual.c").read_text(), source, "ohos_virtual.c"))

source = (unix / "virtual.c").read_text()
start = source.index("/* Temporary candidate-only TEB lifetime trace;")
end = source.index("/* set some initial values in a new TEB */", start)
source = source[:start] + source[end:]
source = "".join(line for line in source.splitlines(True) if "ohos_teb_probe(" not in line)
source = source.replace("        status = NtAllocateVirtualMemory( NtCurrentProcess(), (void **)&ptr, 0, &block_size,\n                                          MEM_COMMIT, PAGE_READWRITE );",
                        "        NtAllocateVirtualMemory( NtCurrentProcess(), (void **)&ptr, 0, &block_size,\n                                 MEM_COMMIT, PAGE_READWRITE );")
(clean / "virtual.c").write_text(source)
(patches / "0004-ntdll-skip-dead-thread-tls-during-shutdown.patch").write_text(
    diff((before / "virtual.c").read_text(), source, "virtual.c"))

source = (unix / "env.c").read_text()
for js_flag in ("--jitless", "--no-opt"):
    source = source.replace(" --noerrdialogs --js-flags=" + js_flag, " --noerrdialogs")
(clean / "env.c").write_text(source)
(patches / "0005-ntdll-ohos-cef-diagnostic-command-line.patch").write_text(
    diff((before / "env.c").read_text(), source, "env.c"))

probe_names = ["ohos_virtual.c", "virtual.c", "env.c"]
server = (unix / "server.c").read_text()
if "[EXIT-PROBE]" in server:
    (clean / "server.c").write_text("".join(
        line for line in server.splitlines(True) if "[EXIT-PROBE]" not in line))
    probe_names.append("server.c")
for name in ("thread.c", "signal_arm64.c"):
    source = (unix / name).read_text()
    if "[QUIT-PROBE]" in source:
        (clean / name).write_text("".join(
            line for line in source.splitlines(True) if "[QUIT-PROBE]" not in line))
        probe_names.append(name)
probe = "".join(diff((clean / name).read_text(), (unix / name).read_text(), name)
                for name in probe_names)
(root / "scripts/patches/wine-native-teb-probe.patch").write_text(probe)
print("Exported fixes 0003-0005, clean sources, and separate native probe patch")
