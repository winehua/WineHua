# WineHua Steam Host

`steam_host.py` is a Host-side reference implementation for the device
protocol in [`../docs/steam-host/PROTOCOL.md`](../docs/steam-host/PROTOCOL.md).
It runs on the machine that already has the user's official Steam or SteamCMD
session. It does not run on OHOS and it does not provide a Windows Steam client
inside Wine.

## Windows Host deployment

`Start-SteamHost.ps1` is the supported Windows entrypoint. It reads one local
configuration file and invokes `steam_host.py` for interactive SteamCMD login,
session status, preflight, Windows depot inspection and foreground service
operation. The wrapper never accepts a password or Steam Guard code. The
`Login` action inherits the Host terminal so that input goes only to the
official SteamCMD process.

Copy `host-config.example.json` to a Host-private directory and replace the
placeholder paths, hostname, Steam login name and AppID policy. Do not commit
the resulting file. It contains no Steam password, refresh token or Steam
Guard code, but it identifies local paths and the Steam login name and should
be readable only by the Host operator. `dataDir`, `certificate`, `privateKey`
and `steamcmd` may be absolute paths or paths relative to that configuration
file.

The configured origin must be a hostname covered by the certificate SAN, and
the issuing CA must be trusted by the OHOS device. A self-signed certificate
with an SPKI pin is insufficient because the device retains ordinary HTTPS
certificate and hostname verification.

Run the Host lifecycle from PowerShell:

```powershell
.\Start-SteamHost.ps1 -Action Login -Config C:\WineHuaSteamHost\host-config.json
.\Start-SteamHost.ps1 -Action Status -Config C:\WineHuaSteamHost\host-config.json
.\Start-SteamHost.ps1 -Action Preflight -Config C:\WineHuaSteamHost\host-config.json
.\Start-SteamHost.ps1 -Action Probe -AppId 480 -Config C:\WineHuaSteamHost\host-config.json
.\Start-SteamHost.ps1 -Action Serve -Config C:\WineHuaSteamHost\host-config.json
```

`Preflight` emits credential-free JSON and must succeed before device pairing.
`Probe` uses SteamCMD with the Windows platform flag, lists executable
candidates, and deletes the temporary depot. Move one reviewed relative path
into the AppID policy and leave `launchReady` false until device testing has
confirmed that the game does not require a local Steam client, Steamworks IPC,
CEF, anti-cheat or another unsupported service. `Serve` remains in the
foreground so the operator can see the short pairing confirmation code.

`-ValidateOnly` verifies configuration shape and required local files without
running Python, SteamCMD or a network service. It is suitable for provisioning
checks before SteamCMD has been logged in.

The Windows wrapper regression can run without SteamCMD or a device:

```powershell
powershell -NoProfile -File .\test_start_steam_host.ps1
```

On the device, open `Steam 宿主` from the Wine launcher to pair the Host, inspect
the approved library, request depot downloads, install verified revisions and
launch titles the Host has explicitly marked launch-ready.

The Host requires a persistent TLS certificate whose chain is trusted by the
device system CA store. The device pairs with the printed SPKI SHA-256 pin,
rather than trusting a certificate learned from the network. The Host derives
that pin using Python's standard library, so a Windows Host does not require a
separate OpenSSL installation.

The certificate subject alternative name must match the hostname in `--origin`.
Use a hostname backed by a device-trusted public CA, or install the private CA
that issued the Host certificate into the device system trust store before
pairing. A self-signed certificate and a pin alone are intentionally rejected:
the client keeps normal certificate validation enabled as well as SPKI pinning.
Print the pin before pairing with:

```sh
python3 steam_host.py --cert /etc/winehua-steam-host/cert.pem --print-pin
```

First run the official SteamCMD login from the Host terminal. The command
inherits that terminal, so the password and Steam Guard prompt are handled by
SteamCMD and never pass through WineHua or the HTTPS service:

```sh
python3 steam_host.py \
  --steamcmd /opt/steamcmd/steamcmd.sh \
  --steam-user your_steam_account \
  --steamcmd-login
```

SteamCMD is the Host's entitlement-backed download tool. It has no storefront
UI: store browsing, purchases, account recovery, and any account-management
action remain in the official Steam client or Steam website on the Host under
the same account. WineHua neither sees nor imports that desktop-client or web
session. Once an entitled title is available to SteamCMD, the Host can inspect
and download its Windows depot through the controlled allowlist flow below.

Confirm that SteamCMD retained that session before exposing the HTTPS Host:

```sh
python3 steam_host.py \
  --data-dir /var/lib/winehua-steam-host \
  --steamcmd /opt/steamcmd/steamcmd.sh \
  --steam-user your_steam_account \
  --steamcmd-status
```

Run the complete no-device deployment preflight before pairing. It validates
the AppID policy, persistent Host storage, TLS material, the certificate SAN
against the configured origin hostname, and the non-interactive SteamCMD
session. Its JSON report contains the device pairing pin and no Steam account
or credential data. A nonzero result means that new depot downloads must
remain disabled until the reported issue is fixed.

```sh
python3 steam_host.py \
  --data-dir /var/lib/winehua-steam-host \
  --config example-apps.json \
  --origin https://steam-host.local:8443 \
  --cert /etc/winehua-steam-host/cert.pem \
  --key /etc/winehua-steam-host/key.pem \
  --steamcmd /opt/steamcmd/steamcmd.sh \
  --steam-user your_steam_account \
  --preflight
```

`deviceTrustChecked` is deliberately `false`: the Host can prove that its key
and certificate load, but only the OHOS device can verify the certificate
chain and origin hostname against its own CA store.

Before adding a title to the device allowlist, the Host operator can use the
same logged-in SteamCMD session to inspect the Windows depot and list its EXE
paths. The command never serves the depot, adds no AppID to the device policy,
and removes its Host-private probe directory after reporting candidates. Copy
one reviewed relative path into `launchExeRelativePath` in the policy, then
leave `launchReady` set to `false` until the title has passed device review.

```sh
python3 steam_host.py \
  --data-dir /var/lib/winehua-steam-host \
  --config example-apps.json \
  --origin https://steam-host.local:8443 \
  --steamcmd /opt/steamcmd/steamcmd.sh \
  --steam-user your_steam_account \
  --probe-app 480
```

Then start the content Host:

```sh
python3 steam_host.py \
  --data-dir /var/lib/winehua-steam-host \
  --config example-apps.json \
  --origin https://steam-host.local:8443 \
  --cert /etc/winehua-steam-host/cert.pem \
  --key /etc/winehua-steam-host/key.pem \
  --steamcmd /opt/steamcmd/steamcmd.sh \
  --steam-user your_steam_account
```

The `--port` value must match the explicit port in `--origin`; use port 443
when the origin has no port. On a Windows Host, use the same commands with the
Python launcher and a SteamCMD executable path, for example:

```powershell
py .\steam_host.py `
  --data-dir C:\WineHuaSteamHost `
  --config .\example-apps.json `
  --origin https://steam-host.local:8443 `
  --cert C:\WineHuaSteamHost\cert.pem `
  --key C:\WineHuaSteamHost\key.pem `
  --steamcmd C:\steamcmd\steamcmd.exe `
  --steam-user your_steam_account
```

The service later sends only `+login <user>` with that Host-owned SteamCMD
session; it never accepts or logs a password or Steam Guard code. It always
runs SteamCMD from that executable's installation directory, so the interactive
login and service share one SteamCMD state directory. Startup performs a
non-interactive `+login <user> +quit` preflight with closed stdin. If that
session has expired, the Host continues serving already verified revisions but
marks new downloads unavailable until the operator repeats `--steamcmd-login`
and restarts it. Depot jobs force SteamCMD's `windows` platform, including when
the Host itself runs on Linux. It serializes SteamCMD work and writes download
output to its Host-private `logs` directory.
Downloads time out after four hours by default; set
`--steamcmd-timeout-seconds` to a value from 60 seconds through 24 hours when
the Host network requires a different bound.

The Host's `apps` object is a deliberate AppID allowlist rather than a scraped
Steam account inventory. Add a title after identifying its AppID and launch
path, then let official SteamCMD enforce account ownership while downloading.
This keeps the device unable to request arbitrary depots and avoids parsing
Steam's private client data or credentials.

Set `launchReady` to `true` only after a title has been reviewed for a
no-local-Steam launch. Download success does not mean that Steam DRM,
Steamworks IPC, CEF, anti-cheat or online requirements work on the device.
The Host evaluates that gate when serving an already verified manifest, so an
operator may change `launchReady` from `false` to `true` and restart the Host
after review without downloading the same revision again.

Run the local state-machine tests with:

```sh
python3 steam-host/test_steam_host.py
```
