# Steam Host implementation

The Steam Host is a separate desktop or server process on hardware controlled
by the user. It is the only process that signs into Steam. WineHua must never
run the Windows Steam client, accept Steam credentials, receive a Steam Guard
code, or emulate Steamworks.

## Host runtime

The reference implementation runs SteamCMD from a Host-owned installation and
uses only that SteamCMD directory for its interactive login, saved session
probe and Windows depot downloads. It exposes only the version 1 HTTPS API in
[PROTOCOL.md](PROTOCOL.md). It uses a persistent TLS certificate and shows its
HTTPS origin plus Base64 SHA-256 SPKI digest in a QR code or other trusted
local pairing channel. A new certificate requires an explicit new pairing; do
not silently replace the device's pin.

SteamCMD is not a Steam Store client. Store browsing, purchases, account
recovery and other account-management actions remain in the Host user's
official Steam client or Steam website. The reference Host does not inspect,
copy or proxy those sessions. SteamCMD downloads only titles for which the
Host account already has an entitlement.

Steam login, Steam Guard and account recovery remain inside the official Steam
tool. The Host keeps its Steam session files and any SteamCMD credentials in
its local protected storage. API logs must redact `Authorization` headers and
must not copy those files into an artifact directory.

Before starting the HTTPS server, the operator can run `steam_host.py
--steamcmd-status` with the intended `--data-dir`, `--steamcmd`, and
`--steam-user`. It runs the same non-interactive `+login <user> +quit` probe as
the server, writes only SteamCMD's own output to the Host log directory, and
returns nonzero until the interactive `--steamcmd-login` flow has completed.

`steam_host.py --preflight` is the deployment gate before device pairing. It
loads the Host policy, opens its private state database, validates that the
listen port agrees with the pinned origin, loads the TLS certificate and key,
performs a local TLS handshake using the origin hostname, derives the SPKI pin,
and runs that same SteamCMD session probe. It emits a credential-free JSON
report and returns nonzero unless new Windows depot downloads are ready. The
Host cannot validate the device's CA trust store; the OHOS client remains
responsible for normal certificate and hostname validation during pairing and
every request.

The Host operator may run `steam_host.py --probe-app <AppID>` before adding a
title to the device allowlist. It invokes the same saved SteamCMD session with
the Windows platform flag and `validate`, lists regular `.exe` paths, and then
deletes its temporary Host-private depot. This local inspection command is not
an API endpoint and cannot make an arbitrary AppID downloadable by a paired
device. The operator uses its output to write a precise executable path into
the allowlist and leaves `launchReady` false until device testing completes.

The configured `apps` object is an allowlist, not a Steam library scraper.
SteamCMD remains the authority for whether the logged-in Host account may
download each approved AppID. The Host must not expose arbitrary AppID download
requests from a paired device.

## Download state machine

For a requested AppID, the Host creates one durable job:

1. `queued`: validate the pairing token and confirm that the title belongs to
   the host user. Before the HTTPS service accepts download requests, it runs
   SteamCMD non-interactively from the configured SteamCMD directory with
   `+login <user> +quit` and closed stdin. A failed or expired session leaves
   existing revisions available but makes `downloadable: false`; the operator
   must run the separate interactive login command again and restart the Host.
2. `running`: invoke the official tooling in a Host-private staging directory.
   A SteamCMD deployment normally uses a dedicated `force_install_dir`, the
   host user's normal login flow, `@sSteamCmdForcePlatformType windows`, and
   `app_update <AppID> validate`. The Host
   reports a monotonic staging-file byte count while SteamCMD runs; it does
   not invent a total byte count before the verified manifest exists.
3. `verifying`: wait for the official tool to finish validation, require the
   policy's launch EXE and working directory to exist as regular content, then
   enumerate regular files only, calculate a lowercase SHA-256 and byte length
   for each file, and write an immutable revision manifest beside the content
   revision. The manifest is a Host sidecar rather than a reserved filename
   inside game content, so a depot file named `winehua-manifest.json` remains
   transferable.
4. `complete`: expose the manifest and `/v1/artifacts/<sha256>` streams only
   after all hashes are stored. Failed or cancelled jobs expose no manifest.

The Host reuses an active job for the same AppID and serializes SteamCMD
invocations for the Host user. It never serves a mutable install directory as a completed revision. Artifact reads come from the immutable
revision selected by the manifest, not from Steam's active download directory.
An unfinished SteamCMD process has a bounded Host timeout; the Host terminates
it, removes staging content and records `steamcmd_timeout` so it cannot block
later queued AppIDs indefinitely.

`launchReady` is a Host policy declaration, not an inference from a successful
download. Start with manually reviewed titles that have no requirement for a
locally running Steam client. Mark games requiring Steam DRM, an online Steam
client, CEF login, Steamworks IPC, an anti-cheat service or untested launch
arguments as `launchReady: false`. This process does not bypass license checks
or substitute any Steam runtime.

The policy is evaluated when the Host serves a verified manifest. An operator
may change an AppID from `launchReady: false` to `true` after review and restart
the Host; the device can then install the already verified revision. File paths,
hashes and revision IDs remain the immutable values recorded at download time.

## API persistence

The Host persists pairing records as a random token hash, client label,
creation time and revocation state. Pairing has a short-lived challenge bound
to the client nonce. `pair/confirm` returns the raw token exactly once. Every
authenticated endpoint checks token revocation before work begins. Revoke the
device token through the authenticated `pair/revoke` endpoint before WineHua
erases it locally. If the Host cannot be contacted, the device records that
remote revocation was not confirmed and the Host operator must revoke the
pairing before treating a lost device as disconnected.

The library endpoint should return an inventory built from Host-owned, already
verified revisions. It may list a title as owned/downloadable before download,
but only a completed manifest may have `installed: true`. The client does not
interpret a library entry as permission to launch; the manifest's
`launchReady` gate remains authoritative.
