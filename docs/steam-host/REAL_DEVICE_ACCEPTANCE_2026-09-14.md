# Steam Host real-device acceptance - 2026-09-14

## Scope

This record validates the WineHua platform gates required before testing the
Steam Host route. The route runs SteamCMD only on a user-controlled Host; it
does not start a Windows Steam client in Wine.

## Candidate

- Device: `192.168.180.71:36875` over wireless HDC.
- Package: `entry-default-signed.hap`.
- Package SHA-256:
  `18AF922DF3F3095E9017DFD4689E41406BAFDCAD9C3EEBF069A2982E68E31171`.
- Runtime payload SHA-256:
  `0ec0718e22987122c93c7b2d5cd5763f0ab846069ddc5f40088770c1c950b64a`.
- Prefix mode: `reuse`; no prefix cleanup was performed.
- Default D3D backend: DXVK legacy 1.10.3. DXVK modern and VKD3D were not
  activated.

## Device results

| Gate | Run ID | Result |
| --- | --- | --- |
| OpenGL core x64/x86 | `core-20260914-195143` | PASS 2/2, present |
| Audio x64/x86 | `audio-20260914-195235` | PASS 2/2, audio |
| Process/IPC x64/x86 | `process-20260914-201050` | PASS 2/2, complete |
| Network/TLS x64/x86 | `network-20260914-201229` | PASS 2/2, complete |
| DXVK legacy x86 D3D11, x64 D3D11, x64 cube | `dxvk-20260914-201249` | PASS 3/3; cube reached present |

The process test covers cross-bitness process creation, named-pipe duplex
traffic, shared memory, event synchronization, inherited handles, loopback,
and child reaping. The network test covers its controlled DNS, TCP, Schannel,
WinHTTP, WinInet, and crypt32 checks, including certificate trust failures.

## Steam Host validation

- `python steam-host/test_steam_host.py`: PASS, 25 tests.
- `powershell -NoProfile -File steam-host/test_start_steam_host.ps1`: PASS.
- The wrapper regression had treated the expected negative Probe result as a
  terminating PowerShell native-command error. Its test now captures that
  expected nonzero exit locally and still checks the error text.

No real Host deployment was present on this workstation at validation time:

- no `steamcmd.exe` installation;
- no Host-private configuration file;
- no persistent TLS certificate whose hostname or IP SAN is reachable from the
  device and whose issuer is trusted by the device;
- no existing interactive SteamCMD session; and
- no reviewed AppID with `launchReady: true`.

Consequently, a device-to-Host pairing, SteamCMD depot download, verified
installation, and game launch have not been claimed as passing. Simulating
these conditions with a self-signed test certificate would only prove the
required TLS rejection path and would not validate the intended service.

## Remaining execution gate

The Host operator must create a private configuration based on
`steam-host/host-config.example.json`, install official SteamCMD, and run the
interactive `Start-SteamHost.ps1 -Action Login` action in a Host terminal. The
password and Steam Guard exchange remain inside SteamCMD.

Before the device is paired, `Start-SteamHost.ps1 -Action Preflight` must
succeed with a persistent certificate trusted by the device and an origin that
matches its SAN. Then run `Probe` for an owned Windows title, review its launch
requirements, set only that approved title's `launchReady` field, and start
the foreground `Serve` action. Pair, request download, install, and launch
through the WineHua `Steam 宿主` panel or the corresponding documented Want
actions.
