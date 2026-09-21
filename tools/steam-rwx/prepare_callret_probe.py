"""Stage an isolated ARM64EC return-cache bypass, keeping push/pop accounting."""
from pathlib import Path
import difflib

root = Path(__file__).resolve().parents[2]
source = root / "build/fex-rwx-probe/source"
before = root / "artifacts/fex-rwx-probe/v21-fex-source-before"
before.mkdir(parents=True, exist_ok=True)
changes = {
    "FEXCore/Source/Interface/Core/JIT/BranchOps.cpp": (
        "      sub(TMP1, TMP1, RipReg.X());\n"
        "      (void)cbz(ARMEmitter::Size::i64Bit, TMP1, &SkipFullLookup);\n",
        "#ifndef ARCHITECTURE_arm64ec\n"
        "      sub(TMP1, TMP1, RipReg.X());\n"
        "      (void)cbz(ARMEmitter::Size::i64Bit, TMP1, &SkipFullLookup);\n"
        "#endif\n"
        "      // Candidate-only: ARM64EC returns always resolve through the normal lookup.\n",
    ),
    "FEXCore/Source/Interface/Core/Dispatcher/Dispatcher.cpp": (
        "  add(ARMEmitter::Size::i64Bit, REG_CALLRET_SP, REG_CALLRET_SP, 0x10);\n"
        "  ret(TMP2);\n",
        "  add(ARMEmitter::Size::i64Bit, REG_CALLRET_SP, REG_CALLRET_SP, 0x10);\n"
        "  // Candidate-only: discard the cached host return target, preserving the pop.\n"
        "  (void)b(&LoopTop);\n",
    ),
    "Source/Windows/ARM64EC/Module.cpp": (
        '  __wine_dbg_output("starting FEX based libarm64ecfex.dll\\n");\n',
        '  __wine_dbg_output("starting FEX based libarm64ecfex.dll\\n");\n'
        '  __wine_dbg_output("[FEX-CALLRET-CONTROL] guest_returns=lookup ec_returns=lookup accounting=preserved\\n");\n',
    ),
}
patch = []
for relative, (old, new) in changes.items():
    path = source / relative
    text = path.read_text(encoding="utf-8")
    if new in text:
        original = (before / relative).read_text(encoding="utf-8")
    else:
        assert text.count(old) == 1, relative
        original = text
        saved = before / relative
        saved.parent.mkdir(parents=True, exist_ok=True)
        assert not saved.exists(), "refusing to replace a saved baseline"
        saved.write_text(original, encoding="utf-8")
        text = text.replace(old, new, 1)
        path.write_text(text, encoding="utf-8")
    patch.extend(difflib.unified_diff(original.splitlines(True), text.splitlines(True),
                                     "a/" + relative, "b/" + relative))
(root / "scripts/patches/fex-arm64ec-callret-bypass-probe.patch").write_text(
    "".join(patch), encoding="utf-8")
print("Staged ARM64EC return-cache bypass in isolated probe source only")
