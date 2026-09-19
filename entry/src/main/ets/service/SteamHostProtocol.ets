import { normalizeGameContainerId } from './GameContainer';

// Steam Host is a separately deployed, user-controlled service. These types
// deliberately cover content delivery only: Steam account credentials and
// Steam Guard stay on the host that runs the official Steam tooling.
export const STEAM_HOST_PROTOCOL_VERSION = 1;

const MAX_HOST_ORIGIN_LENGTH = 320;
const MAX_APP_ID_LENGTH = 20;
const MAX_ARTIFACTS = 200000;

export interface SteamHostCapabilities {
  protocolVersion: number;
  hostId: string;
  libraryRead: boolean;
  managedDownloads: boolean;
  artifactChecksums: boolean;
}

export interface SteamHostLibraryEntry {
  appId: string;
  name: string;
  installed: boolean;
  downloadable: boolean;
  launchReady: boolean;
}

export interface SteamHostPairBeginRequest {
  clientName: string;
  clientNonce: string;
  protocolVersion: number;
}

export interface SteamHostPairChallenge {
  challengeId: string;
  expiresAtMs: number;
  hostId: string;
}

export interface SteamHostPairConfirmRequest {
  challengeId: string;
  verificationCode: string;
  clientNonce: string;
}

export interface SteamHostPairing {
  hostId: string;
  pairingToken: string;
}

export interface SteamHostArtifact {
  // Paths are relative to games/<appId>/, never to an OHOS filesystem root.
  relativePath: string;
  sizeBytes: number;
  sha256: string;
  downloadUrl: string;
}

export interface SteamHostGameManifest {
  schemaVersion: number;
  revision: string;
  appId: string;
  containerId: string;
  installRelativePath: string;
  launchExeRelativePath: string;
  workingDirectoryRelativePath: string;
  launchReady: boolean;
  artifacts: SteamHostArtifact[];
}

export interface SteamHostDownloadJob {
  jobId: string;
  appId: string;
  state: 'queued' | 'running' | 'verifying' | 'complete' | 'failed' | 'cancelled';
  transferredBytes: number;
  totalBytes: number;
  manifestRevision: string;
  errorCode?: string;
}

export interface SteamHostRequest {
  method: 'GET' | 'POST';
  path: string;
  body?: string;
  bearerToken?: string;
}

export interface SteamHostResponse {
  status: number;
  body: string;
}

export interface SteamHostArtifactDownloadRequest {
  url: string;
  destinationPath: string;
  bearerToken: string;
}

export interface SteamHostArtifactDownloadResult {
  status: number;
  bytesWritten: number;
}

// The real implementation pins the paired host certificate. Keeping this
// injected makes the protocol and installer testable without a device.
export interface SteamHostTransport {
  request(request: SteamHostRequest): Promise<SteamHostResponse>;
}

export interface SteamHostArtifactTransport extends SteamHostTransport {
  downloadArtifact(request: SteamHostArtifactDownloadRequest): Promise<SteamHostArtifactDownloadResult>;
}

export interface SteamHostManifestValidation {
  valid: boolean;
  error: string;
}

export function isSteamAppId(value: string): boolean {
  return new RegExp(`^\\d{1,${MAX_APP_ID_LENGTH}}$`).test(value);
}

export function steamContainerIdForApp(appId: string): string {
  return `steam-${appId}`;
}

export function steamInstallRelativePathForApp(appId: string): string {
  return `games/${appId}`;
}

// A Host endpoint is an origin rather than a general URL. In particular, it
// cannot contain a path, userinfo, query or fragment that could redirect an
// artifact request away from the paired service.
export function normalizeSteamHostOrigin(value: string): string | null {
  if (value.length === 0 || value.length > MAX_HOST_ORIGIN_LENGTH ||
      value !== value.trim()) {
    return null;
  }
  const match = /^https:\/\/([A-Za-z0-9](?:[A-Za-z0-9.-]{0,251}[A-Za-z0-9])?|\[[0-9A-Fa-f:]+\])(?::([0-9]{1,5}))?$/.exec(value);
  if (match === null) return null;
  const host = match[1];
  if (host.indexOf('..') >= 0 || host.startsWith('.') || host.endsWith('.')) return null;
  if (match[2] !== undefined) {
    const port = Number(match[2]);
    if (!Number.isSafeInteger(port) || port < 1 || port > 65535) return null;
  }
  return `https://${host.toLowerCase()}${match[2] === undefined ? '' : `:${match[2]}`}`;
}

// NetworkKit expects the SHA-256 SubjectPublicKeyInfo digest as Base64. The
// pairing QR/user flow supplies this value; it is never learned from an
// unauthenticated HTTPS response.
export function isSteamHostCertificatePin(value: string): boolean {
  return /^[A-Za-z0-9+/]{43}=$/.test(value);
}

export function isSafeHostRelativePath(path: string, requireExe: boolean = false): boolean {
  if (path.length === 0 || path.length > 1024 || path.startsWith('/') || path.indexOf('\\') >= 0) {
    return false;
  }
  const pieces = path.split('/');
  for (const piece of pieces) {
    if (piece.length === 0 || piece === '.' || piece === '..') return false;
  }
  return !requireExe || path.toLowerCase().endsWith('.exe');
}

function isSafeWorkingDirectory(path: string): boolean {
  return path === '.' || isSafeHostRelativePath(path);
}

export function isSha256(value: string): boolean {
  return /^[a-f0-9]{64}$/.test(value);
}

function isHostId(value: string): boolean {
  return /^[A-Za-z0-9._-]{1,128}$/.test(value);
}

function isPairingToken(value: string): boolean {
  return /^[A-Za-z0-9._~+/=-]{16,1024}$/.test(value);
}

function isChallengeToken(value: string): boolean {
  return /^[A-Za-z0-9_-]{8,128}$/.test(value);
}

function isRevision(value: string): boolean {
  return /^[A-Za-z0-9._-]{1,128}$/.test(value);
}

function isValidArtifactUrl(url: string, origin: string, sha256: string): boolean {
  return url === `${origin}/v1/artifacts/${sha256}`;
}

function isValidLibraryEntry(entry: SteamHostLibraryEntry): boolean {
  return isSteamAppId(entry.appId) && typeof entry.name === 'string' &&
    entry.name.length > 0 && entry.name.length <= 512 &&
    typeof entry.installed === 'boolean' && typeof entry.downloadable === 'boolean' &&
    typeof entry.launchReady === 'boolean';
}

function isValidDownloadJob(job: SteamHostDownloadJob, expectedAppId: string): boolean {
  const states = ['queued', 'running', 'verifying', 'complete', 'failed', 'cancelled'];
  const revisionIsValid = job.manifestRevision.length === 0 ? job.state !== 'complete' :
    isRevision(job.manifestRevision);
  const progressIsValid = job.totalBytes === 0 ?
    (job.state !== 'complete' || job.transferredBytes === 0) :
    job.transferredBytes <= job.totalBytes;
  return isChallengeToken(job.jobId) && job.appId === expectedAppId &&
    states.indexOf(job.state) >= 0 && Number.isSafeInteger(job.transferredBytes) &&
    Number.isSafeInteger(job.totalBytes) && job.transferredBytes >= 0 &&
    job.totalBytes >= 0 && progressIsValid &&
    revisionIsValid &&
    (job.errorCode === undefined || /^[A-Za-z0-9._-]{1,128}$/.test(job.errorCode));
}

export function validateSteamHostManifest(
  manifest: SteamHostGameManifest, pairedOrigin: string
): SteamHostManifestValidation {
  const origin = normalizeSteamHostOrigin(pairedOrigin);
  if (origin === null) return { valid: false, error: 'invalid paired host origin' };
  if (manifest.schemaVersion !== STEAM_HOST_PROTOCOL_VERSION) {
    return { valid: false, error: 'unsupported manifest schema' };
  }
  if (!isSteamAppId(manifest.appId) || !isRevision(manifest.revision)) {
    return { valid: false, error: 'invalid application identity' };
  }
  const expectedContainer = steamContainerIdForApp(manifest.appId);
  if (manifest.containerId !== expectedContainer ||
      normalizeGameContainerId(manifest.containerId) !== expectedContainer) {
    return { valid: false, error: 'unexpected game container' };
  }
  if (manifest.installRelativePath !== steamInstallRelativePathForApp(manifest.appId) ||
      !isSafeHostRelativePath(manifest.launchExeRelativePath, true) ||
      !isSafeWorkingDirectory(manifest.workingDirectoryRelativePath) ||
      typeof manifest.launchReady !== 'boolean') {
    return { valid: false, error: 'unsafe game path' };
  }
  if (!Array.isArray(manifest.artifacts) || manifest.artifacts.length === 0 ||
      manifest.artifacts.length > MAX_ARTIFACTS) {
    return { valid: false, error: 'invalid artifact count' };
  }
  const paths = new Set<string>();
  let launchArtifactPresent = false;
  for (const artifact of manifest.artifacts) {
    if (!isSafeHostRelativePath(artifact.relativePath) || !isSha256(artifact.sha256) ||
        !Number.isSafeInteger(artifact.sizeBytes) || artifact.sizeBytes < 0 ||
        !isValidArtifactUrl(artifact.downloadUrl, origin, artifact.sha256) ||
        paths.has(artifact.relativePath)) {
      return { valid: false, error: 'invalid artifact' };
    }
    paths.add(artifact.relativePath);
    if (artifact.relativePath === manifest.launchExeRelativePath) launchArtifactPresent = true;
  }
  if (!launchArtifactPresent) return { valid: false, error: 'launch executable is not an artifact' };
  return { valid: true, error: '' };
}

export class SteamHostClient {
  private readonly transport: SteamHostTransport;
  private readonly origin: string;
  private pairingToken: string = '';

  constructor(transport: SteamHostTransport, pairedOrigin: string) {
    this.transport = transport;
    this.origin = normalizeSteamHostOrigin(pairedOrigin) ?? '';
  }

  setPairingToken(token: string): void {
    this.pairingToken = isPairingToken(token) ? token : '';
  }

  clearPairingToken(): void {
    this.pairingToken = '';
  }

  hasPairingToken(): boolean {
    return this.pairingToken.length > 0;
  }

  getPairedOrigin(): string {
    return this.origin;
  }

  artifactTransport(): SteamHostArtifactTransport | null {
    const candidate = this.transport as SteamHostArtifactTransport;
    return typeof candidate.downloadArtifact === 'function' ? candidate : null;
  }

  async getCapabilities(): Promise<SteamHostCapabilities | null> {
    if (this.origin.length === 0) return null;
    const response = await this.transport.request({ method: 'GET', path: '/v1/capabilities' });
    if (response.status !== 200) return null;
    try {
      const parsed = JSON.parse(response.body) as SteamHostCapabilities;
      if (parsed.protocolVersion !== STEAM_HOST_PROTOCOL_VERSION || !isHostId(parsed.hostId) ||
          typeof parsed.libraryRead !== 'boolean' || typeof parsed.managedDownloads !== 'boolean' ||
          typeof parsed.artifactChecksums !== 'boolean') return null;
      return parsed;
    } catch (_) {
      return null;
    }
  }

  async beginPairing(request: SteamHostPairBeginRequest): Promise<SteamHostPairChallenge | null> {
    if (this.origin.length === 0 || request.protocolVersion !== STEAM_HOST_PROTOCOL_VERSION ||
        !isChallengeToken(request.clientNonce) || request.clientName.length === 0 ||
        request.clientName.length > 128) return null;
    const response = await this.transport.request({
      method: 'POST', path: '/v1/pair/begin', body: JSON.stringify(request)
    });
    if (response.status !== 200 && response.status !== 201) return null;
    try {
      const parsed = JSON.parse(response.body) as SteamHostPairChallenge;
      if (!isChallengeToken(parsed.challengeId) || !Number.isSafeInteger(parsed.expiresAtMs) ||
          parsed.expiresAtMs <= Date.now() ||
          !isHostId(parsed.hostId)) return null;
      return parsed;
    } catch (_) {
      return null;
    }
  }

  async confirmPairing(request: SteamHostPairConfirmRequest): Promise<SteamHostPairing | null> {
    if (this.origin.length === 0 || !isChallengeToken(request.challengeId) ||
        !/^[A-Z0-9-]{4,32}$/.test(request.verificationCode) || !isChallengeToken(request.clientNonce)) {
      return null;
    }
    const response = await this.transport.request({
      method: 'POST', path: '/v1/pair/confirm', body: JSON.stringify(request)
    });
    if (response.status !== 200 && response.status !== 201) return null;
    try {
      const parsed = JSON.parse(response.body) as SteamHostPairing;
      if (!isHostId(parsed.hostId) || !isPairingToken(parsed.pairingToken)) return null;
      return parsed;
    } catch (_) {
      return null;
    }
  }

  async getLibrary(): Promise<SteamHostLibraryEntry[] | null> {
    if (!this.hasPairingToken()) return null;
    const response = await this.transport.request({
      method: 'GET', path: '/v1/library', bearerToken: this.pairingToken
    });
    if (response.status !== 200) return null;
    try {
      const parsed = JSON.parse(response.body) as SteamHostLibraryEntry[];
      if (!Array.isArray(parsed) || parsed.length > 50000) return null;
      for (const entry of parsed) if (!isValidLibraryEntry(entry)) return null;
      return parsed;
    } catch (_) {
      return null;
    }
  }

  async revokePairing(): Promise<boolean> {
    if (!this.hasPairingToken()) return false;
    const response = await this.transport.request({
      method: 'POST', path: '/v1/pair/revoke', bearerToken: this.pairingToken,
      body: JSON.stringify({ protocolVersion: STEAM_HOST_PROTOCOL_VERSION })
    });
    if (response.status !== 200) return false;
    try {
      const parsed = JSON.parse(response.body) as Record<string, Object>;
      return parsed['revoked'] === true;
    } catch (_) {
      return false;
    }
  }

  async requestDownload(appId: string): Promise<SteamHostDownloadJob | null> {
    if (!isSteamAppId(appId) || !this.hasPairingToken()) return null;
    const response = await this.transport.request({
      method: 'POST', path: `/v1/games/${appId}/downloads`, bearerToken: this.pairingToken,
      body: JSON.stringify({ protocolVersion: STEAM_HOST_PROTOCOL_VERSION })
    });
    if (response.status !== 200 && response.status !== 201 && response.status !== 202) return null;
    try {
      const parsed = JSON.parse(response.body) as SteamHostDownloadJob;
      return isValidDownloadJob(parsed, appId) ? parsed : null;
    } catch (_) {
      return null;
    }
  }

  async getDownloadJob(appId: string, jobId: string): Promise<SteamHostDownloadJob | null> {
    if (!isSteamAppId(appId) || !isChallengeToken(jobId) || !this.hasPairingToken()) return null;
    const response = await this.transport.request({
      method: 'GET', path: `/v1/downloads/${jobId}`, bearerToken: this.pairingToken
    });
    if (response.status !== 200) return null;
    try {
      const parsed = JSON.parse(response.body) as SteamHostDownloadJob;
      return isValidDownloadJob(parsed, appId) && parsed.jobId === jobId ? parsed : null;
    } catch (_) {
      return null;
    }
  }

  async getManifest(appId: string): Promise<SteamHostGameManifest | null> {
    if (!isSteamAppId(appId) || !this.hasPairingToken() || this.origin.length === 0) return null;
    const response = await this.transport.request({
      method: 'GET', path: `/v1/games/${appId}/manifest`, bearerToken: this.pairingToken
    });
    if (response.status !== 200) return null;
    try {
      const manifest = JSON.parse(response.body) as SteamHostGameManifest;
      return validateSteamHostManifest(manifest, this.origin).valid ? manifest : null;
    } catch (_) {
      return null;
    }
  }

  pairingTokenForArtifactDownload(): string {
    return this.pairingToken;
  }
}
