# Workflow

## Canonical commands

If the repo is new to you, read `docs/ONBOARDING.md` before using the commands below.

### Windows host, default MSYS2 backend

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\bootstrap_msys2.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\rebuild_harmony.ps1 -Backend msys2 -Mode doctor -Arch x86_64
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\rebuild_harmony.ps1 -Backend msys2 -Mode full -Arch x86_64 -Target 127.0.0.1:5555
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\rebuild_harmony.ps1 -Backend msys2 -Mode incremental -Arch x86_64 -Target 127.0.0.1:5555
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\rebuild_harmony.ps1 -Mode deploy -Target 127.0.0.1:5555
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\rebuild_harmony.ps1 -Mode logs -Target 127.0.0.1:5555
```

### WSL fallback

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\rebuild_harmony.ps1 -Backend wsl -Mode doctor -Arch x86_64
```

### Inner shell build-only

```bash
bash scripts/rebuild_harmony.sh doctor x86_64
bash scripts/rebuild_harmony.sh full x86_64
bash scripts/rebuild_harmony.sh incremental x86_64
bash scripts/rebuild_harmony.sh package x86_64
```

Swap `x86_64` for `arm64` or `all` when needed.

## When to use each mode

- `full`: thirdparty changes, Wine changes, Box64 changes, SDK/env changes, or first build on a machine/backend.
- `incremental`: `entry/` code, native glue, ArkTS, HNP layout, signing, or packaging changes. Missing prerequisite artifacts are auto-healed.
- `package`: existing binaries are good; only HNP/HAP assembly changed. Missing prerequisite artifacts are auto-healed, so first use can become heavier than expected.
- `deploy`: rebuild is unnecessary; current HAP only needs reinstalling.
- `logs`: rebuild is unnecessary; only runtime diagnosis is needed.
- `doctor`: environment sanity check before deeper debugging.

## Artifact paths

- Signed HAP: `entry/build/default/outputs/default/entry-default-signed.hap`
- HNP:
  - `entry/hnp/x86_64/winehua.hnp`
  - `entry/hnp/arm64-v8a/winehua.hnp`

## Validation checklist

- `doctor` resolves `HOST_SHELL`, `OHOS_SDK`, `HVIGORW`, `NODE_BIN`, `JAVA_BIN`, `HNPCLI`, `HDC`, and `CLANG`.
- Target HNP exists for the requested arch.
- Signed HAP exists.
- `hdc install -r` succeeds.
- `aa start -b app.hackeris.winehua -a EntryAbility` succeeds.
- On the validated `x86_64` PC emulator path, `notepad.exe` should launch.
