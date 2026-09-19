#!/usr/bin/env python3
"""Local HTTPS Steam content host for the WineHua Steam Host v1 protocol.

The process deliberately delegates Steam login and depot retrieval to official
Steam tooling running under the local Host user. It never accepts a password
or Steam Guard code over its HTTPS API.
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
import os
import re
import secrets
import shutil
import sqlite3
import socket
import ssl
import subprocess
import sys
import threading
import time
import uuid
from dataclasses import dataclass
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any, Iterable
from urllib.parse import urlsplit


PROTOCOL_VERSION = 1
DEFAULT_STEAMCMD_TIMEOUT_SECONDS = 4 * 60 * 60
STEAMCMD_SESSION_CHECK_TIMEOUT_SECONDS = 120
STEAMCMD_TARGET_PLATFORM = "windows"
APP_ID_RE = re.compile(r"^\d{1,20}$")
REVISION_RE = re.compile(r"^[A-Za-z0-9._-]{1,128}$")
SHA256_RE = re.compile(r"^[a-f0-9]{64}$")
ORIGIN_RE = re.compile(
    r"^https://([A-Za-z0-9](?:[A-Za-z0-9.-]{0,251}[A-Za-z0-9])?|\[[0-9A-Fa-f:]+\])(?::([0-9]{1,5}))?$"
)


class HostError(RuntimeError):
    pass


def normalize_origin(value: str) -> str:
    if not value or len(value) > 320 or value != value.strip():
        raise HostError("origin must be a non-empty HTTPS origin")
    match = ORIGIN_RE.fullmatch(value)
    if not match:
        raise HostError("origin must not contain a path, query, fragment, or credentials")
    host, port = match.groups()
    if ".." in host or host.startswith(".") or host.endswith("."):
        raise HostError("invalid origin hostname")
    if port and not 1 <= int(port) <= 65535:
        raise HostError("origin port is out of range")
    return f"https://{host.lower()}{':' + port if port else ''}"


def validate_server_port(origin: str, port: int) -> None:
    if not 1 <= port <= 65535:
        raise HostError("listen port is out of range")
    origin_port = urlsplit(origin).port or 443
    if port != origin_port:
        raise HostError("--port must match the port in --origin")


def valid_relative_path(value: str, *, require_exe: bool = False) -> bool:
    if not value or len(value) > 1024 or value.startswith("/") or "\\" in value:
        return False
    pieces = value.split("/")
    if any(piece in {"", ".", ".."} for piece in pieces):
        return False
    return not require_exe or value.lower().endswith(".exe")


def file_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def directory_regular_file_bytes(root: Path) -> int:
    """Return a bounded progress lower bound without following symlinks."""
    total = 0
    for directory, names, files in os.walk(root, followlinks=False):
        directory_path = Path(directory)
        names[:] = [name for name in names if not (directory_path / name).is_symlink()]
        for name in files:
            path = directory_path / name
            try:
                if not path.is_symlink() and path.is_file():
                    total += path.stat().st_size
            except OSError:
                # SteamCMD can atomically replace or remove files between
                # directory enumeration and stat. Progress is a lower bound,
                # so ignore that one racing entry and scan again next second.
                continue
    return total


def probe_steamcmd_session(steamcmd: Path | None, steam_user: str | None, log_path: Path) -> str:
    """Check an existing SteamCMD session without ever accepting credentials."""
    if steamcmd is None or steam_user is None or not steamcmd.is_file():
        return "unconfigured"
    command = [str(steamcmd), "+login", steam_user, "+quit"]
    try:
        log_path.parent.mkdir(parents=True, exist_ok=True)
        with log_path.open("wb") as log_file:
            completed = subprocess.run(
                command,
                stdin=subprocess.DEVNULL,
                stdout=log_file,
                stderr=subprocess.STDOUT,
                cwd=str(steamcmd.parent),
                timeout=STEAMCMD_SESSION_CHECK_TIMEOUT_SECONDS,
                check=False,
            )
    except subprocess.TimeoutExpired:
        return "timed_out"
    except OSError:
        return "unavailable"
    return "ready" if completed.returncode == 0 else "login_required"


def bearer_token(headers: Any) -> str | None:
    value = headers.get("Authorization", "")
    if not value.startswith("Bearer "):
        return None
    token = value[7:]
    if not re.fullmatch(r"[A-Za-z0-9._~+/=-]{16,1024}", token):
        return None
    return token


@dataclass(frozen=True)
class AppPolicy:
    app_id: str
    name: str
    launch_exe_relative_path: str
    working_directory_relative_path: str
    launch_ready: bool


@dataclass(frozen=True)
class HostConfiguration:
    origin: str
    host_id: str
    data_dir: Path
    apps: dict[str, AppPolicy]
    steamcmd: Path | None
    steam_user: str | None
    steamcmd_timeout_seconds: int = DEFAULT_STEAMCMD_TIMEOUT_SECONDS


def load_configuration(args: argparse.Namespace) -> HostConfiguration:
    origin = normalize_origin(args.origin)
    config_path = Path(args.config)
    raw: dict[str, Any] = {}
    if config_path.exists():
        with config_path.open("r", encoding="utf-8") as source:
            raw = json.load(source)
    if not isinstance(raw, dict):
        raise HostError("Host configuration must be a JSON object")
    app_values = raw.get("apps", {})
    if not isinstance(app_values, dict):
        raise HostError("apps must be an object indexed by AppID")
    apps: dict[str, AppPolicy] = {}
    for app_id, item in app_values.items():
        if not isinstance(item, dict) or not APP_ID_RE.fullmatch(app_id):
            raise HostError("invalid configured AppID")
        name = item.get("name", "")
        launch_exe = item.get("launchExeRelativePath", "")
        working_directory = item.get("workingDirectoryRelativePath", ".")
        launch_ready = item.get("launchReady", False)
        if (not isinstance(name, str) or not name or len(name) > 512 or
                not isinstance(launch_exe, str) or not valid_relative_path(launch_exe, require_exe=True) or
                not isinstance(working_directory, str) or
                (working_directory != "." and not valid_relative_path(working_directory)) or
                not isinstance(launch_ready, bool)):
            raise HostError(f"invalid policy for AppID {app_id}")
        apps[app_id] = AppPolicy(app_id, name, launch_exe, working_directory, launch_ready)
    host_id = args.host_id or raw.get("hostId", "")
    if not isinstance(host_id, str) or not re.fullmatch(r"[A-Za-z0-9._-]{1,128}", host_id):
        raise HostError("hostId must use letters, digits, dot, underscore, or hyphen")
    steamcmd_value = args.steamcmd or raw.get("steamcmd")
    steamcmd = Path(steamcmd_value).expanduser().resolve() if steamcmd_value else None
    steam_user = args.steam_user or raw.get("steamUser")
    if steam_user is not None and (not isinstance(steam_user, str) or not steam_user):
        raise HostError("steamUser must be a non-empty string when configured")
    timeout_value = args.steamcmd_timeout_seconds
    if timeout_value is None:
        timeout_value = raw.get("steamcmdTimeoutSeconds", DEFAULT_STEAMCMD_TIMEOUT_SECONDS)
    if type(timeout_value) is not int or not 60 <= timeout_value <= 24 * 60 * 60:
        raise HostError("SteamCMD timeout must be between 60 seconds and 24 hours")
    return HostConfiguration(
        origin, host_id, Path(args.data_dir).resolve(), apps, steamcmd, steam_user, timeout_value
    )


class SteamHostState:
    def __init__(self, config: HostConfiguration) -> None:
        self.config = config
        self.data_dir = config.data_dir
        self.revisions_dir = self.data_dir / "revisions"
        self.staging_dir = self.data_dir / "staging"
        self.logs_dir = self.data_dir / "logs"
        for directory in (self.data_dir, self.revisions_dir, self.staging_dir, self.logs_dir):
            directory.mkdir(parents=True, exist_ok=True)
        self.db = sqlite3.connect(self.data_dir / "steam-host.sqlite3", check_same_thread=False)
        self.db.row_factory = sqlite3.Row
        self.lock = threading.RLock()
        # SteamCMD owns state below one Host user's home directory. Keep its
        # invocations serial even for distinct AppIDs so two jobs cannot race
        # on Steam's own client/depot cache.
        self.download_lock = threading.Lock()
        # Unknown is permitted for direct state-machine tests, but run_server()
        # always performs the non-interactive probe before it accepts clients.
        # A probe failure disables new downloads without making already verified
        # revisions unavailable.
        self._steamcmd_session_state = "unknown"
        self._initialize_database()

    def _initialize_database(self) -> None:
        with self.lock, self.db:
            self.db.executescript(
                """
                CREATE TABLE IF NOT EXISTS pairings (
                    token_hash TEXT PRIMARY KEY,
                    host_id TEXT NOT NULL,
                    created_at INTEGER NOT NULL,
                    revoked INTEGER NOT NULL DEFAULT 0
                );
                CREATE TABLE IF NOT EXISTS challenges (
                    challenge_id TEXT PRIMARY KEY,
                    code_hash TEXT NOT NULL,
                    client_nonce TEXT NOT NULL,
                    expires_at INTEGER NOT NULL,
                    consumed INTEGER NOT NULL DEFAULT 0
                );
                CREATE TABLE IF NOT EXISTS jobs (
                    job_id TEXT PRIMARY KEY,
                    app_id TEXT NOT NULL,
                    state TEXT NOT NULL,
                    transferred_bytes INTEGER NOT NULL DEFAULT 0,
                    total_bytes INTEGER NOT NULL DEFAULT 0,
                    manifest_revision TEXT NOT NULL DEFAULT '',
                    error_code TEXT NOT NULL DEFAULT ''
                );
                CREATE TABLE IF NOT EXISTS artifacts (
                    sha256 TEXT PRIMARY KEY,
                    file_path TEXT NOT NULL,
                    size_bytes INTEGER NOT NULL
                );
                """
            )
            # A process restart cannot safely resume an unowned SteamCMD
            # child. Make that terminal state explicit instead of presenting a
            # permanently queued job to a paired device.
            self.db.execute(
                "UPDATE jobs SET state = 'failed', error_code = 'host_restarted' "
                "WHERE state IN ('queued', 'running', 'verifying')"
            )

    def close(self) -> None:
        with self.lock:
            self.db.close()

    def capabilities(self) -> dict[str, Any]:
        return {
            "protocolVersion": PROTOCOL_VERSION,
            "hostId": self.config.host_id,
            "libraryRead": True,
            "managedDownloads": True,
            "artifactChecksums": True,
        }

    def steamcmd_configured(self) -> bool:
        return (
            self.config.steamcmd is not None
            and self.config.steam_user is not None
            and self.config.steamcmd.is_file()
        )

    def check_steamcmd_session(self) -> str:
        """Confirm a pre-existing SteamCMD login without accepting credentials.

        SteamCMD keeps its session relative to its own installation directory.
        The check and later downloads therefore share both the executable and
        working directory. stdin is closed so a missing login cannot turn the
        long-running HTTPS Host into a password or Steam Guard prompt.
        """
        log_path = self.logs_dir / "steamcmd-session-check.log"
        status = probe_steamcmd_session(self.config.steamcmd, self.config.steam_user, log_path)
        with self.lock:
            self._steamcmd_session_state = status
        return status

    def begin_pairing(self, client_nonce: str, client_name: str) -> tuple[dict[str, Any], str]:
        if not re.fullmatch(r"[A-Za-z0-9_-]{8,128}", client_nonce):
            raise HostError("invalid client nonce")
        if not client_name or len(client_name) > 128:
            raise HostError("invalid client name")
        challenge_id = secrets.token_urlsafe(18)
        code = f"{secrets.randbelow(1_000_000):06d}"
        expires_at = int(time.time() * 1000) + 120_000
        with self.lock, self.db:
            self.db.execute(
                "INSERT INTO challenges(challenge_id, code_hash, client_nonce, expires_at) VALUES(?, ?, ?, ?)",
                (challenge_id, hashlib.sha256(code.encode("ascii")).hexdigest(), client_nonce, expires_at),
            )
        return {
            "challengeId": challenge_id,
            "expiresAtMs": expires_at,
            "hostId": self.config.host_id,
        }, code

    def confirm_pairing(self, challenge_id: str, code: str, client_nonce: str) -> dict[str, str]:
        if not re.fullmatch(r"[A-Za-z0-9_-]{8,128}", challenge_id):
            raise HostError("invalid challenge")
        if not re.fullmatch(r"[A-Z0-9-]{4,32}", code):
            raise HostError("invalid confirmation code")
        with self.lock, self.db:
            row = self.db.execute(
                "SELECT code_hash, client_nonce, expires_at, consumed FROM challenges WHERE challenge_id = ?",
                (challenge_id,),
            ).fetchone()
            if row is None or row["consumed"] or row["expires_at"] < int(time.time() * 1000):
                raise HostError("pairing challenge expired")
            if not secrets.compare_digest(row["client_nonce"], client_nonce):
                raise HostError("pairing nonce mismatch")
            if not secrets.compare_digest(row["code_hash"], hashlib.sha256(code.encode("ascii")).hexdigest()):
                raise HostError("pairing confirmation failed")
            token = secrets.token_urlsafe(32)
            self.db.execute("UPDATE challenges SET consumed = 1 WHERE challenge_id = ?", (challenge_id,))
            self.db.execute(
                "INSERT INTO pairings(token_hash, host_id, created_at) VALUES(?, ?, ?)",
                (hashlib.sha256(token.encode("ascii")).hexdigest(), self.config.host_id, int(time.time() * 1000)),
            )
        return {"hostId": self.config.host_id, "pairingToken": token}

    def authenticate(self, token: str | None) -> bool:
        if token is None:
            return False
        token_hash = hashlib.sha256(token.encode("ascii")).hexdigest()
        with self.lock:
            row = self.db.execute(
                "SELECT 1 FROM pairings WHERE token_hash = ? AND revoked = 0", (token_hash,)
            ).fetchone()
        return row is not None

    def revoke_pairing(self, token: str | None) -> bool:
        """Revoke exactly the currently authenticated device credential."""
        if token is None:
            return False
        token_hash = hashlib.sha256(token.encode("ascii")).hexdigest()
        with self.lock, self.db:
            cursor = self.db.execute(
                "UPDATE pairings SET revoked = 1 WHERE token_hash = ? AND revoked = 0",
                (token_hash,),
            )
        return cursor.rowcount == 1

    def steamcmd_download_ready(self) -> bool:
        with self.lock:
            session_state = self._steamcmd_session_state
        return self.steamcmd_configured() and session_state not in {
            "unconfigured", "unavailable", "timed_out", "login_required"
        }

    def library(self) -> list[dict[str, Any]]:
        result: list[dict[str, Any]] = []
        for app_id, policy in sorted(self.config.apps.items(), key=lambda item: int(item[0])):
            revision = self._latest_revision(app_id)
            result.append({
                "appId": app_id,
                "name": policy.name,
                "installed": revision is not None,
                "downloadable": self.steamcmd_download_ready(),
                "launchReady": revision is not None and policy.launch_ready,
            })
        return result

    def request_download(self, app_id: str) -> dict[str, Any]:
        if app_id not in self.config.apps:
            raise HostError("AppID is not approved by this Host")
        if not self.steamcmd_download_ready():
            raise HostError("SteamCMD session is not ready for downloads")
        with self.lock, self.db:
            active = self.db.execute(
                "SELECT job_id FROM jobs WHERE app_id = ? "
                "AND state IN ('queued', 'running', 'verifying') ORDER BY rowid DESC LIMIT 1",
                (app_id,),
            ).fetchone()
            if active is not None:
                return self.job(active["job_id"], app_id)
            job_id = secrets.token_urlsafe(18)
            self.db.execute("INSERT INTO jobs(job_id, app_id, state) VALUES(?, ?, 'queued')", (job_id, app_id))
        worker = threading.Thread(target=self._download_job, args=(job_id, app_id), daemon=True)
        worker.start()
        return self.job(job_id, app_id)

    def job(self, job_id: str, app_id: str) -> dict[str, Any]:
        with self.lock:
            row = self.db.execute("SELECT * FROM jobs WHERE job_id = ?", (job_id,)).fetchone()
        if row is None or row["app_id"] != app_id:
            raise HostError("unknown download job")
        value: dict[str, Any] = {
            "jobId": row["job_id"],
            "appId": row["app_id"],
            "state": row["state"],
            "transferredBytes": row["transferred_bytes"],
            "totalBytes": row["total_bytes"],
            "manifestRevision": row["manifest_revision"],
        }
        if row["error_code"]:
            value["errorCode"] = row["error_code"]
        return value

    def manifest(self, app_id: str) -> dict[str, Any]:
        policy = self.config.apps.get(app_id)
        if policy is None:
            raise HostError("AppID is not approved by this Host")
        revision = self._latest_revision(app_id)
        if revision is None:
            raise HostError("no verified revision is available")
        manifest_path = self._manifest_path(self.revisions_dir / app_id / revision)
        with manifest_path.open("r", encoding="utf-8") as source:
            manifest = json.load(source)
        if not isinstance(manifest, dict):
            raise HostError("stored revision manifest is invalid")
        # Hashes, paths and the selected revision remain immutable. Launch
        # approval is a current Host policy decision, so an operator can review
        # downloaded content and permit installation without downloading it
        # again.
        manifest["launchReady"] = policy.launch_ready
        return manifest

    def artifact_path(self, sha256: str) -> Path:
        if not SHA256_RE.fullmatch(sha256):
            raise HostError("invalid artifact hash")
        with self.lock:
            row = self.db.execute("SELECT file_path, size_bytes FROM artifacts WHERE sha256 = ?", (sha256,)).fetchone()
        if row is None:
            raise HostError("artifact does not exist")
        path = Path(row["file_path"])
        try:
            path.resolve().relative_to(self.revisions_dir.resolve())
        except ValueError as exc:
            raise HostError("artifact is outside the immutable revision store") from exc
        if not path.is_file() or path.stat().st_size != row["size_bytes"]:
            raise HostError("artifact is no longer available")
        return path

    def publish_revision(self, app_id: str, source: Path) -> dict[str, Any]:
        policy = self.config.apps.get(app_id)
        if policy is None:
            raise HostError("AppID is not approved by this Host")
        if not source.is_dir():
            raise HostError("verified source directory does not exist")
        launch_path = source / policy.launch_exe_relative_path
        if launch_path.is_symlink() or not launch_path.is_file():
            raise HostError("configured launch executable is absent from verified content")
        working_directory = (
            source if policy.working_directory_relative_path == "."
            else source / policy.working_directory_relative_path
        )
        if working_directory.is_symlink() or not working_directory.is_dir():
            raise HostError("configured working directory is absent from verified content")
        revision = f"{int(time.time() * 1000)}-{uuid.uuid4().hex[:12]}"
        destination = self.revisions_dir / app_id / revision
        destination.parent.mkdir(parents=True, exist_ok=True)
        if destination.exists():
            raise HostError("revision collision")
        # Hash and write the manifest in the private staging directory.  The
        # atomic move below is the publication point: a failed hash must never
        # leave a directory that library() can treat as an installed revision.
        artifacts = list(self._enumerate_artifacts(source))
        if not any(item["relativePath"] == policy.launch_exe_relative_path for item in artifacts):
            raise HostError("configured launch executable is not a regular artifact")
        artifact_url_prefix = f"{self.config.origin}/v1/artifacts/"
        manifest = {
            "schemaVersion": PROTOCOL_VERSION,
            "revision": revision,
            "appId": app_id,
            "containerId": f"steam-{app_id}",
            "installRelativePath": f"games/{app_id}",
            "launchExeRelativePath": policy.launch_exe_relative_path,
            "workingDirectoryRelativePath": policy.working_directory_relative_path,
            "launchReady": policy.launch_ready,
            "artifacts": [
                {
                    "relativePath": item["relativePath"],
                    "sizeBytes": item["sizeBytes"],
                    "sha256": item["sha256"],
                    "downloadUrl": artifact_url_prefix + item["sha256"],
                }
                for item in artifacts
            ],
        }
        manifest_staging_path = source.parent / f"{source.name}.winehua-manifest.json"
        manifest_destination_path = self._sidecar_manifest_path(destination)
        with manifest_staging_path.open("w", encoding="utf-8", newline="\n") as output:
            json.dump(manifest, output, ensure_ascii=False, separators=(",", ":"))
            output.write("\n")
        try:
            os.replace(source, destination)
            # Publishing the sidecar is the commit marker. _latest_revision()
            # ignores a content directory until this second atomic move lands.
            os.replace(manifest_staging_path, manifest_destination_path)
        except OSError:
            manifest_staging_path.unlink(missing_ok=True)
            if destination.is_dir() and not self._is_published_revision(app_id, destination):
                shutil.rmtree(destination, ignore_errors=True)
            raise
        with self.lock, self.db:
            for item in artifacts:
                self.db.execute(
                    "INSERT OR REPLACE INTO artifacts(sha256, file_path, size_bytes) VALUES(?, ?, ?)",
                    (item["sha256"], str(destination / item["relativePath"]), item["sizeBytes"]),
                )
        return manifest

    def _enumerate_artifacts(self, root: Path) -> Iterable[dict[str, Any]]:
        for directory, names, files in os.walk(root, followlinks=False):
            directory_path = Path(directory)
            names[:] = [name for name in names if not (directory_path / name).is_symlink()]
            for name in sorted(files):
                path = directory_path / name
                if path.is_symlink() or not path.is_file():
                    continue
                relative = path.relative_to(root).as_posix()
                if not valid_relative_path(relative):
                    continue
                yield {"relativePath": relative, "sizeBytes": path.stat().st_size, "sha256": file_sha256(path)}

    def _latest_revision(self, app_id: str) -> str | None:
        root = self.revisions_dir / app_id
        if not root.is_dir():
            return None
        revisions = [
            item.name for item in root.iterdir()
            if item.is_dir() and REVISION_RE.fullmatch(item.name)
            and self._is_published_revision(app_id, item)
        ]
        return max(revisions) if revisions else None

    @staticmethod
    def _sidecar_manifest_path(directory: Path) -> Path:
        return directory.parent / f"{directory.name}.winehua-manifest.json"

    @staticmethod
    def _manifest_path(directory: Path) -> Path:
        sidecar = SteamHostState._sidecar_manifest_path(directory)
        if sidecar.is_file():
            return sidecar
        # Revisions published by protocol v1 before the sidecar migration
        # retain their in-content manifest and remain readable.
        return directory / "winehua-manifest.json"

    @staticmethod
    def _is_published_revision(app_id: str, directory: Path) -> bool:
        manifest_path = SteamHostState._manifest_path(directory)
        try:
            with manifest_path.open("r", encoding="utf-8") as source:
                manifest = json.load(source)
            return (
                isinstance(manifest, dict)
                and manifest.get("schemaVersion") == PROTOCOL_VERSION
                and manifest.get("appId") == app_id
                and manifest.get("revision") == directory.name
                and isinstance(manifest.get("artifacts"), list)
            )
        except (OSError, ValueError, json.JSONDecodeError):
            return False

    def _download_job(self, job_id: str, app_id: str) -> None:
        with self.download_lock:
            self._run_download_job(job_id, app_id)

    def _run_download_job(self, job_id: str, app_id: str) -> None:
        if not self.steamcmd_download_ready():
            self._set_job_failed(job_id, "steamcmd_unconfigured")
            return
        staging = self.staging_dir / app_id / job_id
        log_path = self.logs_dir / f"{job_id}.log"
        shutil.rmtree(staging, ignore_errors=True)
        staging.mkdir(parents=True, exist_ok=True)
        self._set_job_state(job_id, "running")
        command = [
            str(self.config.steamcmd),
            "+@sSteamCmdForcePlatformType", STEAMCMD_TARGET_PLATFORM,
            "+force_install_dir", str(staging),
            "+login", self.config.steam_user,
            "+app_update", app_id, "validate",
            "+quit",
        ]
        try:
            with log_path.open("wb") as log_file:
                process = subprocess.Popen(
                    command,
                    stdin=subprocess.DEVNULL,
                    stdout=log_file,
                    stderr=subprocess.STDOUT,
                    cwd=str(self.config.steamcmd.parent),
                )
                deadline = time.monotonic() + self.config.steamcmd_timeout_seconds
                while process.poll() is None:
                    if time.monotonic() >= deadline:
                        process.terminate()
                        try:
                            process.wait(timeout=15)
                        except subprocess.TimeoutExpired:
                            process.kill()
                            process.wait()
                        self._set_job_failed(job_id, "steamcmd_timeout")
                        shutil.rmtree(staging, ignore_errors=True)
                        return
                    self._set_job_progress(job_id, directory_regular_file_bytes(staging))
                    time.sleep(1)
                return_code = process.wait()
            if return_code != 0:
                self._set_job_failed(job_id, "steamcmd_failed")
                shutil.rmtree(staging, ignore_errors=True)
                return
            self._set_job_state(job_id, "verifying")
            manifest = self.publish_revision(app_id, staging)
            total = sum(item["sizeBytes"] for item in manifest["artifacts"])
            with self.lock, self.db:
                self.db.execute(
                    "UPDATE jobs SET state = 'complete', transferred_bytes = ?, total_bytes = ?, manifest_revision = ? WHERE job_id = ?",
                    (total, total, manifest["revision"], job_id),
                )
        except (OSError, HostError):
            self._set_job_failed(job_id, "verification_failed")
            shutil.rmtree(staging, ignore_errors=True)

    def _set_job_state(self, job_id: str, state: str) -> None:
        with self.lock, self.db:
            self.db.execute("UPDATE jobs SET state = ? WHERE job_id = ?", (state, job_id))

    def _set_job_progress(self, job_id: str, transferred_bytes: int) -> None:
        with self.lock, self.db:
            self.db.execute(
                "UPDATE jobs SET transferred_bytes = MAX(transferred_bytes, ?) WHERE job_id = ?",
                (transferred_bytes, job_id),
            )

    def _set_job_failed(self, job_id: str, error_code: str) -> None:
        with self.lock, self.db:
            self.db.execute("UPDATE jobs SET state = 'failed', error_code = ? WHERE job_id = ?", (error_code, job_id))


class SteamHostRequestHandler(BaseHTTPRequestHandler):
    state: SteamHostState
    protocol_version = "HTTP/1.1"

    def log_message(self, _format: str, *_args: object) -> None:
        # Default logging includes request details. Keep tokens out of logs.
        return

    def do_GET(self) -> None:  # noqa: N802
        path = urlsplit(self.path)
        if path.query or path.fragment:
            self._json_error(HTTPStatus.BAD_REQUEST, "invalid request target")
            return
        if path.path == "/v1/capabilities":
            self._json(HTTPStatus.OK, self.state.capabilities())
            return
        if not self._authenticated():
            return
        if path.path == "/v1/library":
            self._json(HTTPStatus.OK, self.state.library())
            return
        job_match = re.fullmatch(r"/v1/downloads/([A-Za-z0-9_-]{8,128})", path.path)
        if job_match:
            try:
                with self.state.lock:
                    row = self.state.db.execute("SELECT app_id FROM jobs WHERE job_id = ?", (job_match.group(1),)).fetchone()
                if row is None:
                    raise HostError("unknown download job")
                self._json(HTTPStatus.OK, self.state.job(job_match.group(1), row["app_id"]))
            except HostError as exc:
                self._json_error(HTTPStatus.NOT_FOUND, str(exc))
            return
        manifest_match = re.fullmatch(r"/v1/games/(\d{1,20})/manifest", path.path)
        if manifest_match:
            try:
                self._json(HTTPStatus.OK, self.state.manifest(manifest_match.group(1)))
            except HostError as exc:
                self._json_error(HTTPStatus.NOT_FOUND, str(exc))
            return
        artifact_match = re.fullmatch(r"/v1/artifacts/([a-f0-9]{64})", path.path)
        if artifact_match:
            try:
                self._stream_file(self.state.artifact_path(artifact_match.group(1)))
            except HostError as exc:
                self._json_error(HTTPStatus.NOT_FOUND, str(exc))
            return
        self._json_error(HTTPStatus.NOT_FOUND, "unknown endpoint")

    def do_POST(self) -> None:  # noqa: N802
        path = urlsplit(self.path)
        if path.query or path.fragment:
            self._json_error(HTTPStatus.BAD_REQUEST, "invalid request target")
            return
        try:
            body = self._json_body()
            if path.path == "/v1/pair/begin":
                if body.get("protocolVersion") != PROTOCOL_VERSION:
                    raise HostError("unsupported protocol version")
                challenge, code = self.state.begin_pairing(str(body.get("clientNonce", "")), str(body.get("clientName", "")))
                print(f"Steam Host pairing code for {challenge['challengeId']}: {code}", flush=True)
                self._json(HTTPStatus.CREATED, challenge)
                return
            if path.path == "/v1/pair/confirm":
                self._json(HTTPStatus.CREATED, self.state.confirm_pairing(
                    str(body.get("challengeId", "")),
                    str(body.get("verificationCode", "")),
                    str(body.get("clientNonce", "")),
                ))
                return
            if path.path == "/v1/pair/revoke":
                if body.get("protocolVersion") != PROTOCOL_VERSION:
                    raise HostError("unsupported protocol version")
                token = bearer_token(self.headers)
                if not self._authenticated():
                    return
                if not self.state.revoke_pairing(token):
                    raise HostError("pairing token is already revoked")
                self._json(HTTPStatus.OK, {"revoked": True})
                return
            if not self._authenticated():
                return
            download_match = re.fullmatch(r"/v1/games/(\d{1,20})/downloads", path.path)
            if download_match:
                if body.get("protocolVersion") != PROTOCOL_VERSION:
                    raise HostError("unsupported protocol version")
                self._json(HTTPStatus.ACCEPTED, self.state.request_download(download_match.group(1)))
                return
            self._json_error(HTTPStatus.NOT_FOUND, "unknown endpoint")
        except HostError as exc:
            self._json_error(HTTPStatus.BAD_REQUEST, str(exc))

    def _authenticated(self) -> bool:
        if self.state.authenticate(bearer_token(self.headers)):
            return True
        self._json_error(HTTPStatus.UNAUTHORIZED, "pairing token required")
        return False

    def _json_body(self) -> dict[str, Any]:
        size = int(self.headers.get("Content-Length", "0"))
        if size <= 0 or size > 65536:
            raise HostError("invalid request body length")
        try:
            value = json.loads(self.rfile.read(size).decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError) as exc:
            raise HostError("invalid JSON request body") from exc
        if not isinstance(value, dict):
            raise HostError("request body must be an object")
        return value

    def _json(self, status: HTTPStatus, value: Any) -> None:
        payload = json.dumps(value, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(payload)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(payload)

    def _json_error(self, status: HTTPStatus, error: str) -> None:
        self._json(status, {"error": error})

    def _stream_file(self, path: Path) -> None:
        self.send_response(HTTPStatus.OK)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(path.stat().st_size))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        with path.open("rb") as source:
            shutil.copyfileobj(source, self.wfile, length=1024 * 1024)


def _der_tlv_bounds(data: bytes, offset: int) -> tuple[int, int, int, int]:
    """Return tag, content start, content end and next offset for one DER TLV."""
    if offset >= len(data):
        raise ValueError("missing DER tag")
    tag = data[offset]
    header_end = offset + 1
    if header_end >= len(data):
        raise ValueError("missing DER length")
    first_length = data[header_end]
    header_end += 1
    if first_length < 0x80:
        length = first_length
    else:
        length_size = first_length & 0x7F
        if length_size == 0 or length_size > 4 or header_end + length_size > len(data):
            raise ValueError("invalid DER length")
        length = int.from_bytes(data[header_end:header_end + length_size], "big")
        header_end += length_size
        if length < 0x80:
            raise ValueError("DER length is not minimally encoded")
    content_end = header_end + length
    if content_end > len(data):
        raise ValueError("truncated DER value")
    return tag, header_end, content_end, content_end


def _certificate_spki_der(certificate_der: bytes) -> bytes:
    """Extract Certificate.tbsCertificate.subjectPublicKeyInfo from DER."""
    outer_tag, outer_start, outer_end, outer_next = _der_tlv_bounds(certificate_der, 0)
    if outer_tag != 0x30 or outer_next != len(certificate_der):
        raise ValueError("certificate is not one DER sequence")
    tbs_offset = outer_start
    tbs_tag, tbs_start, tbs_end, _ = _der_tlv_bounds(certificate_der, tbs_offset)
    if tbs_tag != 0x30 or tbs_end > outer_end:
        raise ValueError("certificate has no TBSCertificate sequence")

    offset = tbs_start
    tag, _, _, next_offset = _der_tlv_bounds(certificate_der, offset)
    if tag == 0xA0:  # [0] EXPLICIT version, optional in v1 certificates.
        offset = next_offset
    # serialNumber, signature, issuer, validity and subject precede SPKI.
    for _ in range(5):
        _, _, _, offset = _der_tlv_bounds(certificate_der, offset)
        if offset > tbs_end:
            raise ValueError("truncated TBSCertificate")
    spki_offset = offset
    spki_tag, _, spki_end, spki_next = _der_tlv_bounds(certificate_der, spki_offset)
    if spki_tag != 0x30 or spki_next > tbs_end:
        raise ValueError("certificate has no SubjectPublicKeyInfo sequence")
    return certificate_der[spki_offset:spki_end]


def certificate_spki_pin(certificate: Path) -> str:
    try:
        certificate_der = ssl.PEM_cert_to_DER_cert(certificate.read_text(encoding="ascii"))
        spki_der = _certificate_spki_der(certificate_der)
    except (OSError, UnicodeDecodeError, ValueError) as exc:
        raise HostError("unable to read the TLS certificate SPKI") from exc
    return base64.b64encode(hashlib.sha256(spki_der).digest()).decode("ascii")


def load_tls_context(certificate: Path, key: Path) -> tuple[ssl.SSLContext, str]:
    """Validate Host TLS material without opening a listening socket."""
    if not certificate.is_file() or not key.is_file():
        raise HostError("a TLS certificate and private key are required")
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.minimum_version = ssl.TLSVersion.TLSv1_2
    try:
        context.load_cert_chain(certificate, key)
    except (OSError, ssl.SSLError) as exc:
        raise HostError("unable to load the TLS certificate and private key") from exc
    return context, certificate_spki_pin(certificate)


def validate_tls_origin(server_context: ssl.SSLContext, certificate: Path, origin: str) -> None:
    """Verify that the configured certificate can serve the configured origin.

    The Host cannot inspect the device CA store, but it can prove locally that
    the loaded key and certificate complete a TLS handshake whose hostname
    check uses the exact host from the pinned HTTPS origin.
    """
    hostname = urlsplit(origin).hostname
    if not hostname:
        raise HostError("origin has no hostname for TLS validation")
    try:
        client_context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
        client_context.minimum_version = ssl.TLSVersion.TLSv1_2
        # A leaf or intermediate in the supplied full-chain PEM is sufficient
        # for this local identity check. This does not claim device CA trust.
        partial_chain = getattr(ssl, "VERIFY_X509_PARTIAL_CHAIN", 0)
        if partial_chain:
            client_context.verify_flags |= partial_chain
        client_context.load_verify_locations(cafile=str(certificate))
    except (OSError, ssl.SSLError, ValueError) as exc:
        raise HostError("unable to load the TLS certificate for origin validation") from exc

    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind(("127.0.0.1", 0))
    listener.listen(1)
    listener.settimeout(5)
    handshake_complete = threading.Event()
    server_errors: list[BaseException] = []

    def accept_once() -> None:
        try:
            connection, _address = listener.accept()
            with connection:
                with server_context.wrap_socket(connection, server_side=True) as tls_connection:
                    if tls_connection.recv(1) != b"\0":
                        raise OSError("TLS origin client confirmation failed")
                    tls_connection.sendall(b"\0")
        except (OSError, ssl.SSLError) as exc:
            server_errors.append(exc)
        finally:
            handshake_complete.set()

    worker = threading.Thread(target=accept_once, daemon=True)
    worker.start()
    client_error: BaseException | None = None
    try:
        with socket.create_connection(("127.0.0.1", listener.getsockname()[1]), timeout=5) as connection:
            with client_context.wrap_socket(connection, server_hostname=hostname) as tls_connection:
                tls_connection.sendall(b"\0")
                if tls_connection.recv(1) != b"\0":
                    raise OSError("TLS origin server confirmation failed")
    except (OSError, ssl.SSLError, ValueError) as exc:
        client_error = exc
    finally:
        listener.close()
        worker.join(5)

    if client_error is not None:
        raise HostError("TLS certificate does not match the configured origin") from client_error
    if not handshake_complete.is_set():
        raise HostError("local TLS origin validation timed out")
    if server_errors:
        raise HostError("local TLS origin validation failed") from server_errors[0]


def run_preflight(args: argparse.Namespace) -> int:
    """Check the complete Host download path without accepting a client."""
    require_arguments(args, "data_dir", "origin", "config", "cert", "key")
    config = load_configuration(args)
    validate_server_port(config.origin, args.port)
    context, public_key_pin = load_tls_context(Path(args.cert).resolve(), Path(args.key).resolve())
    validate_tls_origin(context, Path(args.cert).resolve(), config.origin)
    state = SteamHostState(config)
    try:
        session_state = state.check_steamcmd_session()
        download_ready = state.steamcmd_download_ready()
        report = {
            "schemaVersion": 1,
            "origin": config.origin,
            "hostId": config.host_id,
            "tls": {
                "minimumVersion": "TLSv1.2",
                "publicKeyPin": public_key_pin,
                "originHostnameChecked": True,
                "deviceTrustChecked": False,
            },
            "apps": {
                "approvedCount": len(config.apps),
                "launchReadyCount": sum(1 for policy in config.apps.values() if policy.launch_ready),
            },
            "steamcmd": {
                "configured": state.steamcmd_configured(),
                "sessionState": session_state,
                "targetPlatform": STEAMCMD_TARGET_PLATFORM,
            },
            "downloadReady": download_ready,
        }
        print(json.dumps(report, sort_keys=True, separators=(",", ":")), flush=True)
        return 0 if download_ready else 1
    finally:
        state.close()


def find_windows_executables(root: Path) -> list[dict[str, Any]]:
    """List only regular Windows executables from a Host-private probe."""
    candidates: list[dict[str, Any]] = []
    for directory, names, files in os.walk(root, followlinks=False):
        directory_path = Path(directory)
        names[:] = [name for name in names if not (directory_path / name).is_symlink()]
        for name in files:
            path = directory_path / name
            try:
                if path.is_symlink() or not path.is_file() or path.suffix.lower() != ".exe":
                    continue
                relative = path.relative_to(root).as_posix()
                candidates.append({"relativePath": relative, "sizeBytes": path.stat().st_size})
            except (OSError, ValueError):
                continue
    return sorted(candidates, key=lambda item: item["relativePath"].lower())


def run_probe_app(args: argparse.Namespace) -> int:
    """Use official SteamCMD locally to discover a reviewed title's EXEs."""
    require_arguments(args, "data_dir", "origin", "config", "probe_app")
    if not APP_ID_RE.fullmatch(args.probe_app):
        raise HostError("--probe-app must be a numeric Steam AppID")
    config = load_configuration(args)
    state = SteamHostState(config)
    probe_root = state.data_dir / "probes" / args.probe_app / uuid.uuid4().hex
    log_path = state.logs_dir / f"probe-{args.probe_app}-{uuid.uuid4().hex}.log"
    try:
        session_state = state.check_steamcmd_session()
        if not state.steamcmd_download_ready():
            print(json.dumps({
                "schemaVersion": 1,
                "appId": args.probe_app,
                "sessionState": session_state,
                "probeReady": False,
            }, sort_keys=True, separators=(",", ":")), flush=True)
            return 1
        probe_root.mkdir(parents=True, exist_ok=False)
        command = [
            str(config.steamcmd),
            "+@sSteamCmdForcePlatformType", STEAMCMD_TARGET_PLATFORM,
            "+force_install_dir", str(probe_root),
            "+login", config.steam_user,
            "+app_update", args.probe_app, "validate",
            "+quit",
        ]
        try:
            with log_path.open("wb") as log_file:
                completed = subprocess.run(
                    command,
                    stdin=subprocess.DEVNULL,
                    stdout=log_file,
                    stderr=subprocess.STDOUT,
                    cwd=str(config.steamcmd.parent),
                    timeout=config.steamcmd_timeout_seconds,
                    check=False,
                )
        except subprocess.TimeoutExpired:
            print(json.dumps({
                "schemaVersion": 1,
                "appId": args.probe_app,
                "probeReady": False,
                "errorCode": "steamcmd_timeout",
            }, sort_keys=True, separators=(",", ":")), flush=True)
            return 1
        except OSError:
            print(json.dumps({
                "schemaVersion": 1,
                "appId": args.probe_app,
                "probeReady": False,
                "errorCode": "steamcmd_unavailable",
            }, sort_keys=True, separators=(",", ":")), flush=True)
            return 1
        if completed.returncode != 0:
            print(json.dumps({
                "schemaVersion": 1,
                "appId": args.probe_app,
                "probeReady": False,
                "errorCode": "steamcmd_failed",
            }, sort_keys=True, separators=(",", ":")), flush=True)
            return 1
        print(json.dumps({
            "schemaVersion": 1,
            "appId": args.probe_app,
            "platform": STEAMCMD_TARGET_PLATFORM,
            "probeReady": True,
            "executables": find_windows_executables(probe_root),
        }, sort_keys=True, separators=(",", ":")), flush=True)
        return 0
    finally:
        shutil.rmtree(probe_root, ignore_errors=True)
        state.close()


def run_server(args: argparse.Namespace) -> int:
    require_arguments(args, "data_dir", "origin", "config", "cert", "key")
    config = load_configuration(args)
    validate_server_port(config.origin, args.port)
    certificate = Path(args.cert).resolve()
    key = Path(args.key).resolve()
    context, public_key_pin = load_tls_context(certificate, key)
    validate_tls_origin(context, certificate, config.origin)
    state = SteamHostState(config)
    session_state = state.check_steamcmd_session()
    handler = type("ConfiguredSteamHostRequestHandler", (SteamHostRequestHandler,), {"state": state})
    server = ThreadingHTTPServer((args.bind, args.port), handler)
    server.socket = context.wrap_socket(server.socket, server_side=True)
    print(f"Steam Host listening at {config.origin}", flush=True)
    print(f"SPKI SHA-256 pin: {public_key_pin}", flush=True)
    if session_state == "ready":
        print("SteamCMD session: ready for Windows depot downloads", flush=True)
    elif session_state != "unconfigured":
        print(
            "SteamCMD session is not ready; downloads are disabled. "
            "Run --steamcmd-login in the same SteamCMD directory, then restart the Host.",
            flush=True,
        )
    try:
        server.serve_forever()
    finally:
        server.server_close()
        state.close()
    return 0


def run_steamcmd_login(args: argparse.Namespace) -> int:
    require_arguments(args, "steamcmd", "steam_user")
    steamcmd = Path(args.steamcmd).expanduser().resolve()
    if not steamcmd.is_file():
        raise HostError("SteamCMD executable does not exist")
    # Deliberately inherit the operator terminal. Password and Steam Guard
    # input go straight to the official SteamCMD process and are neither read
    # nor logged by this service.
    completed = subprocess.run(
        [str(steamcmd), "+login", args.steam_user, "+quit"],
        cwd=str(steamcmd.parent),
        check=False,
    )
    if completed.returncode != 0:
        raise HostError(f"SteamCMD login exited with status {completed.returncode}")
    return 0


def run_steamcmd_status(args: argparse.Namespace) -> int:
    require_arguments(args, "data_dir", "steamcmd", "steam_user")
    steamcmd = Path(args.steamcmd).expanduser().resolve()
    log_path = Path(args.data_dir).resolve() / "logs" / "steamcmd-session-check.log"
    status = probe_steamcmd_session(steamcmd, args.steam_user, log_path)
    print(f"SteamCMD session: {status}", flush=True)
    if status != "ready":
        print(
            "Run --steamcmd-login in the same SteamCMD directory before starting the Host.",
            file=sys.stderr,
            flush=True,
        )
        return 1
    return 0


def require_arguments(args: argparse.Namespace, *names: str) -> None:
    missing = [name.replace("_", "-") for name in names if not getattr(args, name)]
    if missing:
        raise HostError("missing required argument(s): " + ", ".join(missing))


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--data-dir", help="Host-private persistent data directory")
    parser.add_argument("--origin", help="Pinned HTTPS origin exposed to WineHua")
    parser.add_argument("--host-id", help="Stable Host identifier; otherwise read from config")
    parser.add_argument("--config", help="JSON policy file containing approved AppIDs")
    parser.add_argument("--cert", help="PEM TLS certificate")
    parser.add_argument("--key", help="PEM TLS private key")
    parser.add_argument("--bind", default="0.0.0.0", help="Local listen address")
    parser.add_argument("--port", type=int, default=8443, help="Local listen port")
    parser.add_argument("--steamcmd", help="SteamCMD executable path")
    parser.add_argument("--steam-user", help="SteamCMD account already authenticated on the Host")
    parser.add_argument("--steamcmd-timeout-seconds", type=int,
                        help="SteamCMD download timeout, from 60 seconds through 24 hours")
    action = parser.add_mutually_exclusive_group()
    action.add_argument("--print-pin", action="store_true", help="Print certificate SPKI pin and exit")
    action.add_argument("--steamcmd-login", action="store_true",
                        help="Run the official SteamCMD interactive login and exit")
    action.add_argument("--steamcmd-status", action="store_true",
                        help="Verify the existing SteamCMD session and exit")
    action.add_argument("--preflight", action="store_true",
                        help="Validate TLS, policy storage, and SteamCMD download readiness, then exit")
    action.add_argument("--probe-app", metavar="APP_ID",
                        help="Download a Windows depot locally and list EXE candidates without serving it")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv or sys.argv[1:])
    if args.print_pin:
        require_arguments(args, "cert")
        print(certificate_spki_pin(Path(args.cert).resolve()))
        return 0
    if args.steamcmd_login:
        return run_steamcmd_login(args)
    if args.steamcmd_status:
        return run_steamcmd_status(args)
    if args.preflight:
        return run_preflight(args)
    if args.probe_app:
        return run_probe_app(args)
    return run_server(args)


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (HostError, subprocess.CalledProcessError) as error:
        print(f"steam-host: {error}", file=sys.stderr)
        raise SystemExit(2) from error
