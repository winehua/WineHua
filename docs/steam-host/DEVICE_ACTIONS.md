# Device Host actions

The launcher exposes this workflow through its `Steam 宿主` panel. The existing
Ability Want interface remains the device acceptance and unattended-test path.
Both use the same constrained API while keeping Steam account login, Steam
Guard and SteamCMD state on the Host.

The action is accepted only when `winehua.mode=steam-host`. All action results
are emitted under the `SteamHost` hilog tag. The app never logs a pairing
token, certificate pin, Steam password or Steam Guard code.

## Pair a Host

First, start the reference Host with a persistent HTTPS certificate. Copy its
HTTPS origin and the printed Base64 SHA-256 SPKI pin through a trusted local
channel. Ask the device to create a challenge:

The hostname in that origin must match the certificate and the certificate
chain must already be trusted by the device system CA store. SPKI pinning is an
additional identity check; it does not permit self-signed or otherwise
untrusted certificates.

```sh
hdc shell aa start -a EntryAbility -b app.hackeris.winehua \
  --ps winehua.mode steam-host \
  --ps winehua.steam_action pair-begin \
  --ps winehua.host_origin https://steam-host.local:8443 \
  --ps winehua.certificate_pin '<Base64-SPKI-SHA256>'
```

The Host console, rather than the device log, prints the six-digit approval
code. Enter it in a second action before the challenge expiry:

```sh
hdc shell aa start -a EntryAbility -b app.hackeris.winehua \
  --ps winehua.mode steam-host \
  --ps winehua.steam_action pair-confirm \
  --ps winehua.host_origin https://steam-host.local:8443 \
  --ps winehua.certificate_pin '<Base64-SPKI-SHA256>' \
  --ps winehua.challenge_id '<challenge-id>' \
  --ps winehua.verification_code '<code-shown-on-host>'
```

Use `winehua.steam_action=capabilities` and then `library` to confirm the
pinned Host is available. The library action reports only counts in device
logs; game names and Steam account data do not need to cross the diagnostic
boundary.

Use `winehua.steam_action=revoke` while the Host is reachable to revoke the
device token from the Host and remove the local pairing. When the Host cannot
be reached, the app still removes its local credential but reports that remote
revocation was not confirmed.

## Download, install and launch

The device tells the Host to download a policy-approved AppID using its own
existing SteamCMD or official Steam login:

```sh
hdc shell aa start -a EntryAbility -b app.hackeris.winehua \
  --ps winehua.mode steam-host \
  --ps winehua.steam_action download \
  --ps winehua.app_id 480
```

The result provides a Host job ID. Poll it without restarting the job:

```sh
hdc shell aa start -a EntryAbility -b app.hackeris.winehua \
  --ps winehua.mode steam-host \
  --ps winehua.steam_action download-status \
  --ps winehua.app_id 480 \
  --ps winehua.job_id '<job-id>'
```

When the job is complete and the Host has manually marked the title
`launchReady: true`, use `install`. WineHua streams every artifact through the
pinned connection, checks byte counts and SHA-256, then promotes one immutable
revision into `steam-<AppID>`'s own Wine prefix. Use `launch` to execute that
verified revision, or `install-launch` to do both in one operation.

Changing that policy after review and restarting the Host permits installation
of the completed revision; it does not download it again or modify its files.

Neither action starts a local Windows Steam process. A game with Steam DRM,
Steamworks IPC, anti-cheat, CEF or online-client requirements remains blocked
by the Host `launchReady` gate.
