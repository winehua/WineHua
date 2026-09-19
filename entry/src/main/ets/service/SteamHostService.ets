import { common } from '@kit.AbilityKit';
import { cryptoFramework } from '@kit.CryptoArchitectureKit';
import { fileIo as fs, hash } from '@kit.CoreFileKit';
import { preferences } from '@kit.ArkData';
import { http } from '@kit.NetworkKit';
import testNapi from 'libentry.so';
import {
  STEAM_HOST_PROTOCOL_VERSION,
  SteamHostArtifactDownloadRequest,
  SteamHostArtifactDownloadResult,
  SteamHostClient,
  SteamHostDownloadJob,
  SteamHostGameManifest,
  SteamHostLibraryEntry,
  SteamHostPairChallenge,
  SteamHostPairConfirmRequest,
  SteamHostRequest,
  SteamHostResponse,
  SteamHostTransport,
  isSteamAppId,
  isSteamHostCertificatePin,
  normalizeSteamHostOrigin,
  steamContainerIdForApp,
  steamInstallRelativePathForApp,
  validateSteamHostManifest
} from './SteamHostProtocol';
import { wineDriveCForContainer } from './GameContainer';
import { WineEnvService } from './WineEnvService';
import { WineWindowManager } from './WineWindowManager';

const STORE_NAME = 'steam_host';
const STORE_ORIGIN = 'origin';
const STORE_PIN = 'certificate_pin';
const STORE_HOST_ID = 'host_id';
const STORE_TOKEN = 'pairing_token';
export interface SteamHostPairingSummary {
  paired: boolean;
  origin: string;
  hostId: string;
}

export interface SteamHostInstallResult {
  installed: boolean;
  error: string;
  appId: string;
  containerId: string;
  revision: string;
  windowsExePath: string;
  windowsWorkingDirectory: string;
}

export interface SteamHostLaunchResult {
  launched: boolean;
  error: string;
  appId: string;
  containerId: string;
  revision: string;
  windowsExePath: string;
  windowsWorkingDirectory: string;
  pid: number;
}

interface LocalInstallationRecord {
  schemaVersion: number;
  appId: string;
  containerId: string;
  revision: string;
  launchExePath: string;
  workingDirectory: string;
}

function pathExists(path: string): boolean {
  try {
    return fs.accessSync(path);
  } catch (_) {
    return false;
  }
}

function ensureDirectory(path: string): boolean {
  try {
    if (!pathExists(path)) fs.mkdirSync(path, true);
    return pathExists(path);
  } catch (_) {
    return false;
  }
}

function removePathRecursive(path: string): void {
  const stat = fs.lstatSync(path);
  if (stat.isSymbolicLink() || !stat.isDirectory()) {
    fs.unlinkSync(path);
    return;
  }
  for (const name of fs.listFileSync(path)) {
    removePathRecursive(`${path}/${name}`);
  }
  fs.rmdirSync(path);
}

function removeIfPresent(path: string): void {
  try {
    if (pathExists(path)) removePathRecursive(path);
  } catch (_) {
  }
}

function parentPath(path: string): string {
  const slash = path.lastIndexOf('/');
  return slash > 0 ? path.substring(0, slash) : '';
}

function writeTextAtomically(path: string, text: string): void {
  const parent = parentPath(path);
  if (!ensureDirectory(parent)) throw new Error('unable to create manifest parent');
  const temporary = `${path}.part`;
  removeIfPresent(temporary);
  const file = fs.openSync(temporary, fs.OpenMode.CREATE | fs.OpenMode.WRITE_ONLY | fs.OpenMode.TRUNC);
  try {
    const written = fs.writeSync(file.fd, text);
    if (written !== text.length) throw new Error('local manifest write was short');
  } finally {
    fs.closeSync(file);
  }
  fs.renameSync(temporary, path);
}

function readTextFile(path: string): string {
  const file = fs.openSync(path, fs.OpenMode.READ_ONLY);
  try {
    const size = fs.statSync(path).size;
    if (!Number.isSafeInteger(size) || size <= 0 || size > 65536) return '';
    const buffer = new ArrayBuffer(size);
    const bytesRead = fs.readSync(file.fd, buffer);
    if (bytesRead !== size) return '';
    let text = '';
    const bytes = new Uint8Array(buffer);
    for (let index = 0; index < bytes.length; ++index) text += String.fromCharCode(bytes[index]);
    return text;
  } catch (_) {
    return '';
  } finally {
    fs.closeSync(file);
  }
}

function windowsPath(installRelativePath: string, revision: string, relativePath: string): string {
  const install = installRelativePath.replace(/\//g, '\\');
  const relative = relativePath.replace(/\//g, '\\');
  return `C:\\${install}\\releases\\${revision}\\${relative}`;
}

function workingDirectoryPath(manifest: SteamHostGameManifest): string {
  if (manifest.workingDirectoryRelativePath === '.') {
    return windowsPath(manifest.installRelativePath, manifest.revision, '');
  }
  return windowsPath(manifest.installRelativePath, manifest.revision,
    manifest.workingDirectoryRelativePath);
}

function localDriveCPath(driveC: string, windowsPath: string): string {
  return `${driveC}/${windowsPath.substring(3).replace(/\\/g, '/')}`;
}

function validStoredHost(origin: string, pin: string, hostId: string, token: string): boolean {
  return normalizeSteamHostOrigin(origin) !== null && isSteamHostCertificatePin(pin) &&
    /^[A-Za-z0-9._-]{1,128}$/.test(hostId) && /^[A-Za-z0-9._~+/=-]{16,1024}$/.test(token);
}

// HTTPS is the only network transport. A paired pin applies to every request,
// including artifact streams. System CA verification remains active as well.
export class PinnedSteamHostTransport implements SteamHostTransport {
  private readonly origin: string;
  private readonly publicKeyPin: string;

  constructor(origin: string, publicKeyPin: string) {
    this.origin = normalizeSteamHostOrigin(origin) ?? '';
    this.publicKeyPin = isSteamHostCertificatePin(publicKeyPin) ? publicKeyPin : '';
  }

  private requestOptions(request: SteamHostRequest): http.HttpRequestOptions {
    const headers: Record<string, string> = {
      'accept': 'application/json'
    };
    if (request.method === 'POST') headers['content-type'] = 'application/json';
    if (request.bearerToken !== undefined && request.bearerToken.length > 0) {
      headers['authorization'] = `Bearer ${request.bearerToken}`;
    }
    return {
      method: request.method === 'POST' ? http.RequestMethod.POST : http.RequestMethod.GET,
      extraData: request.body,
      expectDataType: http.HttpDataType.STRING,
      header: headers,
      connectTimeout: 15000,
      readTimeout: 60000,
      usingCache: false,
      usingProxy: false,
      maxRedirects: 0,
      certificatePinning: [{ publicKeyHash: this.publicKeyPin, hashAlgorithm: 'SHA-256' }]
    };
  }

  private validRequestPath(path: string): boolean {
    return /^\/v1\/(capabilities|pair\/(begin|confirm|revoke)|library|downloads\/[A-Za-z0-9_-]{8,128}|games\/\d{1,20}\/(downloads|manifest)|artifacts\/[a-f0-9]{64})$/.test(path);
  }

  async request(request: SteamHostRequest): Promise<SteamHostResponse> {
    if (this.origin.length === 0 || this.publicKeyPin.length === 0 || !this.validRequestPath(request.path)) {
      return { status: 0, body: '' };
    }
    const requestTask = http.createHttp();
    try {
      const response = await requestTask.request(`${this.origin}${request.path}`, this.requestOptions(request));
      return {
        status: response.responseCode,
        body: typeof response.result === 'string' ? response.result : ''
      };
    } catch (_) {
      return { status: 0, body: '' };
    } finally {
      requestTask.destroy();
    }
  }

  async downloadArtifact(request: SteamHostArtifactDownloadRequest): Promise<SteamHostArtifactDownloadResult> {
    const artifactPrefix = `${this.origin}/v1/artifacts/`;
    if (this.origin.length === 0 || this.publicKeyPin.length === 0 ||
        !request.url.startsWith(artifactPrefix) ||
        !/^[a-f0-9]{64}$/.test(request.url.substring(artifactPrefix.length))) {
      return { status: 0, bytesWritten: 0 };
    }
    const parent = parentPath(request.destinationPath);
    if (!ensureDirectory(parent)) return { status: 0, bytesWritten: 0 };

    const output = fs.openSync(request.destinationPath,
      fs.OpenMode.CREATE | fs.OpenMode.WRITE_ONLY | fs.OpenMode.TRUNC);
    const requestTask = http.createHttp();
    let bytesWritten = 0;
    let writeFailed = false;
    const receiver = (data: ArrayBuffer): void => {
      if (writeFailed) return;
      try {
        const written = fs.writeSync(output.fd, data, { length: data.byteLength });
        if (written !== data.byteLength) {
          writeFailed = true;
          requestTask.destroy();
          return;
        }
        bytesWritten += written;
      } catch (_) {
        writeFailed = true;
        requestTask.destroy();
      }
    };
    try {
      requestTask.on('dataReceive', receiver);
      const status = await requestTask.requestInStream(request.url, {
        method: http.RequestMethod.GET,
        header: { 'authorization': `Bearer ${request.bearerToken}` },
        connectTimeout: 15000,
        readTimeout: 120000,
        usingCache: false,
        usingProxy: false,
        maxRedirects: 0,
        certificatePinning: [{ publicKeyHash: this.publicKeyPin, hashAlgorithm: 'SHA-256' }]
      });
      return { status: writeFailed ? 0 : status, bytesWritten: bytesWritten };
    } catch (_) {
      return { status: 0, bytesWritten: bytesWritten };
    } finally {
      requestTask.off('dataReceive', receiver);
      requestTask.destroy();
      fs.closeSync(output);
    }
  }
}

export class SteamHostService {
  private static instance: SteamHostService | null = null;

  static getInstance(): SteamHostService {
    if (SteamHostService.instance === null) SteamHostService.instance = new SteamHostService();
    return SteamHostService.instance;
  }

  private store: preferences.Preferences | null = null;
  private client: SteamHostClient | null = null;
  private origin: string = '';
  private publicKeyPin: string = '';
  private hostId: string = '';
  private pairingToken: string = '';
  private pendingClientNonce: string = '';

  init(context: common.Context): void {
    if (this.store !== null) return;
    try {
      this.store = preferences.getPreferencesSync(context, { name: STORE_NAME });
      const origin = this.store.getSync(STORE_ORIGIN, '') as string;
      const pin = this.store.getSync(STORE_PIN, '') as string;
      const hostId = this.store.getSync(STORE_HOST_ID, '') as string;
      const token = this.store.getSync(STORE_TOKEN, '') as string;
      if (validStoredHost(origin, pin, hostId, token)) {
        this.configurePairedHost(origin, pin, hostId, token);
      } else {
        this.clearMemoryPairing();
      }
    } catch (_) {
      this.store = null;
      this.clearMemoryPairing();
    }
  }

  pairingSummary(): SteamHostPairingSummary {
    return { paired: this.client !== null && this.client.hasPairingToken(), origin: this.origin, hostId: this.hostId };
  }

  async getCapabilities(): Promise<boolean> {
    const capabilities = this.client === null ? null : await this.client.getCapabilities();
    return capabilities !== null && capabilities.hostId === this.hostId && capabilities.libraryRead &&
      capabilities.managedDownloads && capabilities.artifactChecksums;
  }

  async beginPairing(origin: string, publicKeyPin: string): Promise<SteamHostPairChallenge | null> {
    const normalizedOrigin = normalizeSteamHostOrigin(origin);
    if (normalizedOrigin === null || !isSteamHostCertificatePin(publicKeyPin)) return null;
    const nonce = this.createClientNonce();
    if (nonce.length === 0) return null;
    const candidate = new SteamHostClient(new PinnedSteamHostTransport(normalizedOrigin, publicKeyPin), normalizedOrigin);
    const challenge = await candidate.beginPairing({
      clientName: 'WineHua', clientNonce: nonce, protocolVersion: STEAM_HOST_PROTOCOL_VERSION
    });
    if (challenge === null) return null;
    this.pendingClientNonce = nonce;
    return challenge;
  }

  async confirmPairing(origin: string, publicKeyPin: string, challengeId: string,
    verificationCode: string): Promise<boolean> {
    const normalizedOrigin = normalizeSteamHostOrigin(origin);
    if (normalizedOrigin === null || !isSteamHostCertificatePin(publicKeyPin) ||
        this.pendingClientNonce.length === 0) return false;
    const candidate = new SteamHostClient(new PinnedSteamHostTransport(normalizedOrigin, publicKeyPin), normalizedOrigin);
    const pairing = await candidate.confirmPairing({
      challengeId: challengeId,
      verificationCode: verificationCode,
      clientNonce: this.pendingClientNonce
    } as SteamHostPairConfirmRequest);
    this.pendingClientNonce = '';
    if (pairing === null) return false;
    candidate.setPairingToken(pairing.pairingToken);
    this.configurePairedHost(normalizedOrigin, publicKeyPin, pairing.hostId, pairing.pairingToken);
    return this.persistPairing();
  }

  async revokePairing(): Promise<boolean> {
    const hostRevoked = this.client === null ? false : await this.client.revokePairing();
    if (this.store !== null) {
      try {
        this.store.deleteSync(STORE_ORIGIN);
        this.store.deleteSync(STORE_PIN);
        this.store.deleteSync(STORE_HOST_ID);
        this.store.deleteSync(STORE_TOKEN);
        this.store.flush();
      } catch (_) {
      }
    }
    this.pendingClientNonce = '';
    this.clearMemoryPairing();
    return hostRevoked;
  }

  async getLibrary(): Promise<SteamHostLibraryEntry[] | null> {
    return this.client === null ? null : await this.client.getLibrary();
  }

  async requestDownload(appId: string): Promise<SteamHostDownloadJob | null> {
    return this.client === null ? null : await this.client.requestDownload(appId);
  }

  async getDownloadJob(appId: string, jobId: string): Promise<SteamHostDownloadJob | null> {
    return this.client === null ? null : await this.client.getDownloadJob(appId, jobId);
  }

  hasInstalledApp(appId: string): boolean {
    return this.readInstallationRecord(appId) !== null;
  }

  async installVerifiedManifest(appId: string): Promise<SteamHostInstallResult> {
    if (!isSteamAppId(appId) || this.client === null) return this.installFailure(appId, 'host is not paired');
    const manifest = await this.client.getManifest(appId);
    if (manifest === null) return this.installFailure(appId, 'host did not provide a valid manifest');
    return await this.installManifest(manifest);
  }

  async installManifest(manifest: SteamHostGameManifest): Promise<SteamHostInstallResult> {
    const appId = manifest.appId;
    if (this.client === null) return this.installFailure(appId, 'host is not paired');
    const validation = validateSteamHostManifest(manifest, this.client.getPairedOrigin());
    if (!validation.valid) return this.installFailure(appId, validation.error);
    if (!manifest.launchReady) return this.installFailure(appId, 'host marked this title as not launch-ready');
    const downloader = this.client.artifactTransport();
    if (downloader === null) return this.installFailure(appId, 'host transport cannot stream artifacts');
    const driveC = wineDriveCForContainer(manifest.containerId);
    const installRoot = `${driveC}/${manifest.installRelativePath}`;
    const stagingRoot = `${installRoot}/.staging/${manifest.revision}`;
    const releaseRoot = `${installRoot}/releases/${manifest.revision}`;
    const metadataPath = `${driveC}/games/.winehua-installs/${appId}.json`;
    if (pathExists(releaseRoot)) {
      const existing = this.readInstallationRecord(appId);
      if (existing !== null && existing.revision === manifest.revision &&
          existing.containerId === manifest.containerId) {
        return this.installSuccess(existing);
      }
      return this.installFailure(appId, 'manifest revision already exists');
    }

    // The prefix must be created by Wine before files are placed under drive_c.
    // Merely creating that directory would leave a half-initialized container.
    if (!await WineEnvService.getInstance().ensureContainerReady(manifest.containerId)) {
      return this.installFailure(appId, 'unable to initialize the game container');
    }

    removeIfPresent(stagingRoot);
    if (!ensureDirectory(stagingRoot)) return this.installFailure(appId, 'unable to create staging directory');
    try {
      for (const artifact of manifest.artifacts) {
        const destination = `${stagingRoot}/${artifact.relativePath}`;
        const temporary = `${destination}.part`;
        if (!ensureDirectory(parentPath(destination))) throw new Error('unable to create artifact directory');
        removeIfPresent(temporary);
        const response = await downloader.downloadArtifact({
          url: artifact.downloadUrl,
          destinationPath: temporary,
          bearerToken: this.client.pairingTokenForArtifactDownload()
        });
        if (response.status !== 200 || response.bytesWritten !== artifact.sizeBytes || !pathExists(temporary)) {
          throw new Error('artifact transfer failed');
        }
        const stat = fs.statSync(temporary);
        if (stat.size !== artifact.sizeBytes) throw new Error('artifact length mismatch');
        const actualHash = (await hash.hash(temporary, 'sha256')).toLowerCase();
        if (actualHash !== artifact.sha256) throw new Error('artifact checksum mismatch');
        fs.renameSync(temporary, destination);
      }

      if (pathExists(releaseRoot)) throw new Error('manifest revision already exists');
      if (!ensureDirectory(parentPath(releaseRoot))) throw new Error('unable to create release directory');
      fs.renameSync(stagingRoot, releaseRoot);
      const record: LocalInstallationRecord = {
        schemaVersion: 1,
        appId: manifest.appId,
        containerId: manifest.containerId,
        revision: manifest.revision,
        launchExePath: windowsPath(manifest.installRelativePath, manifest.revision, manifest.launchExeRelativePath),
        workingDirectory: workingDirectoryPath(manifest)
      };
      writeTextAtomically(metadataPath, JSON.stringify(record));
      return {
        installed: true,
        error: '',
        appId: manifest.appId,
        containerId: manifest.containerId,
        revision: manifest.revision,
        windowsExePath: record.launchExePath,
        windowsWorkingDirectory: record.workingDirectory
      };
    } catch (_) {
      removeIfPresent(stagingRoot);
      return this.installFailure(appId, 'artifact download or verification failed');
    }
  }

  // A launch is permitted only from a locally committed, verified revision.
  // It never asks the Host to start Steam or to proxy a Windows Steam process.
  async launchInstalledApp(appId: string): Promise<SteamHostLaunchResult> {
    const record = this.readInstallationRecord(appId);
    if (record === null) return this.launchFailure(appId, 'no verified local installation');

    const env = WineEnvService.getInstance();
    if (!await env.ensureContainerReady(record.containerId)) {
      return this.launchFailure(appId, 'unable to initialize the game container');
    }
    const driveC = wineDriveCForContainer(record.containerId);
    const localExePath = localDriveCPath(driveC, record.launchExePath);
    const localWorkingDirectory = localDriveCPath(driveC, record.workingDirectory);
    if (!pathExists(localExePath) || !pathExists(localWorkingDirectory)) {
      return this.launchFailure(appId, 'verified revision files are no longer available');
    }
    if (!await env.ensureDesktop()) {
      return this.launchFailure(appId, 'Wine desktop is not ready');
    }

    try {
      const process = testNapi.runWineProgram({
        windowsExePath: record.launchExePath,
        containerId: record.containerId,
        argv: [],
        environment: {},
        workingDirectory: record.workingDirectory,
        d3dBackend: env.getD3dBackend(),
        dxvkBackend: env.getDxvkBackend()
      }) as Record<string, Object>;
      const pid = Number(process['pid'] ?? 0);
      if (!(pid > 0)) return this.launchFailure(appId, 'Wine did not start the verified executable');
      setTimeout(() => WineWindowManager.getInstance().restoreDesktopIfRunning(), 250);
      return {
        launched: true,
        error: '',
        appId: record.appId,
        containerId: record.containerId,
        revision: record.revision,
        windowsExePath: record.launchExePath,
        windowsWorkingDirectory: record.workingDirectory,
        pid: pid
      };
    } catch (_) {
      return this.launchFailure(appId, 'Wine launch failed');
    }
  }

  async installAndLaunch(appId: string): Promise<SteamHostLaunchResult> {
    const installed = await this.installVerifiedManifest(appId);
    if (!installed.installed) return this.launchFailure(appId, installed.error);
    return await this.launchInstalledApp(appId);
  }

  private configurePairedHost(origin: string, pin: string, hostId: string, token: string): void {
    this.origin = normalizeSteamHostOrigin(origin) ?? '';
    this.publicKeyPin = pin;
    this.hostId = hostId;
    this.pairingToken = token;
    if (!validStoredHost(this.origin, pin, hostId, token)) {
      this.clearMemoryPairing();
      return;
    }
    this.client = new SteamHostClient(new PinnedSteamHostTransport(this.origin, this.publicKeyPin), this.origin);
    this.client.setPairingToken(this.pairingToken);
  }

  private clearMemoryPairing(): void {
    this.client = null;
    this.origin = '';
    this.publicKeyPin = '';
    this.hostId = '';
    this.pairingToken = '';
  }

  private persistPairing(): boolean {
    if (this.store === null || !validStoredHost(this.origin, this.publicKeyPin, this.hostId, this.pairingToken)) {
      return false;
    }
    try {
      this.store.putSync(STORE_ORIGIN, this.origin);
      this.store.putSync(STORE_PIN, this.publicKeyPin);
      this.store.putSync(STORE_HOST_ID, this.hostId);
      this.store.putSync(STORE_TOKEN, this.pairingToken);
      this.store.flush();
      return true;
    } catch (_) {
      this.clearMemoryPairing();
      return false;
    }
  }

  private createClientNonce(): string {
    try {
      const bytes = cryptoFramework.createRandom().generateRandomSync(24).data;
      let text = '';
      for (let index = 0; index < bytes.length; ++index) {
        text += bytes[index].toString(16).padStart(2, '0');
      }
      return text;
    } catch (_) {
      return '';
    }
  }

  private installFailure(appId: string, error: string): SteamHostInstallResult {
    return {
      installed: false,
      error: error,
      appId: appId,
      containerId: '',
      revision: '',
      windowsExePath: '',
      windowsWorkingDirectory: ''
    };
  }

  private installSuccess(record: LocalInstallationRecord): SteamHostInstallResult {
    return {
      installed: true,
      error: '',
      appId: record.appId,
      containerId: record.containerId,
      revision: record.revision,
      windowsExePath: record.launchExePath,
      windowsWorkingDirectory: record.workingDirectory
    };
  }

  private readInstallationRecord(appId: string): LocalInstallationRecord | null {
    if (!isSteamAppId(appId)) return null;
    const expectedContainer = steamContainerIdForApp(appId);
    const metadataPath = `${wineDriveCForContainer(expectedContainer)}/games/.winehua-installs/${appId}.json`;
    if (!pathExists(metadataPath)) return null;
    try {
      const record = JSON.parse(readTextFile(metadataPath)) as LocalInstallationRecord;
      const expectedInstall = steamInstallRelativePathForApp(appId).replace(/\//g, '\\');
      const expectedPrefix = `C:\\${expectedInstall}\\releases\\`;
      if (record.schemaVersion !== 1 || record.appId !== appId || record.containerId !== expectedContainer ||
          !/^[A-Za-z0-9._-]{1,128}$/.test(record.revision) ||
          !record.launchExePath.startsWith(`${expectedPrefix}${record.revision}\\`) ||
          !(record.workingDirectory === `${expectedPrefix}${record.revision}` ||
            record.workingDirectory.startsWith(`${expectedPrefix}${record.revision}\\`))) {
        return null;
      }
      return record;
    } catch (_) {
      return null;
    }
  }

  private launchFailure(appId: string, error: string): SteamHostLaunchResult {
    return {
      launched: false,
      error: error,
      appId: appId,
      containerId: '',
      revision: '',
      windowsExePath: '',
      windowsWorkingDirectory: '',
      pid: 0
    };
  }
}
