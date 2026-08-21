import { fileIo as fs } from '@kit.CoreFileKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import testNapi from 'libentry.so';

const DOMAIN = 0x0000;
const FILES_BASE = '/data/storage/el2/base/files';
const WINE_ROOT = `${FILES_BASE}/wine`;
const GUEST_VULKAN_ROOT = `${WINE_ROOT}/bin/guest_vulkan`;
const HOST_VULKAN_ROOT = `${WINE_ROOT}/bin/host_vulkan`;
const AUTOMATION_RESULTS = `${FILES_BASE}/automation/results`;
const CACHE_BASE = '/data/storage/el2/base/cache';
const TEMP_BASE = '/data/storage/el2/base/temp';

export interface SmokeLaunchRequest {
  mode: string;
  runId: string;
  suite: string;
  prefixMode: string;
  longSeconds: number;
  batchMappedFlush: boolean;
  experimentId: string;
  experimentExecutable: string;
  experimentResultName: string;
  experimentSourceUrl: string;
}

interface SmokeTestSpec {
  id: string;
  architecture: string;
  executable: string;
  seconds: number;
  timeoutMs: number;
}

interface ProcessInfo {
  found: boolean;
  pid: number;
  status: string;
  startTimestamp: number;
  endTimestamp: number;
  exitCode: number | null;
  exitCodeSource: string;
}

interface SmokeResult {
  status: string;
  stage: string;
  message: string;
  heartbeatTimestampMs: number;
}

interface SmokeSuiteTestResult {
  schemaVersion?: number;
  runId?: string;
  testId?: string;
  status: string;
  stage: string;
  message: string;
  pid?: number;
  exitCode?: number;
  exitCodeSource?: string;
  heartbeatTimestampMs?: number;
}

interface SmokeSuiteSummary {
  schemaVersion: number;
  runId: string;
  suite: string;
  prefixMode: string;
  status: string;
  stage?: string;
  message?: string;
  completedTimestampMs?: number;
  tests: SmokeSuiteTestResult[];
}

function pathExists(path: string): boolean {
  try {
    return fs.accessSync(path);
  } catch (_) {
    return false;
  }
}

function ensureDirectory(path: string): void {
  if (!pathExists(path)) fs.mkdirSync(path, true);
}

function removeRecursive(path: string): void {
  if (!pathExists(path)) return;
  const stat = fs.lstatSync(path);
  if (stat.isSymbolicLink() || !stat.isDirectory()) {
    fs.unlinkSync(path);
    return;
  }
  for (const name of fs.listFileSync(path)) removeRecursive(`${path}/${name}`);
  fs.rmdirSync(path);
}

function copyRecursive(source: string, target: string): void {
  const stat = fs.statSync(source);
  if (!stat.isDirectory()) {
    const slash = target.lastIndexOf('/');
    if (slash > 0) ensureDirectory(target.substring(0, slash));
    fs.copyFileSync(source, target);
    return;
  }
  ensureDirectory(target);
  for (const name of fs.listFileSync(source)) copyRecursive(`${source}/${name}`, `${target}/${name}`);
}

function readText(path: string): string {
  try {
    return fs.readTextSync(path, { encoding: 'utf-8' }).trim();
  } catch (_) {
    return '';
  }
}

// guest Vulkan 栈架构从 manifest.json 的 architecture 字段判断 (aarch64|x86_64),
// arm64 原生 wine → aarch64, x86_64 → x86_64。无 manifest 时默认 x86_64 (旧行为)。
interface GuestVulkanManifest {
  architecture?: string;
}

function guestVulkanArch(): string {
  const manifestPath = `${GUEST_VULKAN_ROOT}/manifest.json`;
  if (pathExists(manifestPath)) {
    const architecture = readText(manifestPath);
    if (architecture.length > 0) {
      try {
        const manifest = JSON.parse(architecture) as GuestVulkanManifest;
        const arch: string = manifest.architecture ?? '';
        if (arch.startsWith('aarch64')) return 'aarch64';
        if (arch.startsWith('x86_64')) return 'x86_64';
      } catch (_) { /* fall through to default */ }
    }
  }
  return 'x86_64';
}

function writeAtomic(path: string, text: string): void {
  const slash = path.lastIndexOf('/');
  if (slash > 0) ensureDirectory(path.substring(0, slash));
  const temporary = `${path}.tmp.${Date.now()}`;
  const file = fs.openSync(temporary, fs.OpenMode.CREATE | fs.OpenMode.WRITE_ONLY | fs.OpenMode.TRUNC);
  try {
    fs.writeSync(file.fd, text);
  } finally {
    fs.closeSync(file);
  }
  if (pathExists(path)) fs.unlinkSync(path);
  fs.renameSync(temporary, path);
}

function resetAutomationLogs(): void {
  const now = new Date();
  const twoDigits = (value: number): string => value < 10 ? `0${value}` : `${value}`;
  const day = `${now.getFullYear()}${twoDigits(now.getMonth() + 1)}${twoDigits(now.getDate())}`;
  const paths = [
    `${TEMP_BASE}/wine_stderr_${day}.log`,
    `${TEMP_BASE}/winehua_vtest_frontbuffer.log`,
    `${CACHE_BASE}/winehua_virgl_host.log`
  ];
  for (const path of paths) {
    try {
      const file = fs.openSync(path, fs.OpenMode.CREATE | fs.OpenMode.WRITE_ONLY | fs.OpenMode.TRUNC);
      fs.closeSync(file);
    } catch (error) {
      hilog.warn(DOMAIN, 'SmokeRunner', 'log reset failed path=%{public}s error=%{public}s',
        path, JSON.stringify(error));
    }
  }
}

function safeId(value: string, fallback: string): string {
  const normalized = value.replace(/[^A-Za-z0-9_.-]/g, '_');
  return normalized.length > 0 ? normalized.substring(0, 80) : fallback;
}

function prefixRoot(prefixMode: string): string {
  return prefixMode === 'clean' ? `${FILES_BASE}/.wine-smoke` : `${FILES_BASE}/.wine`;
}

function hasReplayShader(directory: string): boolean {
  try {
    return fs.listFileSync(directory).some((name: string) =>
      name.indexOf('CS_') === 0 && name.indexOf('.remapped.spv') >= 0);
  } catch (_) {
    return false;
  }
}

/* A DXVK run writes its SPIR-V dumps under C:\smoke\results\<run-id>.
 * Replay is a separate process/suite so Venus can start from a fresh render
 * server after DXVK teardown; select the newest completed dump directory. */
function latestReplayShaderDirectory(prefix: string, current: string): string {
  const root = `${prefix}/drive_c/smoke/results`;
  try {
    const names = fs.listFileSync(root).sort().reverse();
    for (const name of names) {
      const directory = `${root}/${name}`;
      if (directory === current || !pathExists(directory)) continue;
      if (hasReplayShader(directory)) return directory;
    }
  } catch (_) {}
  return current;
}

export function defaultSmokeRequest(): SmokeLaunchRequest {
  return { mode: '', runId: '', suite: 'core', prefixMode: 'reuse', longSeconds: 3600,
    batchMappedFlush: false, experimentId: '', experimentExecutable: '', experimentResultName: '',
    experimentSourceUrl: '' };
}

export function parseSmokeRequest(text: string): SmokeLaunchRequest {
  const fallback = defaultSmokeRequest();
  if (!text) return fallback;
  try {
    const value = JSON.parse(text) as SmokeLaunchRequest;
    return {
      mode: value.mode === 'smoke' || value.mode === 'game' || value.mode === 'experiment' ? value.mode : '',
      runId: safeId(value.runId, `${Date.now()}`),
      suite: safeId(value.suite, 'core').toLowerCase(),
      prefixMode: value.prefixMode === 'clean' ? 'clean' : 'reuse',
      longSeconds: value.longSeconds >= 60 && value.longSeconds <= 3600 ?
        Math.floor(value.longSeconds) : 3600,
      batchMappedFlush: value.batchMappedFlush === true,
      experimentId: safeId(value.experimentId || '', ''),
      experimentExecutable: safeId(value.experimentExecutable || '', ''),
      experimentResultName: safeId(value.experimentResultName || '', ''),
      experimentSourceUrl: typeof value.experimentSourceUrl === 'string' ?
        value.experimentSourceUrl.substring(0, 480) : ''
    };
  } catch (_) {
    return fallback;
  }
}

export function syncManagedSmoke(prefixMode: string): boolean {
  const source = `${WINE_ROOT}/smoke`;
  const target = `${prefixRoot(prefixMode)}/drive_c/smoke`;
  const sourceManifest = readText(`${source}/manifest.json`);
  if (!sourceManifest) {
    hilog.error(DOMAIN, 'SmokeRunner', 'managed smoke manifest missing: %{public}s', source);
    return false;
  }
  /* A previous interrupted extraction can leave a matching manifest but a
   * partially populated C:\smoke tree.  Validate the managed entry points
   * before taking the fast path; DXVK x64 otherwise fails as a misleading
   * "d3d11.dll not found" while the x86 slot happens to exist. */
  const required = [
    'x64/winehua_audio_smoke.exe', 'x64/winehua_graphics_smoke.exe',
    'x64/winehua_vulkan_smoke.exe', 'x64/winehua_d3d11_smoke.exe',
    'x64/winehua_d3d8_smoke.exe', 'x64/winehua_d3d_switch_cube.exe',
    'x64/winehua_gpu_diagnostics.exe',
    'x64/winehua_win32_driver.exe', 'x64/winehua_dxvk26_requirements.exe',
    'x64/winehua_d3d12_smoke.exe',
    'x64/triangle.exe', 'x64/gears.exe',
    'x86/winehua_audio_smoke.exe', 'x86/winehua_graphics_smoke.exe',
    'x86/winehua_vulkan_smoke.exe', 'x86/winehua_d3d11_smoke.exe',
    'x86/winehua_d3d8_smoke.exe', 'x86/winehua_d3d_switch_cube.exe',
    'x86/winehua_gpu_diagnostics.exe',
    'x86/winehua_win32_driver.exe', 'x86/winehua_dxvk26_requirements.exe',
    'assets/venus_storage_write.spv', 'assets/venus_storage_read.spv',
    'assets/venus_image_fetch.spv', 'assets/venus_combined_sample.spv',
    'assets/venus_separated_sample.spv',
  ];
  /* venus shader assets 随 guest_vulkan 打包，仅 x86_64 有内容 (arm64 原生走
   * 宿主 Vulkan, files/wine/smoke/assets 为空目录)。这部分按"源目录实际内容"
   * 同步，不在此硬性要求——arm64 下缺失这些 .spv 不得让 C:\smoke 同步失败。 */
  /* DXVK is a WineHua runtime overlay, not part of C:\\smoke.  Remove the
   * four files from older payloads, but only inside this managed directory. */
  for (const file of ['x64/d3d11.dll', 'x64/dxgi.dll', 'x86/d3d11.dll', 'x86/dxgi.dll']) {
    try {
      const stale = `${target}/${file}`;
      if (pathExists(stale)) fs.unlinkSync(stale);
    } catch (error) {
      hilog.warn(DOMAIN, 'SmokeRunner', 'stale managed DXVK cleanup failed path=%{public}s error=%{public}s',
        `${target}/${file}`, JSON.stringify(error));
    }
  }
  /* Remove the old payload layout from prefixes upgraded before DXVK became a
   * runtime component.  This never touches user-installed game files. */
  const staleDxvk = target + '/dxvk';
  if (pathExists(staleDxvk)) removeRecursive(staleDxvk);
  let complete = sourceManifest === readText(`${target}/manifest.json`);
  for (const file of required) {
    if (!pathExists(`${target}/${file}`)) {
      complete = false;
      break;
    }
  }
  /* venus shader assets 是 x86_64 guest_vulkan 专属 (arm64 空目录): 仅当源
   * assets 有内容时才要求 target 同步其全部文件。 */
  if (complete && pathExists(`${source}/assets`)) {
    for (const name of fs.listFileSync(`${source}/assets`)) {
      if (!pathExists(`${target}/assets/${name}`)) {
        complete = false;
        break;
      }
    }
  }
  if (complete) return true;

  try {
    removeRecursive(target);
    ensureDirectory(target);
    /* Keep the runtime-owned dxvk/ directory out of the prefix. */
    copyRecursive(`${source}/manifest.json`, `${target}/manifest.json`);
    for (const file of required) copyRecursive(`${source}/${file}`, `${target}/${file}`);
    if (pathExists(`${source}/assets`)) copyRecursive(`${source}/assets`, `${target}/assets`);
    ensureDirectory(`${target}/results`);
    hilog.info(DOMAIN, 'SmokeRunner', 'managed smoke synchronized prefix=%{public}s', prefixMode);
    return sourceManifest === readText(`${target}/manifest.json`);
  } catch (error) {
    hilog.error(DOMAIN, 'SmokeRunner', 'managed smoke sync failed: %{public}s', JSON.stringify(error));
    return false;
  }
}

export function stageManagedVkd3dRuntime(prefixMode: string): boolean {
  // The standalone VKD3D button is intentionally restricted to the app-owned
  // clean prefix. LoadLibrary("d3d12.dll") searches beside the PE, while the
  // Wine loader's WINEDLLPATH is not part of that Win32 file search path.
  if (prefixMode !== 'clean') {
    hilog.error(DOMAIN, 'VKD3D', 'refusing to stage VKD3D runtime outside clean prefix');
    return false;
  }
  const source = `${WINE_ROOT}/vkd3d/limited-500k/x64/d3d12.dll`;
  const target = `${prefixRoot(prefixMode)}/drive_c/smoke/x64/d3d12.dll`;
  try {
    const sourceStat = fs.statSync(source);
    if (sourceStat.isDirectory() || sourceStat.size <= 0) return false;
    if (pathExists(target)) fs.unlinkSync(target);
    fs.copyFileSync(source, target);
    const targetStat = fs.statSync(target);
    if (targetStat.isDirectory() || targetStat.size !== sourceStat.size) {
      fs.unlinkSync(target);
      return false;
    }
    hilog.info(DOMAIN, 'VKD3D',
      'managed runtime staged prefix=clean file=d3d12.dll bytes=%{public}d', targetStat.size);
    return true;
  } catch (error) {
    try {
      if (pathExists(target)) fs.unlinkSync(target);
    } catch (_) {}
    hilog.error(DOMAIN, 'VKD3D', 'managed runtime staging failed: %{public}s', JSON.stringify(error));
    return false;
  }
}

/* Validate the versioned DXVK payload without injecting it into the prefix's
 * system32/syswow64 directories.  DXVK DLLs belong to the selected WineHua runtime
 * profile; the Wine loader supplies the normal
 * system/builtin fallback for every other process. */
function validateManagedDxvk(profile: string): boolean {
  const source = `${WINE_ROOT}/dxvk/${profile}`;
  const manifest = WINE_ROOT + "/dxvk/manifest.json";
  if (!pathExists(manifest)) return false;
  const required = [
    `${source}/x64/d3d11.dll`, `${source}/x64/dxgi.dll`,
    `${source}/x86/d3d11.dll`, `${source}/x86/dxgi.dll`
  ];
  try {
    for (const file of required) if (!pathExists(file)) return false;
    return true;
  } catch (error) {
    hilog.error(DOMAIN, 'SmokeRunner', 'managed DXVK payload validation failed: %{public}s', JSON.stringify(error));
    return false;
  }
}

function sleep(milliseconds: number): Promise<void> {
  return new Promise<void>((resolve) => setTimeout(resolve, milliseconds));
}

export class SmokeRunner {
  private request: SmokeLaunchRequest;
  private onStatus: (text: string) => void;
  private perfProfile: string;
  private prefix: string;
  private resultDirectory: string;

  constructor(request: SmokeLaunchRequest, onStatus: (text: string) => void,
              perfProfile: string = 'baseline') {
    this.request = request;
    this.onStatus = onStatus;
    this.perfProfile = perfProfile;
    this.prefix = prefixRoot(request.prefixMode);
    this.resultDirectory = `${this.prefix}/drive_c/smoke/results/${request.runId}`;
  }

  private specs(): SmokeTestSpec[] {
    const tests: SmokeTestSpec[] = [];
    const includeAudio = this.request.suite === 'core' || this.request.suite === 'audio' ||
      this.request.suite === 'all' || this.request.suite === 'long';
    const includeOpenGl = this.request.suite === 'core' || this.request.suite === 'opengl' ||
      this.request.suite === 'all' || this.request.suite === 'long';
    const includeD3d8 = this.request.suite === 'd3d8' || this.request.suite === 'all';
    const includeD3d9 = this.request.suite === 'd3d9' || this.request.suite === 'all';
    const includeVenus = this.request.suite === 'venus' || this.request.suite === 'all';
    const includeVenusSampled = this.request.suite === 'venus-sampled' || this.request.suite === 'all';
    /* queue-idle is a separate diagnostic.  Current Venus/vtest builds can
     * return VK_SUCCESS from queue/device idle before the mapped write is
     * observable; it must not poison the sampled-image qualification suite. */
    const includeVenusSampledIdle = this.request.suite === 'venus-sampled-idle';
    const includeVenusDepthCube = this.request.suite === 'venus-depth-cube';
    const includeVenusDepthCubeArray2D =
      this.request.suite === 'venus-depth-cube-array-2d-golden';
    const includeVenusDepthCubeGraphics = this.request.suite === 'venus-depth-cube-graphics';
    const includeVenusHeavenMaterial = this.request.suite === 'venus-heaven-material';
    const includeVenusHeavenMaterialDepth =
      this.request.suite === 'venus-heaven-material-depth';
    const includeVenusHeavenCaptured = this.request.suite === 'venus-heaven-captured';
    const includeVenusHeavenDraw0 = this.request.suite === 'venus-heaven-draw0';
    const includeVenusHeavenDraw170 = this.request.suite === 'venus-heaven-draw170';
    const includeVenusHeavenF647 = this.request.suite === 'venus-heaven-f647';
    const includeVenusHeavenInputs = this.request.suite === 'venus-heaven-inputs';
    const includeVenusHeavenCapturedAb = this.request.suite === 'venus-heaven-captured-ab';
    const includeVenusHeavenDiscardAb = this.request.suite === 'venus-heaven-discard-ab';
    const includeVenusHeavenMaterialLayout =
      this.request.suite === 'venus-heaven-material-layout';
    const includeHostHeaven = this.request.suite === 'host-heaven';
    const includeHostHeavenMaterialDepth =
      this.request.suite === 'host-heaven-material-depth';
    const includeHostHeavenInputs = this.request.suite === 'host-heaven-inputs';
    const includeWineVulkan = this.request.suite === 'wine-vulkan' ||
      this.request.suite === 'all';
    const includeWineVulkanPresent = this.request.suite === 'wine-vulkan-present' ||
      this.request.suite === 'all';
    const includeDxvk = this.request.suite === 'dxvk' || this.request.suite === 'all';
    const includeGpuDiagnostics = this.request.suite === 'gpu-diagnostics';
    const includeDxvkModernBaseline = this.request.suite === 'dxvk-modern-baseline';
    const includeDxvk26Requirements = this.request.suite === 'dxvk26-requirements';
    /* Gate A never loads vkd3d. These probes only observe the same Vulkan
     * layers a future D3D12 runtime would use. */
    const includeVkd3dCapability = this.request.suite === 'vkd3d-capability';
    const includeDxvkLong = this.request.suite === 'dxvk-long';
    const includeDxvkModernLong = this.request.suite === 'dxvk-modern-long';
    const includeDxvkReplay = this.request.suite === 'dxvk-replay';
      const includeDxvkLayoutGeneral = this.request.suite === 'dxvk-layout-general';
      const includeDxvkCombined = this.request.suite === 'dxvk-combined';
    const includeDxvkDynamic = this.request.suite === 'dxvk-dynamic';
    if (includeAudio) {
      tests.push({ id: 'audio-x64', architecture: 'x64', executable: 'winehua_audio_smoke.exe',
        seconds: 3, timeoutMs: 45000 });
      tests.push({ id: 'audio-x86', architecture: 'x86', executable: 'winehua_audio_smoke.exe',
        seconds: 3, timeoutMs: 45000 });
    }
    if (includeOpenGl) {
      const seconds = this.request.suite === 'long' ? 3600 : 8;
      const timeout = this.request.suite === 'long' ? 3660000 : 60000;
      tests.push({ id: 'opengl-x64', architecture: 'x64', executable: 'winehua_graphics_smoke.exe',
        seconds: seconds, timeoutMs: timeout });
      tests.push({ id: 'opengl-x86', architecture: 'x86', executable: 'winehua_graphics_smoke.exe',
        seconds: seconds, timeoutMs: timeout });
    }
    if (includeD3d8) {
      /* Most D3D8 titles are 32-bit. Run WoW64 first, then verify the native
       * x64 frontend uses the same virtual mode and WineD3D contracts. */
      tests.push({ id: 'd3d8-capability-x86', architecture: 'x86',
        executable: 'winehua_d3d8_smoke.exe', seconds: 5, timeoutMs: 180000 });
      tests.push({ id: 'd3d8-capability-x64', architecture: 'x64',
        executable: 'winehua_d3d8_smoke.exe', seconds: 5, timeoutMs: 180000 });
    }
    if (includeD3d9) {
      tests.push({ id: 'd3d9-cube-x86', architecture: 'x86',
        executable: 'winehua_d3d_switch_cube.exe', seconds: 8, timeoutMs: 180000 });
      tests.push({ id: 'd3d9-cube-x64', architecture: 'x64',
        executable: 'winehua_d3d_switch_cube.exe', seconds: 8, timeoutMs: 180000 });
    }
    if (includeVenus) {
      tests.push({ id: 'venus-offscreen-x64', architecture: 'guest-x64',
        executable: 'libwinehua_guest_vulkan_smoke.so', seconds: 0, timeoutMs: 60000 });
    }
    if (includeVenusSampled) {
      tests.push({ id: 'venus-sampled-normal-x64', architecture: 'guest-x64',
        executable: 'libvenus_sampled_image_probe.so', seconds: 0, timeoutMs: 90000 });
      tests.push({ id: 'venus-sampled-general-x64', architecture: 'guest-x64',
        executable: 'libvenus_sampled_image_probe.so', seconds: 0, timeoutMs: 90000 });
      tests.push({ id: 'venus-sampled-only-x64', architecture: 'guest-x64',
        executable: 'libvenus_sampled_image_probe.so', seconds: 0, timeoutMs: 90000 });
    }
    if (includeVenusSampledIdle) {
      tests.push({ id: 'venus-sampled-idle-x64', architecture: 'guest-x64',
        executable: 'libvenus_sampled_image_probe.so', seconds: 0, timeoutMs: 90000 });
    }
    if (includeVenusDepthCube) {
      tests.push({ id: 'venus-depth-cube-x64', architecture: 'guest-x64',
        executable: 'libvenus_sampled_image_probe.so', seconds: 0, timeoutMs: 60000 });
      tests.push({ id: 'venus-depth-cube-array-2d-golden-x64', architecture: 'guest-x64',
        executable: 'libvenus_sampled_image_probe.so', seconds: 0, timeoutMs: 60000 });
      tests.push({ id: 'venus-depth-cube-array-x64', architecture: 'guest-x64',
        executable: 'libvenus_sampled_image_probe.so', seconds: 0, timeoutMs: 60000 });
    }
    if (includeVenusDepthCubeArray2D) {
      tests.push({ id: 'venus-depth-cube-array-2d-golden-x64', architecture: 'guest-x64',
        executable: 'libvenus_sampled_image_probe.so', seconds: 0, timeoutMs: 60000 });
    }
    if (includeVenusDepthCubeGraphics) {
      tests.push({ id: 'venus-depth-cube-graphics-x64', architecture: 'guest-x64',
        executable: 'libvenus_sampled_image_probe.so', seconds: 0, timeoutMs: 90000 });
      tests.push({ id: 'venus-depth-cube-graphics-golden-x64', architecture: 'guest-x64',
        executable: 'libvenus_sampled_image_probe.so', seconds: 0, timeoutMs: 90000 });
      tests.push({ id: 'venus-depth-cube-graphics-golden-dxvk-contract-x64', architecture: 'guest-x64',
        executable: 'libvenus_sampled_image_probe.so', seconds: 0, timeoutMs: 90000 });
      tests.push({ id: 'venus-depth-cube-graphics-initialized-x64', architecture: 'guest-x64',
        executable: 'libvenus_sampled_image_probe.so', seconds: 0, timeoutMs: 90000 });
      tests.push({ id: 'venus-depth-cube-graphics-optimized-x64', architecture: 'guest-x64',
        executable: 'libvenus_sampled_image_probe.so', seconds: 0, timeoutMs: 90000 });
      tests.push({ id: 'venus-depth-cube-graphics-coordinate-trace-x64', architecture: 'guest-x64',
        executable: 'libvenus_sampled_image_probe.so', seconds: 0, timeoutMs: 90000 });
      tests.push({ id: 'venus-depth-cube-graphics-padded-dref-x64', architecture: 'guest-x64',
        executable: 'libvenus_sampled_image_probe.so', seconds: 0, timeoutMs: 90000 });
      tests.push({ id: 'venus-depth-cube-graphics-no-float-controls-x64', architecture: 'guest-x64',
        executable: 'libvenus_sampled_image_probe.so', seconds: 0, timeoutMs: 90000 });
    }
    if (includeVenusHeavenMaterial) {
      tests.push({ id: 'venus-heaven-material-graphics-x64', architecture: 'guest-x64',
        executable: 'libvenus_heaven_material_replay.so', seconds: 0, timeoutMs: 120000 });
      tests.push({ id: 'venus-heaven-material-exact-x64', architecture: 'guest-x64',
        executable: 'libvenus_heaven_material_replay.so', seconds: 0, timeoutMs: 120000 });
      tests.push({ id: 'venus-heaven-material-exact-fragment-x64', architecture: 'guest-x64',
        executable: 'libvenus_heaven_material_replay.so', seconds: 0, timeoutMs: 120000 });
      tests.push({ id: 'venus-heaven-material-exact-probe-x64', architecture: 'guest-x64',
        executable: 'libvenus_heaven_material_replay.so', seconds: 0, timeoutMs: 120000 });
      tests.push({ id: 'venus-heaven-material-no-float-x64', architecture: 'guest-x64',
        executable: 'libvenus_heaven_material_replay.so', seconds: 0, timeoutMs: 120000 });
      tests.push({ id: 'venus-heaven-material-constant-x64', architecture: 'guest-x64',
        executable: 'libvenus_heaven_material_replay.so', seconds: 0, timeoutMs: 120000 });
      tests.push({ id: 'venus-heaven-material-contract-simple-x64', architecture: 'guest-x64',
        executable: 'libvenus_heaven_material_replay.so', seconds: 0, timeoutMs: 120000 });
      tests.push({ id: 'venus-heaven-material-constant-final-vertex-x64', architecture: 'guest-x64',
        executable: 'libvenus_heaven_material_replay.so', seconds: 0, timeoutMs: 120000 });
      tests.push({ id: 'venus-heaven-material-final-fragment-simple-vertex-x64', architecture: 'guest-x64',
        executable: 'libvenus_heaven_material_replay.so', seconds: 0, timeoutMs: 120000 });
      tests.push({ id: 'venus-heaven-material-bool-true-x64', architecture: 'guest-x64',
        executable: 'libvenus_heaven_material_replay.so', seconds: 0, timeoutMs: 120000 });
      tests.push({ id: 'venus-heaven-material-bool-false-x64', architecture: 'guest-x64',
        executable: 'libvenus_heaven_material_replay.so', seconds: 0, timeoutMs: 120000 });
    }
    if (includeVenusHeavenMaterialDepth) {
      tests.push({ id: 'venus-heaven-material-depth-pass0-x64', architecture: 'guest-x64',
        executable: 'libvenus_heaven_material_replay.so', seconds: 0, timeoutMs: 120000 });
      tests.push({ id: 'venus-heaven-material-depth-pass3-x64', architecture: 'guest-x64',
        executable: 'libvenus_heaven_material_replay.so', seconds: 0, timeoutMs: 120000 });
      tests.push({ id: 'venus-heaven-material-depth-pass0-no-float-controls-x64',
        architecture: 'guest-x64', executable: 'libvenus_heaven_material_replay.so',
        seconds: 0, timeoutMs: 120000 });
      tests.push({ id: 'venus-heaven-material-depth-pass3-no-float-controls-x64',
        architecture: 'guest-x64', executable: 'libvenus_heaven_material_replay.so',
        seconds: 0, timeoutMs: 120000 });
    }
    if (includeVenusHeavenCaptured) {
      tests.push({ id: 'venus-heaven-material-captured-x64', architecture: 'guest-x64',
        executable: 'libvenus_heaven_material_replay.so', seconds: 0, timeoutMs: 120000 });
      tests.push({ id: 'venus-heaven-material-captured-nodepth-x64', architecture: 'guest-x64',
        executable: 'libvenus_heaven_material_replay.so', seconds: 0, timeoutMs: 120000 });
      tests.push({ id: 'venus-heaven-material-captured-constant-x64', architecture: 'guest-x64',
        executable: 'libvenus_heaven_material_replay.so', seconds: 0, timeoutMs: 120000 });
    }
    if (includeVenusHeavenDraw0) {
      tests.push({ id: 'venus-heaven-material-captured-draw0-loaded-x64', architecture: 'guest-x64',
        executable: 'libvenus_heaven_material_replay.so', seconds: 0, timeoutMs: 120000 });
      tests.push({ id: 'venus-heaven-material-captured-draw0-clear-x64', architecture: 'guest-x64',
        executable: 'libvenus_heaven_material_replay.so', seconds: 0, timeoutMs: 120000 });
      tests.push({ id: 'venus-heaven-material-captured-draw0-nodepth-x64', architecture: 'guest-x64',
        executable: 'libvenus_heaven_material_replay.so', seconds: 0, timeoutMs: 120000 });
      tests.push({ id: 'venus-heaven-material-captured-draw0-input-verify-nodepth-x64',
        architecture: 'guest-x64', executable: 'libvenus_heaven_material_replay.so',
        seconds: 0, timeoutMs: 180000 });
      tests.push({ id: 'venus-heaven-material-captured-draw0-always-x64', architecture: 'guest-x64',
        executable: 'libvenus_heaven_material_replay.so', seconds: 0, timeoutMs: 120000 });
    }
    if (includeVenusHeavenDraw170) {
      tests.push({ id: 'venus-heaven-material-captured-draw170-clear-x64', architecture: 'guest-x64',
        executable: 'libvenus_heaven_material_replay.so', seconds: 0, timeoutMs: 120000 });
      tests.push({ id: 'venus-heaven-material-captured-draw170-nodepth-x64', architecture: 'guest-x64',
        executable: 'libvenus_heaven_material_replay.so', seconds: 0, timeoutMs: 120000 });
      tests.push({ id: 'venus-heaven-material-captured-draw170-always-x64', architecture: 'guest-x64',
        executable: 'libvenus_heaven_material_replay.so', seconds: 0, timeoutMs: 120000 });
    }
    if (includeVenusHeavenF647) {
      tests.push({ id: 'venus-heaven-material-captured-f647-baseline-x64',
        architecture: 'guest-x64', executable: 'libvenus_heaven_material_replay.so',
        seconds: 0, timeoutMs: 180000 });
      tests.push({ id: 'venus-heaven-material-captured-f647-input-verify-nodepth-x64',
        architecture: 'guest-x64', executable: 'libvenus_heaven_material_replay.so',
        seconds: 0, timeoutMs: 180000 });
      tests.push({ id: 'venus-heaven-material-captured-f647-loaded-x64',
        architecture: 'guest-x64', executable: 'libvenus_heaven_material_replay.so',
        seconds: 0, timeoutMs: 180000 });
      tests.push({ id: 'venus-heaven-material-captured-f647-nodepth-x64',
        architecture: 'guest-x64', executable: 'libvenus_heaven_material_replay.so',
        seconds: 0, timeoutMs: 180000 });
      tests.push({ id: 'venus-heaven-material-captured-f647-always-x64',
        architecture: 'guest-x64', executable: 'libvenus_heaven_material_replay.so',
        seconds: 0, timeoutMs: 180000 });
    }
    if (includeVenusHeavenInputs) {
      tests.push({ id: 'venus-heaven-material-captured-input-verify-nodepth-x64',
        architecture: 'guest-x64', executable: 'libvenus_heaven_material_replay.so',
        seconds: 0, timeoutMs: 120000 });
    }
    if (includeVenusHeavenCapturedAb) {
      tests.push({ id: 'venus-heaven-material-captured-x64', architecture: 'guest-x64',
        executable: 'libvenus_heaven_material_replay.so', seconds: 0, timeoutMs: 120000 });
      tests.push({ id: 'venus-heaven-material-captured-nodepth-x64', architecture: 'guest-x64',
        executable: 'libvenus_heaven_material_replay.so', seconds: 0, timeoutMs: 120000 });
      tests.push({ id: 'venus-heaven-material-captured-depth-always-x64', architecture: 'guest-x64',
        executable: 'libvenus_heaven_material_replay.so', seconds: 0, timeoutMs: 120000 });
      tests.push({ id: 'venus-heaven-material-captured-no-kill-x64', architecture: 'guest-x64',
        executable: 'libvenus_heaven_material_replay.so', seconds: 0, timeoutMs: 120000 });
      tests.push({ id: 'venus-heaven-material-captured-force-opaque-x64', architecture: 'guest-x64',
        executable: 'libvenus_heaven_material_replay.so', seconds: 0, timeoutMs: 120000 });
      tests.push({ id: 'venus-heaven-material-captured-t0-implicit-x64', architecture: 'guest-x64',
        executable: 'libvenus_heaven_material_replay.so', seconds: 0, timeoutMs: 120000 });
      tests.push({ id: 'venus-heaven-material-captured-t0-lod0-x64', architecture: 'guest-x64',
        executable: 'libvenus_heaven_material_replay.so', seconds: 0, timeoutMs: 120000 });
      tests.push({ id: 'venus-heaven-material-captured-explicit-kill-init-x64', architecture: 'guest-x64',
        executable: 'libvenus_heaven_material_replay.so', seconds: 0, timeoutMs: 120000 });
      tests.push({ id: 'venus-heaven-material-captured-kill-direct-bit-x64', architecture: 'guest-x64',
        executable: 'libvenus_heaven_material_replay.so', seconds: 0, timeoutMs: 120000 });
      tests.push({ id: 'venus-heaven-material-captured-kill-direct-compare-x64', architecture: 'guest-x64',
        executable: 'libvenus_heaven_material_replay.so', seconds: 0, timeoutMs: 120000 });
      tests.push({ id: 'venus-heaven-material-captured-kill-recompare-x64', architecture: 'guest-x64',
        executable: 'libvenus_heaven_material_replay.so', seconds: 0, timeoutMs: 120000 });
      tests.push({ id: 'venus-heaven-material-captured-local-kill-x64', architecture: 'guest-x64',
        executable: 'libvenus_heaven_material_replay.so', seconds: 0, timeoutMs: 120000 });
      tests.push({ id: 'venus-heaven-material-captured-compare-color-x64', architecture: 'guest-x64',
        executable: 'libvenus_heaven_material_replay.so', seconds: 0, timeoutMs: 120000 });
      tests.push({ id: 'venus-heaven-material-captured-inline-kill-x64', architecture: 'guest-x64',
        executable: 'libvenus_heaven_material_replay.so', seconds: 0, timeoutMs: 120000 });
      tests.push({ id: 'venus-heaven-material-captured-uint-kill-x64', architecture: 'guest-x64',
        executable: 'libvenus_heaven_material_replay.so', seconds: 0, timeoutMs: 120000 });
      tests.push({ id: 'venus-heaven-material-captured-compare-alpha-x64', architecture: 'guest-x64',
        executable: 'libvenus_heaven_material_replay.so', seconds: 0, timeoutMs: 120000 });
      tests.push({ id: 'venus-heaven-material-captured-no-float-x64', architecture: 'guest-x64',
        executable: 'libvenus_heaven_material_replay.so', seconds: 0, timeoutMs: 120000 });
      tests.push({ id: 'venus-heaven-material-captured-compare-alpha-no-float-x64', architecture: 'guest-x64',
        executable: 'libvenus_heaven_material_replay.so', seconds: 0, timeoutMs: 120000 });
      tests.push({ id: 'venus-heaven-material-captured-sample-mask-discard-x64', architecture: 'guest-x64',
        executable: 'libvenus_heaven_material_replay.so', seconds: 0, timeoutMs: 120000 });
      tests.push({ id: 'venus-heaven-material-captured-sample-mask-keep-x64', architecture: 'guest-x64',
        executable: 'libvenus_heaven_material_replay.so', seconds: 0, timeoutMs: 120000 });
      tests.push({ id: 'venus-heaven-material-captured-inlined-x64', architecture: 'guest-x64',
        executable: 'libvenus_heaven_material_replay.so', seconds: 0, timeoutMs: 120000 });
      tests.push({ id: 'venus-heaven-material-captured-inlined-local-x64', architecture: 'guest-x64',
        executable: 'libvenus_heaven_material_replay.so', seconds: 0, timeoutMs: 120000 });
      tests.push({ id: 'venus-heaven-material-captured-return-compare-x64', architecture: 'guest-x64',
        executable: 'libvenus_heaven_material_replay.so', seconds: 0, timeoutMs: 120000 });
      tests.push({ id: 'venus-heaven-material-captured-sample-mask-final-select-x64', architecture: 'guest-x64',
        executable: 'libvenus_heaven_material_replay.so', seconds: 0, timeoutMs: 120000 });
      tests.push({ id: 'venus-heaven-material-captured-sample-mask-final-branch-x64', architecture: 'guest-x64',
        executable: 'libvenus_heaven_material_replay.so', seconds: 0, timeoutMs: 120000 });
      tests.push({ id: 'venus-heaven-material-captured-terminate-invocation-x64', architecture: 'guest-x64',
        executable: 'libvenus_heaven_material_replay.so', seconds: 0, timeoutMs: 120000 });
      tests.push({ id: 'venus-heaven-material-captured-ccp-x64', architecture: 'guest-x64',
        executable: 'libvenus_heaven_material_replay.so', seconds: 0, timeoutMs: 120000 });
    }
    if (includeVenusHeavenDiscardAb) {
      for (const mode of ['captured-nodepth', 'captured-compare-alpha-nodepth',
        'captured-omap-frozen-nodepth', 'captured-no-kill-omap-frozen-nodepth',
        'captured-kill-direct-clean-omap-frozen-nodepth',
        'captured-return-compare-clean-omap-frozen-nodepth',
        'captured-sample-mask-direct-clean-omap-frozen-nodepth']) {
        tests.push({ id: `venus-heaven-material-${mode}-x64`, architecture: 'guest-x64',
          executable: 'libvenus_heaven_material_replay.so', seconds: 0, timeoutMs: 120000 });
      }
    }
    if (includeVenusHeavenMaterialLayout) {
      for (const mode of ['no-set', 'empty', 'small', 'ubo', 'dynamic', 'sampler', 'sampled', 'full', 'exact']) {
        tests.push({ id: `venus-heaven-material-${mode}-x64`, architecture: 'guest-x64',
          executable: 'libvenus_heaven_material_replay.so', seconds: 0, timeoutMs: 120000 });
      }
    }
    if (includeHostHeaven) {
      tests.push({ id: 'host-heaven-original-nodepth', architecture: 'host-native',
        executable: 'heaven_exact_host_replay', seconds: 0, timeoutMs: 120000 });
      tests.push({ id: 'host-heaven-compare-alpha-nodepth', architecture: 'host-native',
        executable: 'heaven_exact_host_replay', seconds: 0, timeoutMs: 120000 });
      tests.push({ id: 'host-heaven-no-kill-nodepth', architecture: 'host-native',
        executable: 'heaven_exact_host_replay', seconds: 0, timeoutMs: 120000 });
    }
    if (includeHostHeavenMaterialDepth) {
      tests.push({ id: 'host-heaven-material-depth-pass0', architecture: 'host-native',
        executable: 'heaven_exact_host_replay', seconds: 0, timeoutMs: 120000 });
      tests.push({ id: 'host-heaven-material-depth-pass3', architecture: 'host-native',
        executable: 'heaven_exact_host_replay', seconds: 0, timeoutMs: 120000 });
      tests.push({ id: 'host-heaven-material-depth-pass0-no-float-controls',
        architecture: 'host-native', executable: 'heaven_exact_host_replay',
        seconds: 0, timeoutMs: 120000 });
      tests.push({ id: 'host-heaven-material-depth-pass3-no-float-controls',
        architecture: 'host-native', executable: 'heaven_exact_host_replay',
        seconds: 0, timeoutMs: 120000 });
    }
    if (includeHostHeavenInputs) {
      tests.push({ id: 'host-heaven-original-input-verify-nodepth',
        architecture: 'host-native', executable: 'heaven_exact_host_replay',
        seconds: 0, timeoutMs: 120000 });
    }
    if (includeWineVulkan) {
      tests.push({ id: 'wine-vulkan-offscreen-x64', architecture: 'x64',
        executable: 'winehua_vulkan_smoke.exe', seconds: 0, timeoutMs: 90000 });
      tests.push({ id: 'wine-vulkan-offscreen-x86', architecture: 'x86',
        executable: 'winehua_vulkan_smoke.exe', seconds: 0, timeoutMs: 90000 });
      tests.push({ id: 'wine-vulkan-sampled-only-x64', architecture: 'x64',
        executable: 'winehua_vulkan_smoke.exe', seconds: 0, timeoutMs: 90000 });
      tests.push({ id: 'wine-vulkan-sampled-only-x86', architecture: 'x86',
        executable: 'winehua_vulkan_smoke.exe', seconds: 0, timeoutMs: 90000 });
    }
    if (includeWineVulkanPresent) {
      tests.push({ id: 'wine-vulkan-present-x64', architecture: 'x64',
        executable: 'winehua_vulkan_smoke.exe', seconds: 5, timeoutMs: 180000 });
      tests.push({ id: 'wine-vulkan-present-x86', architecture: 'x86',
        executable: 'winehua_vulkan_smoke.exe', seconds: 5, timeoutMs: 180000 });
    }
    if (includeDxvk26Requirements) {
      /* The probe does not load DXVK.  It qualifies the exact Windows Vulkan
       * transport seen by DXVK 2.6.2 before a Modern DLL is introduced. */
      tests.push({ id: 'dxvk26-requirements-x86', architecture: 'x86',
        executable: 'winehua_dxvk26_requirements.exe', seconds: 0, timeoutMs: 90000 });
      tests.push({ id: 'dxvk26-requirements-x64', architecture: 'x64',
        executable: 'winehua_dxvk26_requirements.exe', seconds: 0, timeoutMs: 90000 });
      /* Guest qualification is intentionally last. Its renderer teardown can
       * take longer than a Win32 probe's bounded diagnostic, so it must not
       * hide the Windows Vulkan transport result needed for DXVK bring-up. */
      tests.push({ id: 'guest-dxvk26-requirements-x64', architecture: 'guest-x64',
        executable: 'winehua_guest_vulkan_smoke', seconds: 0, timeoutMs: 90000 });
    }
    if (includeVkd3dCapability) {
      tests.push({ id: 'vkd3d-capability-pe-x64', architecture: 'x64',
        executable: 'winehua_dxvk26_requirements.exe', seconds: 0, timeoutMs: 90000 });
      tests.push({ id: 'guest-vkd3d-capability-x64', architecture: 'guest-x64',
        executable: 'winehua_guest_vulkan_smoke', seconds: 0, timeoutMs: 90000 });
      tests.push({ id: 'wine-vkd3d-capability-x64', architecture: 'x64',
        executable: 'winehua_vulkan_smoke.exe', seconds: 0, timeoutMs: 90000 });
    }
    if (includeGpuDiagnostics) {
      /* This runs through the Legacy overlay deliberately. The program then
       * proves its loaded DLL paths and reports Modern eligibility separately. */
      tests.push({ id: 'gpu-diagnostics-x86', architecture: 'x86',
        executable: 'winehua_gpu_diagnostics.exe', seconds: 0, timeoutMs: 90000 });
      tests.push({ id: 'gpu-diagnostics-x64', architecture: 'x64',
        executable: 'winehua_gpu_diagnostics.exe', seconds: 0, timeoutMs: 90000 });
    }
    if (includeDxvk) {
      /* On the current Harmony Venus renderer, creating an x64 DXVK device
       * and immediately creating the WoW64/x86 device can occasionally return
       * VK_ERROR_DEVICE_LOST while the host driver retires the first device.
       * Running the WoW64 case first is deterministic; the handoff wait above
       * still keeps both contexts strictly serial. */
      tests.push({ id: 'dxvk-legacy-x86', architecture: 'x86',
        executable: 'winehua_d3d11_smoke.exe', seconds: 5, timeoutMs: 180000 });
      tests.push({ id: 'dxvk-legacy-x64', architecture: 'x64',
        executable: 'winehua_d3d11_smoke.exe', seconds: 5, timeoutMs: 180000 });
      tests.push({ id: 'dxvk-cube-x64', architecture: 'x64',
        executable: 'winehua_d3d_switch_cube.exe', seconds: 8, timeoutMs: 180000 });
    }
    if (includeDxvkModernBaseline) {
      tests.push({ id: 'dxvk-modern-baseline-x86', architecture: 'x86',
        executable: 'winehua_d3d11_smoke.exe', seconds: 5, timeoutMs: 180000 });
      tests.push({ id: 'dxvk-modern-baseline-x64', architecture: 'x64',
        executable: 'winehua_d3d11_smoke.exe', seconds: 5, timeoutMs: 180000 });
      tests.push({ id: 'dxvk-modern-cube-x64', architecture: 'x64',
        executable: 'winehua_d3d_switch_cube.exe', seconds: 8, timeoutMs: 180000 });
    }
    if (includeDxvkLong) {
      tests.push({ id: 'dxvk-long-x64', architecture: 'x64',
        executable: 'winehua_d3d11_smoke.exe', seconds: this.request.longSeconds,
        timeoutMs: this.request.longSeconds * 1000 + 90000 });
    }
    if (includeDxvkModernLong) {
      tests.push({ id: 'dxvk-modern-long-x64', architecture: 'x64',
        executable: 'winehua_d3d11_smoke.exe', seconds: this.request.longSeconds,
        timeoutMs: this.request.longSeconds * 1000 + 90000 });
    }
    if (includeDxvkReplay) {
      tests.push({ id: 'dxvk-exact-replay-cs', architecture: 'guest-x64',
        executable: 'libvenus_spirv_replay.so', seconds: 0, timeoutMs: 120000 });
    }
    if (includeDxvkLayoutGeneral) {
      tests.push({ id: 'dxvk-layout-general-x86', architecture: 'x86',
        executable: 'winehua_d3d11_smoke.exe', seconds: 5, timeoutMs: 180000 });
      tests.push({ id: 'dxvk-layout-general-x64', architecture: 'x64',
        executable: 'winehua_d3d11_smoke.exe', seconds: 5, timeoutMs: 180000 });
    }
    if (includeDxvkCombined) {
      tests.push({ id: 'dxvk-combined-x86', architecture: 'x86',
        executable: 'winehua_d3d11_smoke.exe', seconds: 5, timeoutMs: 180000 });
      tests.push({ id: 'dxvk-combined-x64', architecture: 'x64',
        executable: 'winehua_d3d11_smoke.exe', seconds: 5, timeoutMs: 180000 });
    }
    if (includeDxvkDynamic) {
      tests.push({ id: 'dxvk-dynamic-cb-x86', architecture: 'x86',
        executable: 'winehua_d3d11_smoke.exe', seconds: 5, timeoutMs: 180000 });
      tests.push({ id: 'dxvk-dynamic-cb-x64', architecture: 'x64',
        executable: 'winehua_d3d11_smoke.exe', seconds: 5, timeoutMs: 180000 });
    }
    return tests;
  }

  private finalStatus(result: SmokeResult): boolean {
    return result.status === 'PASS' || result.status === 'FAIL' || result.status === 'SKIP' ||
      result.status === 'UNSUPPORTED';
  }

  private failureResult(spec: SmokeTestSpec, stage: string, message: string,
                        process: ProcessInfo | null): SmokeSuiteTestResult {
    return {
      schemaVersion: 1,
      runId: this.request.runId,
      testId: spec.id,
      status: 'FAIL',
      stage: stage,
      message: message,
      pid: process ? process.pid : -1,
      exitCode: process && process.exitCode !== null ? process.exitCode : -1,
      exitCodeSource: process ? process.exitCodeSource : 'unknown',
      heartbeatTimestampMs: Date.now()
    };
  }

  private async waitForFinal(spec: SmokeTestSpec, resultPath: string,
                             process: ProcessInfo): Promise<SmokeSuiteTestResult> {
    const deadline = Date.now() + spec.timeoutMs;
    while (Date.now() < deadline) {
      const text = readText(resultPath);
      if (text) {
        try {
          const result = JSON.parse(text) as SmokeResult;
          if (this.finalStatus(result)) {
            /* A result is authoritative, but it is not proof that the NCP
             * child has finished tearing down its Vulkan context.  In
             * particular, vkDestroyDevice may drain Venus work after the
             * smoke JSON has been atomically written.  Do not start the next
             * x86/x64 case while that context is still alive: concurrent
             * contexts can make the Harmony Venus ring report a fatal seqno
             * wait.  Give normal teardown a bounded grace period, then kill
             * the exact process returned by runWineProgram and wait for it to
             * disappear before returning the result.
             */
            for (let i = 0; i < 40; ++i) {
              const current = testNapi.queryWineProcess(process.pid) as ProcessInfo;
              if (!current.found || current.status !== 'running') break;
              await sleep(250);
            }
            const stillRunning = testNapi.queryWineProcess(process.pid) as ProcessInfo;
            if (stillRunning.found && stillRunning.status === 'running') {
              hilog.warn(DOMAIN, 'SmokeRunner',
                'final result arrived before process teardown pid=%{public}d test=%{public}s; terminating',
                process.pid, spec.id);
              testNapi.terminateWineProcess(process.pid);
              for (let i = 0; i < 20; ++i) {
                const current = testNapi.queryWineProcess(process.pid) as ProcessInfo;
                if (!current.found || current.status !== 'running') break;
                await sleep(250);
                }
            }
            if (spec.id.indexOf('wine-vulkan-present') >= 0 ||
              spec.id.indexOf('dxvk-legacy') >= 0 ||
              spec.id.indexOf('dxvk-modern') >= 0) {
              /* Process exit closes the vtest socket first; the Venus render
               * server frees the context asynchronously.  Leave a bounded
               * handoff window so the next architecture does not reuse the
               * ring while disconnect cleanup is still in flight. */
              await sleep(8000);
            }
            const parsed = JSON.parse(text) as SmokeSuiteTestResult;
            parsed.exitCodeSource = 'result';
            return parsed;
          }
        } catch (_) {}
      }
      const current = testNapi.queryWineProcess(process.pid) as ProcessInfo;
      if (!current.found || current.status !== 'running') {
        const detail = text ?
          'process disappeared with only a non-final result' :
          'process disappeared without an authoritative result';
        return this.failureResult(spec, 'crash', detail, current);
      }
      await sleep(500);
    }

    testNapi.terminateWineProcess(process.pid);
    for (let i = 0; i < 20; ++i) {
      const current = testNapi.queryWineProcess(process.pid) as ProcessInfo;
      if (!current.found || current.status !== 'running') break;
      await sleep(250);
    }
    if (spec.id.indexOf('wine-vulkan-present') >= 0 ||
      spec.id.indexOf('dxvk-legacy') >= 0 ||
      spec.id.indexOf('dxvk-modern') >= 0)
      await sleep(8000);
    return this.failureResult(spec, 'timeout', `test exceeded ${spec.timeoutMs} ms`, process);
  }

  private async waitForHostFinal(spec: SmokeTestSpec,
                                 resultPath: string): Promise<SmokeSuiteTestResult> {
    const deadline = Date.now() + spec.timeoutMs;
    while (Date.now() < deadline) {
      const text = readText(resultPath);
      if (text) {
        try {
          const result = JSON.parse(text) as SmokeResult;
          if (this.finalStatus(result)) {
            while (testNapi.isHostReplayRunning() && Date.now() < deadline) await sleep(25);
            const parsed = JSON.parse(text) as SmokeSuiteTestResult;
            parsed.exitCodeSource = 'result';
            return parsed;
          }
        } catch (_) {}
      }
      if (!testNapi.isHostReplayRunning())
        return this.failureResult(spec, 'crash',
          text ? 'Host replay stopped with only a non-final result' :
            'Host replay stopped without an authoritative result', null);
      await sleep(100);
    }
    return this.failureResult(spec, 'timeout', `test exceeded ${spec.timeoutMs} ms`, null);
  }

  private async runOne(spec: SmokeTestSpec): Promise<SmokeSuiteTestResult> {
    const resultPath = `${this.resultDirectory}/${spec.id}.json`;
    if (pathExists(resultPath)) fs.unlinkSync(resultPath);
    this.onStatus(`Smoke: ${spec.id}`);

    if (spec.architecture === 'host-native') {
      const executablePath = `${HOST_VULKAN_ROOT}/bin/${spec.executable}`;
      const materialDepth = spec.id.indexOf('host-heaven-material-depth-pass') === 0;
      const materialDepthPass3 =
        spec.id.indexOf('host-heaven-material-depth-pass3') === 0;
      const materialDepthNoFloatControls =
        materialDepth && spec.id.indexOf('-no-float-controls') >= 0;
      const capturedHeavenRoot = materialDepth ?
        `${GUEST_VULKAN_ROOT}/share/winehua/replay_external/heaven_material_depth` :
        `${GUEST_VULKAN_ROOT}/share/winehua/replay_external/heaven_exact`;
      const captureDirectory = materialDepth ?
        `${capturedHeavenRoot}/${materialDepthPass3 ? 'pass3' : 'pass0'}` :
        capturedHeavenRoot;
      if (!pathExists(`${HOST_VULKAN_ROOT}/manifest.json`) || !pathExists(executablePath))
        return this.failureResult(spec, 'startup', 'managed Host Vulkan replay is missing', null);
      if (!pathExists(captureDirectory))
        return this.failureResult(spec, 'startup', 'managed Heaven exact capture is missing', null);

      const fragmentName = spec.id.indexOf('compare-alpha') >= 0 ?
        'heaven_exact_fs_compare_alpha.spv' : spec.id.indexOf('no-kill') >= 0 ?
        'heaven_exact_fs_no_kill.spv' :
        'FS_844c52a11b923f49d2afee97a8d6119eef8efeb3.remapped-d5aa1ffafa7f5fa1.spv';
      const vertexName =
        'VS_809ef7d3d23ff811f90c51be8d0cfddf6994cdb0.remapped-3d526c03b048bdf1.spv';
      const fragmentPath = materialDepth ? `${capturedHeavenRoot}/depth.frag.spv` :
        `${capturedHeavenRoot}/${fragmentName}`;
      const vertexPath = materialDepth ?
        `${captureDirectory}/${materialDepthNoFloatControls ?
          'vs-no-float-controls.spv' : 'vs.spv'}` :
        `${capturedHeavenRoot}/${vertexName}`;
      const started = testNapi.runHostReplay({
        executablePath: executablePath,
        argv: ['--automation', '--run-id', this.request.runId, '--test-id', spec.id,
          '--result', resultPath,
          '--rgba-output', `${this.resultDirectory}/${spec.id}.rgba`,
          '--vertex-spv', vertexPath,
          '--fragment-spv', fragmentPath,
          '--capture-dir', captureDirectory,
          '--capture-color', 'sentinel', '--capture-depth', materialDepth ? 'clear' : 'disabled',
          ...(materialDepth ? ['--capture-depth-compare', 'less-equal', '--capture-profile',
            materialDepthPass3 ? 'material-pass3-depth' : 'material-pass0-depth'] : []),
          '--layout-mode', 'captured',
          ...(spec.id.indexOf('input-verify') >= 0 || materialDepth ?
            ['--verify-inputs', '1'] : [])],
        environment: {
          'WINEHUA_SMOKE_RUN_ID': this.request.runId,
          'WINEHUA_SMOKE_TEST_ID': spec.id
        },
        workingDirectory: HOST_VULKAN_ROOT,
        automationMode: true
      }) as boolean;
      if (!started)
        return this.failureResult(spec, 'startup', 'runHostReplay failed to start', null);
      return this.waitForHostFinal(spec, resultPath);
    }

    if (spec.architecture === 'guest-x64') {
      const executablePath = `${GUEST_VULKAN_ROOT}/bin/${spec.executable}`;
      if (!pathExists(`${GUEST_VULKAN_ROOT}/manifest.json`) || !pathExists(executablePath))
        return this.failureResult(spec, 'startup', 'managed Guest Vulkan runtime is missing', null);
      const currentReplayDirectory = `${this.prefix}/drive_c/smoke/results/${this.request.runId}`;
      const replaySpvDirectory = spec.executable === 'libvenus_spirv_replay.so' &&
        !hasReplayShader(currentReplayDirectory) ?
        latestReplayShaderDirectory(this.prefix, currentReplayDirectory) : currentReplayDirectory;
      const heavenFragment = spec.id.indexOf('no-float') >= 0 ?
        `${GUEST_VULKAN_ROOT}/share/winehua/replay_external/heaven_final_fs_no_float.spv` :
        spec.id.indexOf('constant') >= 0 ?
        `${GUEST_VULKAN_ROOT}/share/winehua/replay_external/heaven_constant.frag.spv` :
        spec.id.indexOf('contract-simple') >= 0 ?
        `${GUEST_VULKAN_ROOT}/share/winehua/replay_external/heaven_contract_simple.frag.spv` :
        spec.id.indexOf('exact-probe') >= 0 ?
        `${GUEST_VULKAN_ROOT}/share/winehua/replay_external/heaven_exact_probe.frag.spv` :
        `${GUEST_VULKAN_ROOT}/share/winehua/replay_external/heaven_final_fs.spv`;
      const heavenVertex = (spec.id.indexOf('constant') >= 0 ||
        spec.id.indexOf('contract-simple') >= 0 ||
        spec.id.indexOf('exact-fragment') >= 0 ||
        spec.id.indexOf('exact-probe') >= 0 ||
        spec.id.indexOf('bool-') >= 0) &&
        spec.id.indexOf('final-vertex') < 0 ?
        `${GUEST_VULKAN_ROOT}/share/winehua/venus_heaven_material.vert.spv` :
        `${GUEST_VULKAN_ROOT}/share/winehua/replay_external/heaven_final_vs.spv`;
      const replayVertex = spec.id.indexOf('final-fragment-simple-vertex') >= 0 ?
        `${GUEST_VULKAN_ROOT}/share/winehua/venus_heaven_material.vert.spv` : heavenVertex;
      const capturedHeavenDraw0 =
        spec.id.indexOf('venus-heaven-material-captured-draw0-') === 0;
      const capturedHeavenDraw0NoDepth =
        spec.id === 'venus-heaven-material-captured-draw0-nodepth-x64' ||
        spec.id === 'venus-heaven-material-captured-draw0-input-verify-nodepth-x64';
      const capturedHeavenDraw0Clear =
        spec.id === 'venus-heaven-material-captured-draw0-clear-x64';
      const capturedHeavenDraw0Always =
        spec.id === 'venus-heaven-material-captured-draw0-always-x64';
      const capturedHeavenDraw170 =
        spec.id.indexOf('venus-heaven-material-captured-draw170-') === 0;
      const capturedHeavenDraw170NoDepth =
        spec.id === 'venus-heaven-material-captured-draw170-nodepth-x64';
      const capturedHeavenDraw170Clear =
        spec.id === 'venus-heaven-material-captured-draw170-clear-x64';
      const capturedHeavenDraw170Always =
        spec.id === 'venus-heaven-material-captured-draw170-always-x64';
      const capturedHeavenF647 =
        spec.id.indexOf('venus-heaven-material-captured-f647-') === 0;
      const capturedHeavenF647Always =
        spec.id === 'venus-heaven-material-captured-f647-always-x64';
      const capturedHeavenF647Baseline =
        spec.id === 'venus-heaven-material-captured-f647-baseline-x64';
      const capturedHeavenMaterialDepth =
        spec.id.indexOf('venus-heaven-material-depth-pass') === 0;
      const capturedHeavenMaterialDepthPass3 =
        spec.id.indexOf('venus-heaven-material-depth-pass3') === 0;
      const capturedHeavenMaterialDepthNoFloatControls =
        capturedHeavenMaterialDepth && spec.id.indexOf('-no-float-controls') >= 0;
      const capturedHeavenMaterialDepthRoot =
        `${GUEST_VULKAN_ROOT}/share/winehua/replay_external/heaven_material_depth`;
      const capturedHeavenRoot = capturedHeavenDraw0 ?
        `${GUEST_VULKAN_ROOT}/share/winehua/replay_external/heaven_draw0` :
        capturedHeavenDraw170 ?
          `${GUEST_VULKAN_ROOT}/share/winehua/replay_external/heaven_draw170` :
          capturedHeavenF647 ?
            `${GUEST_VULKAN_ROOT}/share/winehua/replay_external/heaven_f647` :
          `${GUEST_VULKAN_ROOT}/share/winehua/replay_external/heaven_exact`;
      const capturedHeaven = spec.id.indexOf('venus-heaven-material-captured-') === 0;
      const capturedHeavenConstant = spec.id === 'venus-heaven-material-captured-constant-x64';
      const capturedHeavenNoDepth =
        spec.id.indexOf('venus-heaven-material-captured-') === 0 &&
        spec.id.indexOf('-nodepth-x64') >= 0;
      const capturedHeavenDepthAlways =
        spec.id === 'venus-heaven-material-captured-depth-always-x64' ||
        capturedHeavenDraw0Always || capturedHeavenDraw170Always ||
        capturedHeavenF647Always;
      const capturedHeavenNoKill =
        spec.id === 'venus-heaven-material-captured-no-kill-x64' ||
        spec.id === 'venus-heaven-material-captured-no-kill-nodepth-x64';
      const capturedHeavenForceOpaque =
        spec.id === 'venus-heaven-material-captured-force-opaque-x64';
      const capturedHeavenT0Implicit =
        spec.id === 'venus-heaven-material-captured-t0-implicit-x64';
      const capturedHeavenT0Lod0 =
        spec.id === 'venus-heaven-material-captured-t0-lod0-x64';
      const capturedHeavenExplicitKillInit =
        spec.id === 'venus-heaven-material-captured-explicit-kill-init-x64';
      const capturedHeavenKillDirectBit =
        spec.id === 'venus-heaven-material-captured-kill-direct-bit-x64';
      const capturedHeavenKillDirectCompare =
        spec.id === 'venus-heaven-material-captured-kill-direct-compare-x64';
      const capturedHeavenKillRecompare =
        spec.id === 'venus-heaven-material-captured-kill-recompare-x64';
      const capturedHeavenLocalKill =
        spec.id === 'venus-heaven-material-captured-local-kill-x64';
      const capturedHeavenCompareColor =
        spec.id === 'venus-heaven-material-captured-compare-color-x64';
      const capturedHeavenInlineKill =
        spec.id === 'venus-heaven-material-captured-inline-kill-x64';
      const capturedHeavenUintKill =
        spec.id === 'venus-heaven-material-captured-uint-kill-x64';
      const capturedHeavenCompareAlpha =
        spec.id === 'venus-heaven-material-captured-compare-alpha-x64' ||
        spec.id === 'venus-heaven-material-captured-compare-alpha-nodepth-x64';
      const capturedHeavenNoFloat =
        spec.id === 'venus-heaven-material-captured-no-float-x64';
      const capturedHeavenCompareAlphaNoFloat =
        spec.id === 'venus-heaven-material-captured-compare-alpha-no-float-x64';
      const capturedHeavenSampleMaskDiscard =
        spec.id === 'venus-heaven-material-captured-sample-mask-discard-x64';
      const capturedHeavenSampleMaskKeep =
        spec.id === 'venus-heaven-material-captured-sample-mask-keep-x64' ||
        spec.id === 'venus-heaven-material-captured-sample-mask-keep-nodepth-x64';
      const capturedHeavenInlined =
        spec.id === 'venus-heaven-material-captured-inlined-x64';
      const capturedHeavenInlinedLocal =
        spec.id === 'venus-heaven-material-captured-inlined-local-x64';
      const capturedHeavenReturnCompare =
        spec.id === 'venus-heaven-material-captured-return-compare-x64';
      const capturedHeavenSampleMaskFinalSelect =
        spec.id === 'venus-heaven-material-captured-sample-mask-final-select-x64' ||
        spec.id === 'venus-heaven-material-captured-sample-mask-final-select-nodepth-x64';
      const capturedHeavenSampleMaskFinalBranch =
        spec.id === 'venus-heaven-material-captured-sample-mask-final-branch-x64' ||
        spec.id === 'venus-heaven-material-captured-sample-mask-final-branch-nodepth-x64';
      const capturedHeavenTerminateInvocation =
        spec.id === 'venus-heaven-material-captured-terminate-invocation-x64';
      const capturedHeavenKillDirectClean =
        spec.id === 'venus-heaven-material-captured-kill-direct-clean-nodepth-x64';
      const capturedHeavenReturnCompareClean =
        spec.id === 'venus-heaven-material-captured-return-compare-clean-nodepth-x64';
      const capturedHeavenSampleMaskDirectClean =
        spec.id === 'venus-heaven-material-captured-sample-mask-direct-clean-nodepth-x64';
      const capturedHeavenOmapFrozen =
        spec.id === 'venus-heaven-material-captured-omap-frozen-nodepth-x64';
      const capturedHeavenNoKillOmapFrozen =
        spec.id === 'venus-heaven-material-captured-no-kill-omap-frozen-nodepth-x64';
      const capturedHeavenKillDirectCleanOmapFrozen = spec.id ===
        'venus-heaven-material-captured-kill-direct-clean-omap-frozen-nodepth-x64';
      const capturedHeavenReturnCompareCleanOmapFrozen = spec.id ===
        'venus-heaven-material-captured-return-compare-clean-omap-frozen-nodepth-x64';
      const capturedHeavenSampleMaskDirectCleanOmapFrozen = spec.id ===
        'venus-heaven-material-captured-sample-mask-direct-clean-omap-frozen-nodepth-x64';
      const capturedHeavenCcp =
        spec.id === 'venus-heaven-material-captured-ccp-x64';
      const capturedHeavenFragment = capturedHeavenDraw0 ?
        `${capturedHeavenRoot}/FS_56289d3e1ccd04a77c3d954c5ea8fe76a545a831.remapped.spv` :
        capturedHeavenDraw170 ? `${capturedHeavenRoot}/` +
          'FS_e47a0b705fa8d6f25bab50a322b14606f914dac6.remapped-6f2ed2660be66fad.spv' :
        capturedHeavenF647 ? `${capturedHeavenRoot}/` +
          'FS_f64724cbe909fa7080a3f266cec16847ec90bb92.remapped-4a2c265ca77b091e.spv' :
        capturedHeavenConstant ?
        `${GUEST_VULKAN_ROOT}/share/winehua/replay_external/heaven_constant.frag.spv` :
        capturedHeavenNoKill ? `${capturedHeavenRoot}/heaven_exact_fs_no_kill.spv` :
        capturedHeavenForceOpaque ? `${capturedHeavenRoot}/heaven_exact_fs_force_opaque.spv` :
        capturedHeavenT0Implicit ? `${capturedHeavenRoot}/heaven_exact_fs_t0_implicit.spv` :
        capturedHeavenT0Lod0 ? `${capturedHeavenRoot}/heaven_exact_fs_t0_lod0.spv` :
        capturedHeavenExplicitKillInit ?
          `${capturedHeavenRoot}/heaven_exact_fs_explicit_kill_init.spv` :
        capturedHeavenKillDirectBit ?
          `${capturedHeavenRoot}/heaven_exact_fs_kill_direct_bit.spv` :
        capturedHeavenKillDirectCompare ?
          `${capturedHeavenRoot}/heaven_exact_fs_kill_direct_compare.spv` :
        capturedHeavenKillRecompare ?
          `${capturedHeavenRoot}/heaven_exact_fs_kill_recompare.spv` :
        capturedHeavenLocalKill ?
          `${capturedHeavenRoot}/heaven_exact_fs_local_kill.spv` :
        capturedHeavenCompareColor ?
          `${capturedHeavenRoot}/heaven_exact_fs_compare_color.spv` :
        capturedHeavenInlineKill ?
          `${capturedHeavenRoot}/heaven_exact_fs_inline_kill.spv` :
        capturedHeavenUintKill ?
          `${capturedHeavenRoot}/heaven_exact_fs_uint_kill.spv` :
        capturedHeavenCompareAlpha ?
          `${capturedHeavenRoot}/heaven_exact_fs_compare_alpha.spv` :
        capturedHeavenNoFloat ?
          `${capturedHeavenRoot}/heaven_exact_fs_no_float.spv` :
        capturedHeavenCompareAlphaNoFloat ?
          `${capturedHeavenRoot}/heaven_exact_fs_compare_alpha_no_float.spv` :
        capturedHeavenSampleMaskDiscard ?
          `${capturedHeavenRoot}/heaven_exact_fs_sample_mask_discard.spv` :
        capturedHeavenSampleMaskKeep ?
          `${capturedHeavenRoot}/heaven_exact_fs_sample_mask_keep.spv` :
        capturedHeavenInlined ? `${capturedHeavenRoot}/heaven_exact_fs_inlined.spv` :
        capturedHeavenInlinedLocal ?
          `${capturedHeavenRoot}/heaven_exact_fs_inlined_local.spv` :
        capturedHeavenReturnCompare ?
          `${capturedHeavenRoot}/heaven_exact_fs_return_compare.spv` :
        capturedHeavenSampleMaskFinalSelect ?
          `${capturedHeavenRoot}/heaven_exact_fs_sample_mask_final_select.spv` :
        capturedHeavenSampleMaskFinalBranch ?
          `${capturedHeavenRoot}/heaven_exact_fs_sample_mask_final_branch.spv` :
        capturedHeavenTerminateInvocation ?
          `${capturedHeavenRoot}/heaven_exact_fs_terminate_invocation.spv` :
        capturedHeavenKillDirectClean ?
          `${capturedHeavenRoot}/heaven_exact_fs_kill_direct_clean.spv` :
        capturedHeavenReturnCompareClean ?
          `${capturedHeavenRoot}/heaven_exact_fs_return_compare_clean.spv` :
        capturedHeavenSampleMaskDirectClean ?
          `${capturedHeavenRoot}/heaven_exact_fs_sample_mask_direct_clean.spv` :
        capturedHeavenOmapFrozen ?
          `${capturedHeavenRoot}/heaven_exact_fs_omap_frozen.spv` :
        capturedHeavenNoKillOmapFrozen ?
          `${capturedHeavenRoot}/heaven_exact_fs_no_kill_omap_frozen.spv` :
        capturedHeavenKillDirectCleanOmapFrozen ?
          `${capturedHeavenRoot}/heaven_exact_fs_kill_direct_clean_omap_frozen.spv` :
        capturedHeavenReturnCompareCleanOmapFrozen ?
          `${capturedHeavenRoot}/heaven_exact_fs_return_compare_clean_omap_frozen.spv` :
        capturedHeavenSampleMaskDirectCleanOmapFrozen ?
          `${capturedHeavenRoot}/heaven_exact_fs_sample_mask_direct_clean_omap_frozen.spv` :
        capturedHeavenCcp ? `${capturedHeavenRoot}/heaven_exact_fs_ccp.spv` :
        `${capturedHeavenRoot}/` +
        'FS_844c52a11b923f49d2afee97a8d6119eef8efeb3.remapped-d5aa1ffafa7f5fa1.spv';
      const capturedHeavenVertex = capturedHeavenDraw0 ?
        `${capturedHeavenRoot}/VS_809ef7d3d23ff811f90c51be8d0cfddf6994cdb0.remapped.spv` :
        capturedHeavenDraw170 ? `${capturedHeavenRoot}/` +
          'VS_809ef7d3d23ff811f90c51be8d0cfddf6994cdb0.remapped-3d526c03b048bdf1.spv' :
        capturedHeavenF647 ? `${capturedHeavenRoot}/` +
          'VS_fa0f746828dc66ef12b425928c234654b310a37e.remapped-1eded4254f7290.spv' :
        `${capturedHeavenRoot}/` +
        'VS_809ef7d3d23ff811f90c51be8d0cfddf6994cdb0.remapped-3d526c03b048bdf1.spv';
      const selectedHeavenFragment = capturedHeavenMaterialDepth ?
        `${capturedHeavenMaterialDepthRoot}/depth.frag.spv` :
        capturedHeaven ? capturedHeavenFragment : heavenFragment;
      const selectedHeavenVertex = capturedHeavenMaterialDepth ?
        `${capturedHeavenMaterialDepthRoot}/${capturedHeavenMaterialDepthPass3 ?
          'pass3' : 'pass0'}/${capturedHeavenMaterialDepthNoFloatControls ?
            'vs-no-float-controls.spv' : 'vs.spv'}` :
        capturedHeaven ? capturedHeavenVertex : replayVertex;
      const guestProcess = testNapi.runGuestProgram({
        executablePath: executablePath,
        argv: ['--automation', '--run-id', this.request.runId, '--test-id', spec.id,
          '--result', resultPath,
          ...(spec.id === 'guest-dxvk26-requirements-x64' ?
            ['--dxvk26-requirements', '1'] : []),
          ...(spec.id === 'guest-vkd3d-capability-x64' ? ['--vkd3d-capability'] : []),
          ...(spec.id.indexOf('general') >= 0 ? ['--layout', 'general'] : []),
          ...(spec.id.indexOf('sampled-only') >= 0 ? ['--sampled-only'] : []),
          ...(spec.id.indexOf('idle') >= 0 ? ['--force-idle'] : []),
          ...(spec.id === 'venus-depth-cube-x64' ? ['--depth-cube'] : []),
          ...(spec.id === 'venus-depth-cube-graphics-x64' ?
            ['--depth-cube-graphics'] : []),
          ...(spec.id === 'venus-depth-cube-graphics-golden-x64' ?
            ['--depth-cube-graphics-golden'] : []),
          ...(spec.id === 'venus-depth-cube-graphics-golden-dxvk-contract-x64' ?
            ['--depth-cube-graphics-golden-dxvk-contract'] : []),
          ...(spec.id === 'venus-depth-cube-graphics-initialized-x64' ?
            ['--depth-cube-graphics-initialized'] : []),
          ...(spec.id === 'venus-depth-cube-graphics-optimized-x64' ?
            ['--depth-cube-graphics-optimized'] : []),
          ...(spec.id === 'venus-depth-cube-graphics-coordinate-trace-x64' ?
            ['--depth-cube-graphics-coordinate-trace'] : []),
          ...(spec.id === 'venus-depth-cube-graphics-padded-dref-x64' ?
            ['--depth-cube-graphics-padded-dref'] : []),
          ...(spec.id === 'venus-depth-cube-graphics-no-float-controls-x64' ?
            ['--depth-cube-graphics-no-float-controls'] : []),
          ...(spec.id.indexOf('venus-heaven-material-bool-') === 0 ?
            ['--bool-mode', spec.id.indexOf('bool-false') >= 0 ? 'false' : 'true'] : []),
          ...(spec.id === 'venus-heaven-material-constant-final-vertex-x64' ?
            ['--vs-transform-probe', '1'] : []),
          ...(capturedHeaven ?
            ['--capture-dir', capturedHeavenRoot,
              '--capture-color', capturedHeavenF647 ? 'golden' : 'sentinel',
              '--capture-depth',
              capturedHeavenDraw0 ?
                (capturedHeavenDraw0NoDepth ? 'disabled' : capturedHeavenDraw0Clear ?
                  'clear' : 'loaded') : capturedHeavenDraw170 ?
                  (capturedHeavenDraw170NoDepth ? 'disabled' : capturedHeavenDraw170Clear ?
                    'clear' : 'loaded') : capturedHeavenF647 ?
                    (capturedHeavenNoDepth ? 'disabled' : 'loaded') :
                    capturedHeavenNoDepth ? 'disabled' : 'clear',
              '--capture-depth-compare', capturedHeavenDepthAlways ? 'always' :
                capturedHeavenF647 ? 'equal' : 'less-equal'] : []),
          ...(capturedHeavenDraw0 ? ['--capture-profile', 'draw0'] : []),
          ...(capturedHeavenDraw170 ? ['--capture-profile', 'draw170'] : []),
          ...(capturedHeavenF647 ? ['--capture-profile', 'f647'] : []),
          ...(capturedHeavenMaterialDepth ? [
            '--capture-dir', `${capturedHeavenMaterialDepthRoot}/${capturedHeavenMaterialDepthPass3 ? 'pass3' : 'pass0'}`,
            '--capture-color', 'sentinel', '--capture-depth', 'clear',
            '--capture-depth-compare', 'less-equal', '--capture-profile',
            capturedHeavenMaterialDepthPass3 ? 'material-pass3-depth' : 'material-pass0-depth'
          ] : []),
          ...(capturedHeavenF647Baseline ? ['--skip-draw', '1'] : []),
          ...(spec.id.indexOf('venus-heaven-material-') === 0 ?
            ['--vertex-spv', selectedHeavenVertex,
              '--fragment-spv', selectedHeavenFragment,
              '--rgba-output', `${this.resultDirectory}/${spec.id}.rgba`] : []),
          ...(spec.id.endsWith('-empty-x64') ? ['--layout-mode', 'empty'] : []),
          ...(spec.id.endsWith('-no-set-x64') ? ['--layout-mode', 'no-set'] : []),
          ...(spec.id.endsWith('-small-x64') ? ['--layout-mode', 'small'] : []),
          ...(spec.id.endsWith('-ubo-x64') ? ['--layout-mode', 'ubo'] : []),
          ...(spec.id.endsWith('-dynamic-x64') ? ['--layout-mode', 'dynamic'] : []),
          ...(spec.id.endsWith('-sampler-x64') ? ['--layout-mode', 'sampler'] : []),
          ...(spec.id.endsWith('-sampled-x64') ? ['--layout-mode', 'sampled'] : []),
          ...(spec.id.endsWith('-full-x64') ? ['--layout-mode', 'full'] : []),
          ...(spec.id === 'venus-heaven-material-exact-x64' ||
            spec.id === 'venus-heaven-material-exact-fragment-x64' ||
            spec.id === 'venus-heaven-material-exact-probe-x64' ||
            spec.id === 'venus-heaven-material-graphics-x64' ||
            spec.id === 'venus-heaven-material-no-float-x64' ||
            spec.id === 'venus-heaven-material-constant-final-vertex-x64' ||
            spec.id === 'venus-heaven-material-constant-x64' ||
            spec.id === 'venus-heaven-material-bool-true-x64' ||
            spec.id === 'venus-heaven-material-bool-false-x64' ?
            ['--layout-mode', 'exact'] : []),
          ...(capturedHeaven ? ['--layout-mode', 'captured'] : []),
          ...(capturedHeavenMaterialDepth ? ['--layout-mode', 'captured'] : []),
          ...(spec.id.indexOf('input-verify') >= 0 || capturedHeavenMaterialDepth ?
            ['--verify-inputs', '1'] : []),
          ...(spec.id.endsWith('-exact-x64') ? ['--layout-mode', 'exact'] : []),
          ...(spec.id === 'venus-depth-cube-array-x64' ?
            ['--depth-cube-array'] : []),
          ...(spec.id === 'venus-depth-cube-array-2d-golden-x64' ?
            ['--depth-cube-array-2d-golden'] : []),
          ...(spec.executable === 'libvenus_spirv_replay.so' ? ['--spv-dir', replaySpvDirectory] : [])],
        environment: {
          'WINEHUA_SMOKE_RUN_ID': this.request.runId,
          'WINEHUA_SMOKE_TEST_ID': spec.id,
            /* Guest and Host map different shadow files. Functional probes use
             * explicit flush/invalidate for their tiny readback allocations. */
            'VN_WINEHUA_REMOTE_MEMORY_SYNC': '1',
            /* The OHOS shadow-memory bridge requires Host-side fence polling;
             * guest fence-feedback slots are not visible across the mappings. */
            'VN_PERF': spec.id === 'venus-offscreen-x64' ?
              'no_fence_feedback,no_query_feedback' : '',
            'WINEHUA_VKR_TRACE_PIPELINE':
              this.request.suite === 'venus-heaven-material-layout' ? '1' : '0',
            'VKR_WINEHUA_SHADOW_TRACE':
              spec.id.indexOf('input-verify') >= 0 || spec.id === 'venus-offscreen-x64' ? '1' : '0',
            'BOX64_DYNAREC_WEAKBARRIER':
              spec.id.indexOf('input-verify') >= 0 ? '0' : '2',
            'VN_WINEHUA_STRONG_RING_BARRIER':
              spec.id.indexOf('input-verify') >= 0 ? '1' : '0',
            /* Diagnostic only: establish whether 920 misses the Host idle
             * status and therefore omits the wake-up control command. */
            'VN_WINEHUA_ALWAYS_NOTIFY_RING':
              spec.id === 'venus-offscreen-x64' ? '1' : ''
          },
        workingDirectory: GUEST_VULKAN_ROOT,
        automationMode: true
      }) as ProcessInfo;
      if (!guestProcess.found || guestProcess.pid <= 0)
        return this.failureResult(spec, 'startup', 'runGuestProgram failed to return a PID', guestProcess);
      return this.waitForFinal(spec, resultPath, guestProcess);
    }

    const windowsResult = `C:\\smoke\\results\\${this.request.runId}\\${spec.id}.json`;
    const isWineVulkan = spec.executable === 'winehua_vulkan_smoke.exe';
    const isVkd3dCapability = spec.id === 'wine-vkd3d-capability-x64';
    const isVkd3dCapabilityPe = spec.id === 'vkd3d-capability-pe-x64';
    const isDxvk26Requirements = spec.executable === 'winehua_dxvk26_requirements.exe';
    const isGpuDiagnostics = spec.executable === 'winehua_gpu_diagnostics.exe';
    const isWineVulkanPresent = isWineVulkan && spec.id.indexOf('present') >= 0;
    const isD3d8 = spec.executable === 'winehua_d3d8_smoke.exe';
    const isD3d9 = spec.executable === 'winehua_d3d_switch_cube.exe' &&
      spec.id.indexOf('d3d9-') === 0;
    const isDxvk = !isD3d9 && (isGpuDiagnostics || spec.executable === 'winehua_d3d11_smoke.exe' ||
      spec.executable === 'winehua_d3d_switch_cube.exe');
    const isDxvkModern = isDxvk && spec.id.indexOf('dxvk-modern-') === 0;
    let process: ProcessInfo;
    if (isWineVulkan || isDxvk || isDxvk26Requirements) {
      // guest 栈架构从 guest_vulkan manifest 读取 (aarch64|x86_64)
      const guestArch = guestVulkanArch();
      const guestVulkanLibrary = `${GUEST_VULKAN_ROOT}/lib`;
      const guestVulkanIcd = `${GUEST_VULKAN_ROOT}/share/vulkan/icd.d/venus_icd.${guestArch}.json`;
      const wineUnixDir = guestArch === 'aarch64' ? 'aarch64-unix' : 'x86_64-unix';
      const box64LibraryPath = `${guestVulkanLibrary}:${WINE_ROOT}/bin/guest_gfx/lib:` +
        `${WINE_ROOT}/bin:${WINE_ROOT}/bin/${wineUnixDir}:${WINE_ROOT}/lib/${guestArch === 'aarch64' ? 'arm64' : 'x86_64'}`;
      const isPresent = isWineVulkanPresent || isDxvk;
      const smokeEnvironment: Record<string, string> = {
        'WINEHUA_SMOKE_RUN_ID': this.request.runId,
        'WINEHUA_SMOKE_TEST_ID': spec.id,
        'WINEHUA_SMOKE_ASSETS': 'C:\\smoke\\assets',
        'WINEHUA_VULKAN_RUNTIME': '1',
        'WINEHUA_VULKAN_LOADER_ARCH': guestArch,
        'WINEHUA_VENUS_ICD_ARCH': guestArch,
        'VK_DRIVER_FILES': guestVulkanIcd,
        'VK_ICD_FILENAMES': guestVulkanIcd,
        'VN_DEBUG': 'vtest',
        /* Wine Vulkan uses the same Guest/Host shadow mappings as DXVK.
         * Keep the B3 smoke on the real remote-memory path even though it
         * selects WineD3D as its unrelated D3D backend. */
        'VN_WINEHUA_REMOTE_MEMORY_SYNC': '1',
        /* Host GPU feedback lands in the Host side of WineHua's explicit
         * shadow mapping. Use the synchronous Host query path so a stale
         * Guest availability word cannot hide a valid query result. */
        'VN_PERF': isDxvk26Requirements
          /* Timeline semaphore feedback writes a Guest-side slot. WineHua's
           * remote Host/Guest shadow mapping does not make that write visible
           * to the Wine process, while the synchronous renderer query is
           * correct. Qualify the exact Modern runtime path by default. */
          ? 'no_fence_feedback,no_query_feedback,no_semaphore_feedback,no_multi_ring'
          : 'no_fence_feedback,no_query_feedback,no_multi_ring',
        /* Keep DLL load evidence for fallback detection without tracing every
         * Vulkan call. A caller can still override WINEDEBUG for a focused
         * diagnostic run through runWineProgram. */
        'WINEDEBUG': '+loaddll,+module'
      };
      // guestArch === 'x86_64' 时注入 box64 桥接 env:
      //  方案② (arm64 设备 + x86_64 wine + box64 转译) — 必需, box64 加载 x86_64 venus guest;
      //  方案③ (arm64 原生, guestArch=aarch64) — 不注入, aarch64 guest 原生 dlopen;
      //  方案① (x86_64 设备, guestArch=x86_64) — 多余注入但无害 (C++ guestElfMode 原生
      //  dlopen .so, 不读 USE_LIBBOX64/BOX64_*, 故不区分设备架构)。
      if (guestArch === 'x86_64') {
        smokeEnvironment['USE_LIBBOX64'] = '1';
        smokeEnvironment['BOX64_LOG'] = '1';
        smokeEnvironment['BOX64_LD_LIBRARY_PATH'] = box64LibraryPath;
        smokeEnvironment['BOX64_EMULATED_LIBS'] = 'libvulkan.so:libvulkan.so.1:' +
          'libEGL.so:libEGL.so.1:libGLESv2.so:libGLESv2.so.2:' +
          'libGLESv1_CM.so:libGLESv1_CM.so.1:libGL.so:libGL.so.1:' +
          'libwayland-client.so:libwayland-client.so.0:libwayland-server.so:' +
          'libwayland-server.so.0:libwayland-server.so.0:libwayland-egl.so:' +
          'libwayland-egl.so.1:libdrm.so:libdrm.so.2:libffi.so:libffi.so.8';
      }
      if (isDxvk26Requirements &&
        this.perfProfile === 'shadow-precise-no-semaphore-feedback') {
        /* Preserve this profile as an A/B diagnostic without allowing it to
         * accidentally exercise the unsafe feedback implementation. */
        smokeEnvironment['VN_PERF'] =
          'no_fence_feedback,no_query_feedback,no_semaphore_feedback,no_multi_ring';
      }
      if (isDxvk) {
        smokeEnvironment['BOX64_DYNAREC_WEAKBARRIER'] = '0';
        smokeEnvironment['WINEHUA_DXVK_VERSION'] = isDxvkModern ? '2.6.2' : '1.10.3';
        if (isDxvkModern)
          smokeEnvironment['VN_PERF'] =
            'no_fence_feedback,no_query_feedback,no_semaphore_feedback,no_multi_ring';
        if (isDxvkModern) {
          smokeEnvironment['VN_WINEHUA_STRONG_RING_BARRIER'] = '1';
          smokeEnvironment['DXVK_WINEHUA_TRACE_SAMPLED'] = '0';
          smokeEnvironment['DXVK_WINEHUA_TRACE_FLOW'] = '0';
          smokeEnvironment['DXVK_WINEHUA_TRACE_API'] = '0';
        }
        smokeEnvironment['DXVK_LOG_LEVEL'] = 'info';
        smokeEnvironment['DXVK_LOG_PATH'] = `C:\\smoke\\results\\${this.request.runId}`;
        if (!isDxvkModern) {
        smokeEnvironment['WINEHUA_DXVK_RELAXED_FEATURES'] = '1';
        smokeEnvironment['WINEDLLOVERRIDES'] = 'd3d11=n;dxgi=n';
        smokeEnvironment['DXVK_WINEHUA_TRACE_SAMPLED'] = '1';
        smokeEnvironment['DXVK_WINEHUA_TRACE_QUERY'] = '1';
        /* Bounded to 4096 records in DXVK. This preserves clear/bind/update/
         * draw ordering evidence when the exhaustive D3D11 smoke exits in a
         * later feature case. */
        smokeEnvironment['DXVK_WINEHUA_TRACE_FLOW'] = '1';
        smokeEnvironment['DXVK_WINEHUA_COMMAND_QUERY_RESET'] = '1';
        smokeEnvironment['WINEHUA_D3D11_STENCIL_QUERY'] = '1';
        /* Capture the exact DXBC-derived SPIR-V used by this run. Disable
         * the state cache so every shader reaches D3D11 shader compilation,
         * making the dump deterministic and replayable. */
        smokeEnvironment['DXVK_SHADER_DUMP_PATH'] = `C:\\smoke\\results\\${this.request.runId}`;
        smokeEnvironment['DXVK_WINEHUA_DUMP_REMAPPED_SPIRV'] = '1';
        smokeEnvironment['DXVK_STATE_CACHE'] = '0';
        if (this.request.batchMappedFlush) {
          smokeEnvironment['DXVK_WINEHUA_BATCH_MAPPED_FLUSH'] = '1';
          smokeEnvironment['DXVK_WINEHUA_BATCH_MAPPED_FLUSH_STATS'] = '1';
        } else {
          /* AppendD3dBackendEnv enables the qualified product path. Override
           * it here so a false request remains a real off-side A/B run. */
          smokeEnvironment['DXVK_WINEHUA_BATCH_MAPPED_FLUSH'] = '0';
        }
        }
        if (this.perfProfile === 'shadow-precise' ||
          this.perfProfile === 'shadow-precise-no-semaphore-feedback' ||
          this.perfProfile === 'shadow-precise-no-semaphore-feedback-single-ring' ||
          this.perfProfile === 'shadow-precise-no-semaphore-feedback-single-ring-sync-submit' ||
          this.perfProfile === 'shadow-precise-no-semaphore-feedback-single-ring-readback-idle' ||
          this.perfProfile === 'shadow-precise-no-semaphore-feedback-single-ring-trace' ||
          this.perfProfile === 'shadow-full-no-semaphore-feedback-single-ring-trace' ||
          this.perfProfile === 'shadow-precise-single-ring' ||
          this.perfProfile === 'shadow-precise-sync-submit' ||
          this.perfProfile === 'shadow-precise-strong-ring' ||
          this.perfProfile === 'shadow-precise-legacy-host-sync' ||
          this.perfProfile === 'shadow-precise-dirty-ring' ||
          this.perfProfile === 'shadow-precise-dirty-ring-no-upload' ||
          this.perfProfile === 'shadow-precise-dirty-ring-no-upload-fast' ||
          this.perfProfile === 'shadow-precise-dirty-ring-inline-upload' ||
          this.perfProfile === 'shadow-precise-dirty-ring-upload-wait' ||
          this.perfProfile === 'shadow-precise-dirty-ring-inline-upload-coverage-sort' ||
          this.perfProfile === 'shadow-precise-dirty-ring-coverage-sort-sampled' ||
          this.perfProfile === 'shadow-precise-dirty-ring-coverage-poll' ||
          this.perfProfile === 'shadow-precise-dirty-ring-inline-upload-alias-cover' ||
          this.perfProfile === 'shadow-precise-dirty-ring-inline-upload-serialized' ||
          this.perfProfile === 'shadow-precise-dirty-ring-inline-upload-descriptor-serialized' ||
          this.perfProfile === 'shadow-precise-dirty-ring-frame-assoc-trace' ||
          this.perfProfile === 'shadow-precise-dirty-ring-present-image-trace' ||
          this.perfProfile === 'shadow-precise-dirty-ring-perf' ||
          this.perfProfile === 'shadow-precise-dirty-ring-gpu-frame-profile' ||
          this.perfProfile === 'shadow-precise-dirty-ring-frame-timeline' ||
          this.perfProfile === 'shadow-precise-strong-ring-trace' ||
          this.perfProfile === 'shadow-precise-strong-ring-perf' ||
          this.perfProfile === 'shadow-precise-strong-ring-async-present' ||
          this.perfProfile === 'shadow-precise-strong-ring-fence-poll' ||
          this.perfProfile === 'shadow-precise-strong-ring-mailbox' ||
          this.perfProfile === 'shadow-precise-direct-fence' ||
          this.perfProfile === 'shadow-precise-retain-shmem' ||
          this.perfProfile === 'shadow-precise-cpu-upload') {
          smokeEnvironment['DXVK_WINEHUA_PRECISE_SHADOW'] = '1';
          smokeEnvironment['DXVK_WINEHUA_FLUSH_DYNAMIC_MAPPED'] = '1';
          smokeEnvironment['VN_WINEHUA_REMOTE_MEMORY_SYNC'] = '1';
          if (this.perfProfile === 'shadow-precise-no-semaphore-feedback')
            smokeEnvironment['VN_PERF'] =
              'no_fence_feedback,no_query_feedback,no_semaphore_feedback,no_multi_ring';
          if (this.perfProfile === 'shadow-precise-no-semaphore-feedback-single-ring' ||
            this.perfProfile === 'shadow-precise-no-semaphore-feedback-single-ring-sync-submit' ||
            this.perfProfile === 'shadow-precise-no-semaphore-feedback-single-ring-readback-idle' ||
            this.perfProfile === 'shadow-precise-no-semaphore-feedback-single-ring-trace' ||
            this.perfProfile === 'shadow-full-no-semaphore-feedback-single-ring-trace')
            smokeEnvironment['VN_PERF'] =
              'no_fence_feedback,no_query_feedback,no_semaphore_feedback,no_multi_ring';
          if (this.perfProfile ===
            'shadow-precise-no-semaphore-feedback-single-ring-sync-submit')
            smokeEnvironment['VN_PERF'] += ',no_async_queue_submit';
          if (this.perfProfile === 'shadow-precise-single-ring')
            smokeEnvironment['VN_PERF'] =
              'no_fence_feedback,no_query_feedback,no_multi_ring';
          if (this.perfProfile === 'shadow-precise-sync-submit')
            smokeEnvironment['VN_PERF'] =
              'no_fence_feedback,no_query_feedback,no_async_queue_submit';
          if (this.perfProfile === 'shadow-precise-no-semaphore-feedback-single-ring' ||
            this.perfProfile === 'shadow-precise-no-semaphore-feedback-single-ring-sync-submit' ||
            this.perfProfile === 'shadow-precise-no-semaphore-feedback-single-ring-readback-idle' ||
            this.perfProfile === 'shadow-precise-no-semaphore-feedback-single-ring-trace' ||
            this.perfProfile === 'shadow-precise-strong-ring' ||
            this.perfProfile === 'shadow-precise-legacy-host-sync' ||
            this.perfProfile === 'shadow-precise-dirty-ring' ||
          this.perfProfile === 'shadow-precise-dirty-ring-no-upload' ||
          this.perfProfile === 'shadow-precise-dirty-ring-no-upload-fast' ||
          this.perfProfile === 'shadow-precise-dirty-ring-inline-upload' ||
          this.perfProfile === 'shadow-precise-dirty-ring-upload-wait' ||
          this.perfProfile === 'shadow-precise-dirty-ring-inline-upload-coverage-sort' ||
          this.perfProfile === 'shadow-precise-dirty-ring-coverage-sort-sampled' ||
          this.perfProfile === 'shadow-precise-dirty-ring-coverage-poll' ||
          this.perfProfile === 'shadow-precise-dirty-ring-inline-upload-alias-cover' ||
          this.perfProfile === 'shadow-precise-dirty-ring-inline-upload-serialized' ||
          this.perfProfile === 'shadow-precise-dirty-ring-inline-upload-descriptor-serialized' ||
          this.perfProfile === 'shadow-precise-dirty-ring-frame-assoc-trace' ||
          this.perfProfile === 'shadow-precise-dirty-ring-present-image-trace' ||
          this.perfProfile === 'shadow-precise-dirty-ring-perf' ||
          this.perfProfile === 'shadow-precise-dirty-ring-gpu-frame-profile' ||
          this.perfProfile === 'shadow-precise-dirty-ring-frame-timeline' ||
            this.perfProfile === 'shadow-precise-strong-ring-trace' ||
            this.perfProfile === 'shadow-precise-strong-ring-perf' ||
            this.perfProfile === 'shadow-precise-strong-ring-async-present' ||
            this.perfProfile === 'shadow-precise-strong-ring-fence-poll' ||
            this.perfProfile === 'shadow-precise-strong-ring-mailbox')
            smokeEnvironment['VN_WINEHUA_STRONG_RING_BARRIER'] = '1';
          if (this.perfProfile ===
            'shadow-precise-no-semaphore-feedback-single-ring-readback-idle')
            smokeEnvironment['DXVK_WINEHUA_READBACK_DEVICE_IDLE'] = '1';
          if (this.perfProfile === 'shadow-precise-strong-ring-perf' ||
            this.perfProfile === 'shadow-precise-dirty-ring-perf')
            smokeEnvironment['VN_WINEHUA_PERF_SUMMARY'] = '1';
          if (this.perfProfile === 'shadow-precise-direct-fence')
            smokeEnvironment['VN_WINEHUA_DIRECT_FENCE_WAIT'] = '1';
          if (this.perfProfile === 'shadow-precise-dirty-ring-present-image-trace')
            smokeEnvironment['WINEHUA_DXVK_TRACE_PRESENT_IMAGE'] = '1';
          if (this.perfProfile === 'shadow-precise-retain-shmem')
            smokeEnvironment['VN_WINEHUA_DEFER_SHMEM_UNREF'] = '1';
          if (this.perfProfile === 'shadow-precise-cpu-upload')
            smokeEnvironment['VKR_WINEHUA_GPU_UPLOAD'] = '0';
        }
        if (this.request.suite === 'dxvk-layout-general')
          smokeEnvironment['DXVK_WINEHUA_FORCE_SAMPLED_GENERAL'] = '1';
      }
      if (spec.id.indexOf('wine-vulkan-sampled-only') >= 0)
        smokeEnvironment['WINEHUA_VULKAN_SAMPLED_ONLY'] = '1';
        /* Keep the current Venus runtime capability independent of the
         * diagnostic profile selected above. Profiles may disable more
         * feedback paths, but must not accidentally re-enable multi-ring. */
        if (smokeEnvironment['VN_PERF'].indexOf('no_multi_ring') < 0)
          smokeEnvironment['VN_PERF'] += ',no_multi_ring';
        if (isDxvkModern && smokeEnvironment['VN_PERF'].indexOf('no_semaphore_feedback') < 0)
          smokeEnvironment['VN_PERF'] += ',no_semaphore_feedback';
        if (spec.id.indexOf('dxvk-dynamic-cb') >= 0) {
          smokeEnvironment['WINEHUA_D3D11_DYNAMIC_CB'] = '1';
          smokeEnvironment['DXVK_WINEHUA_FLUSH_DYNAMIC_MAPPED'] = '1';
          smokeEnvironment['VN_WINEHUA_REMOTE_MEMORY_SYNC'] = '1';
        }
        if (spec.id.indexOf('dxvk-combined') >= 0)
          smokeEnvironment['DXVK_STATE_CACHE'] = '0';
        if (spec.id.indexOf('dxvk-combined') >= 0)
          smokeEnvironment['WINEHUA_D3D11_COMBINED_SAMPLER'] = '1';
      process = testNapi.runWineProgram({
        windowsExePath: `C:\\smoke\\${spec.architecture}\\${spec.executable}`,
        argv: ['--automation', '--run-id', this.request.runId, '--test-id', spec.id,
          '--result', windowsResult, isPresent ? '--present' : '--offscreen',
          ...(isVkd3dCapabilityPe ? ['--vkd3d-capability'] : []),
          ...(isVkd3dCapability ? ['--capability-audit'] : []),
          ...(isPresent ? ['--seconds', `${spec.seconds}`] : [])],
        environment: smokeEnvironment,
        /* DXVK lives in the WineHua runtime overlay, so the smoke process
         * still runs from its ordinary managed application directory. */
        workingDirectory: `C:\\smoke\\${spec.architecture}`,
        prefixMode: this.request.prefixMode,
        d3dBackend: isDxvk ? (isDxvkModern ? 'dxvk_modern_2_6' : 'dxvk_legacy') : 'wined3d',
        presentBackend: isPresent ? 'venus_broker_present' : 'venus_offscreen',
        automationMode: true
      }) as ProcessInfo;
    } else {
      process = testNapi.runWineProgram({
        windowsExePath: `C:\\smoke\\${spec.architecture}\\${spec.executable}`,
        argv: ['--automation', '--run-id', this.request.runId, '--test-id', spec.id,
          '--result', windowsResult, '--seconds', `${spec.seconds}`, '--present',
          ...(isD3d9 ? ['--d3d9'] : [])],
        environment: {
          'WINEHUA_SMOKE_RUN_ID': this.request.runId,
          'WINEHUA_SMOKE_TEST_ID': spec.id,
          'WINEDEBUG': isD3d8 ? '+loaddll,+d3d8' : (isD3d9 ? '+loaddll,+d3d9' : '-all'),
          /* Guest and Host map different shadow files. Functional probes use
           * explicit flush/invalidate for their tiny readback allocations. */
          'VN_WINEHUA_REMOTE_MEMORY_SYNC': '1'
        },
        workingDirectory: `C:\\smoke\\${spec.architecture}`,
        prefixMode: this.request.prefixMode,
        d3dBackend: 'wined3d',
        presentBackend: 'virgl_compositor',
        automationMode: true
      }) as ProcessInfo;
    }
    if (!process.found || process.pid <= 0)
      return this.failureResult(spec, 'startup', 'runWineProgram failed to return a PID', process);
    return this.waitForFinal(spec, resultPath, process);
  }

  async run(): Promise<void> {
    resetAutomationLogs();
    const stableDirectory = `${AUTOMATION_RESULTS}/${this.request.runId}`;
    ensureDirectory(stableDirectory);
    ensureDirectory(this.resultDirectory);
    writeAtomic(`${stableDirectory}/request.json`, JSON.stringify(this.request, null, 2));

    if (!syncManagedSmoke(this.request.prefixMode)) {
      const summary: SmokeSuiteSummary = {
        schemaVersion: 1, runId: this.request.runId, suite: this.request.suite,
        prefixMode: this.request.prefixMode, status: 'FAIL', stage: 'startup',
        message: 'managed C:\\smoke payload synchronization failed', tests: []
      };
      writeAtomic(`${stableDirectory}/suite-summary.json`, JSON.stringify(summary, null, 2));
      this.onStatus('Smoke: FAIL (sync)');
      return;
    }

      if ((this.request.suite === 'dxvk' || this.request.suite === 'gpu-diagnostics' ||
        this.request.suite === 'dxvk-long' ||
        this.request.suite === 'dxvk-replay' || this.request.suite === 'dxvk-layout-general' ||
        this.request.suite === 'dxvk-combined' || this.request.suite === 'all') &&
        !validateManagedDxvk('legacy')) {
      const summary: SmokeSuiteSummary = {
        schemaVersion: 1, runId: this.request.runId, suite: this.request.suite,
        prefixMode: this.request.prefixMode, status: 'FAIL', stage: 'startup',
        message: 'managed DXVK payload validation failed', tests: []
      };
      writeAtomic(`${stableDirectory}/suite-summary.json`, JSON.stringify(summary, null, 2));
      this.onStatus('Smoke: FAIL (DXVK sync)');
      return;
    }
    if ((this.request.suite === 'dxvk-modern-baseline' ||
      this.request.suite === 'dxvk-modern-long') &&
      !validateManagedDxvk('modern-2.6')) {
      const summary: SmokeSuiteSummary = {
        schemaVersion: 1, runId: this.request.runId, suite: this.request.suite,
        prefixMode: this.request.prefixMode, status: 'FAIL', stage: 'startup',
        message: 'managed DXVK Modern payload validation failed', tests: []
      };
      writeAtomic(`${stableDirectory}/suite-summary.json`, JSON.stringify(summary, null, 2));
      this.onStatus('Smoke: FAIL (DXVK Modern sync)');
      return;
    }

    const specs = this.specs();
    if (specs.length === 0) {
      const summary: SmokeSuiteSummary = {
        schemaVersion: 1, runId: this.request.runId, suite: this.request.suite,
        prefixMode: this.request.prefixMode, status: 'UNSUPPORTED', stage: 'startup',
        message: 'suite is not enabled by the phase2-vulkan-b3-v1 manifest', tests: []
      };
      const text = JSON.stringify(summary, null, 2);
      writeAtomic(`${stableDirectory}/suite-summary.json`, text);
      writeAtomic(`${this.resultDirectory}/suite-summary.json`, text);
      this.onStatus('Smoke: UNSUPPORTED');
      return;
    }

    const results: SmokeSuiteTestResult[] = [];
    for (const spec of specs) {
      const result = await this.runOne(spec);
      results.push(result);
      const diagnostic = (result.message || 'no diagnostic').replaceAll('\n', ' ')
        .replaceAll('\r', ' ').substring(0, 512);
      hilog.info(DOMAIN, "SmokeRunner",
        "result run=%{public}s test=%{public}s status=%{public}s stage=%{public}s message=%{public}s",
        this.request.runId, result.testId, result.status, result.stage, diagnostic);
    }
    const passed = results.every((result) => result.status === 'PASS');
    const summary: SmokeSuiteSummary = {
      schemaVersion: 1,
      runId: this.request.runId,
      suite: this.request.suite,
      prefixMode: this.request.prefixMode,
      status: passed ? 'PASS' : 'FAIL',
      completedTimestampMs: Date.now(),
      tests: results
    };
    const text = JSON.stringify(summary, null, 2);
    writeAtomic(`${stableDirectory}/suite-summary.json`, text);
    writeAtomic(`${this.resultDirectory}/suite-summary.json`, text);
    hilog.info(DOMAIN, "SmokeRunner",
      "summary run=%{public}s suite=%{public}s status=%{public}s tests=%{public}d",
      this.request.runId, this.request.suite, summary.status, results.length);
    this.onStatus(passed ? 'Smoke: PASS' : 'Smoke: FAIL');
  }
}
