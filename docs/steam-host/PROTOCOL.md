# Steam Host Content Protocol

WineHua runs Windows game executables from an isolated Wine container. It does
not launch the Windows Steam client and it does not attempt to transplant a
desktop Steam session into Wine. A separately deployed Steam Host performs
Steam account login, Steam Guard, license checks, depot download and depot
verification with official Steam tooling on hardware chosen by the user.

## Trust boundary

The OHOS app pairs with one Host over HTTPS on the local network. Pairing pins
the Host certificate's SHA-256 SPKI digest and stores only the origin, pin,
host ID and a revocable pairing token in OHOS preferences. The pairing input is
an HTTPS origin with no path plus a Base64 SPKI digest; it is supplied out of
band, for example in a QR code displayed by the Host. The app never transmits
or stores a Steam password, refresh token, or Steam Guard code. Certificate
verification remains enabled for every request; proxy use and redirects are
disabled.

The Host is responsible for its own Steam authentication. This reference
implementation uses SteamCMD under the Host user's account; it does not import
or proxy a desktop Steam client session. The Host must not claim that
downloaded content is launchable until the Steam depot verification step has
completed.

## Version 1 endpoints

| Method | Path | Purpose |
| --- | --- | --- |
| `GET` | `/v1/capabilities` | Protocol version and supported operations |
| `POST` | `/v1/pair/begin` | Begin user-visible pairing, returning a short confirmation challenge |
| `POST` | `/v1/pair/confirm` | Confirm the challenge and return a revocable pairing token |
| `POST` | `/v1/pair/revoke` | Revoke the authenticated device pairing token |
| `GET` | `/v1/library` | Owned/installed application inventory without Steam credentials |
| `POST` | `/v1/games/{appId}/downloads` | Ask Host to download and verify a depot revision |
| `GET` | `/v1/downloads/{jobId}` | Download state and byte progress |
| `GET` | `/v1/games/{appId}/manifest` | Verified launch manifest and per-file SHA-256 values |
| `GET` | `/v1/artifacts/{sha256}` | HTTPS content stream for a manifest artifact |

All authenticated endpoints require `Authorization: Bearer <pairing token>`.
Tokens must not appear in logs, URLs, manifest files, or game environment
variables.

While SteamCMD is running, `transferredBytes` is the Host staging directory's
monotonic regular-file byte count. SteamCMD does not provide a trusted total
for every depot layout, so `totalBytes` remains `0` until verification has
published the manifest. A completed job always has matching nonzero or zero
`transferredBytes` and `totalBytes`.

`downloadable` means the AppID is in the Host policy and the Host's startup
SteamCMD session preflight succeeded. It is `false` when SteamCMD is absent,
its saved session has expired, or the non-interactive preflight cannot complete.
The Host still exposes prior verified revisions in that state. The operator
runs the interactive Host login command again in the same SteamCMD directory,
then restarts the Host before requesting a new download. A download request in
that state is rejected without creating a job.

When the device removes a paired Host it first posts protocol version 1 to
`/v1/pair/revoke` through the already pinned connection, then erases its local
origin, pin and token. If the Host is offline, the device still erases its
local credential and reports that the Host did not confirm revocation; the
operator must later revoke that pairing from the Host before trusting a lost
device as disconnected.

`pair/begin` creates a short-lived challenge and returns only its ID, expiry
and Host ID to the device. The Host shows the confirmation code locally to its
operator. The operator enters that code into the device's pairing flow, which
sends it to `pair/confirm`. Returning the code to an unpaired requester would
let that requester approve its own pairing and is forbidden.

## Manifest and installation rule

`SteamHostGameManifest` is defined in
`entry/src/main/ets/service/SteamHostProtocol.ets`. The OHOS client accepts a
manifest only when all file paths are relative, contain no `..`, use `/`
separators and have lowercase SHA-256 hashes. A manifest for AppID `480` must
declare exactly these roots:

```json
{
  "containerId": "steam-480",
  "installRelativePath": "games/480"
}
```

`launchExeRelativePath` and `workingDirectoryRelativePath` are relative to a
verified revision below that root. `workingDirectoryRelativePath` may be `.`
for the root. The launch EXE must appear in the artifact list. Every artifact
URL must equal the paired origin plus `/v1/artifacts/<lowercase sha256>`;
manifests cannot nominate a CDN, a redirect target, an OHOS path, a
`WINEPREFIX`, or a shell command.

For application `480`, Host content is installed only below:

```text
files/containers/steam-480/prefix/drive_c/games/480/
```

The client writes each file to
`drive_c/games/480/.staging/<revision>/<relative path>.part`, verifies its
size and SHA-256, then renames it. Only after all artifacts verify does it
atomically promote the staging revision to
`drive_c/games/480/releases/<revision>`. It preserves prior revisions on
download failure. The corresponding Wine path is derived, for example,
`C:\\games\\480\\releases\\build-17\\game.exe`.

## Runtime boundary

Downloading a Steam depot does not make every title runnable without Steam.
Games that require a live Steam client, Steam DRM, or unsupported Steamworks
runtime remain unsupported until a compatible, licensed runtime exists on the
target. The Host must expose this as `launchReady: false`; it must not replace
Steam API behavior or bypass license checks.

`launchReady` is evaluated from the current Host allowlist policy when a
verified manifest is requested. Changing that policy after a review permits or
blocks installation of an existing immutable revision without changing its
artifact paths, hashes, or revision ID.

The first Host milestone is therefore: pair a Host, retrieve a verified
manifest for a library title that can run without a local Steam client, copy it
to an isolated Wine container, then launch the derived executable. Store login
and depot download are solved on the Host; WineHua owns integrity checks,
container selection, and local process launch.

See [HOST_IMPLEMENTATION.md](HOST_IMPLEMENTATION.md) for the required Host
state machine and SteamCMD/official-client boundary.
