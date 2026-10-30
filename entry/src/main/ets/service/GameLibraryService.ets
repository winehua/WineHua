import { common } from '@kit.AbilityKit';
import { preferences } from '@kit.ArkData';
import { fileIo as fs } from '@kit.CoreFileKit';
import { deviceInfo } from '@kit.BasicServicesKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import testNapi from 'libentry.so';
import { GameEntry, GameProfile, GameLaunchResult, buildGameEntries, defaultGameProfile,
  normalizeGameProfile, relativeGamePath, parseGameArguments, parseGameEnvironment } from '../common/GameLibraryModel';
import { DEFAULT_D3D_BACKEND } from '../common/GraphicsDefaults';
import { wineProgramOptions } from '../common/WineProgramOptions';
import { box64DynarecEnvString } from './Box64Dynarec';
import { DRIVE_C, WineEnvService } from './WineEnvService';
import { WineWindowManager } from './WineWindowManager';

interface ScanDirectory { absolutePath: string; relativePath: string; }

export class GameLibraryService {
  private static instance: GameLibraryService | null = null;
  static getInstance(): GameLibraryService {
    if (!GameLibraryService.instance) GameLibraryService.instance = new GameLibraryService();
    return GameLibraryService.instance;
  }

  entries: GameEntry[] = [];
  root: string = '';
  scanning: boolean = false;
  launchingId: string = '';
  message: string = '';
  scanWarnings: number = 0;
  private ctx: common.Context | null = null;
  private store: preferences.Preferences | null = null;
  private profiles: Map<string, GameProfile> = new Map();
  private generation: number = 0;
  private listeners: Array<() => void> = [];

  init(ctx: common.Context): void {
    this.ctx = ctx;
    if (this.store) return;
    try {
      this.store = preferences.getPreferencesSync(ctx, { name: 'game_library' });
      const saved = JSON.parse(this.store.getSync('profiles.v1', '[]') as string) as GameProfile[];
      if (Array.isArray(saved)) {
        for (const profile of saved) {
          if (profile && typeof profile.id === 'string') this.profiles.set(profile.id, profile);
        }
      }
    } catch (err) {
      this.message = '读取游戏设置失败；已有游戏文件不会改动';
      hilog.warn(0, 'GameLibrary', 'Load profiles: %{public}s', JSON.stringify(err));
    }
  }

  subscribe(listener: () => void): void {
    if (this.listeners.indexOf(listener) < 0) this.listeners.push(listener);
  }
  unsubscribe(listener: () => void): void {
    const i = this.listeners.indexOf(listener);
    if (i >= 0) this.listeners.splice(i, 1);
  }
  private notify(): void { this.listeners.forEach((listener: () => void) => listener()); }

  cancelScan(): void {
    this.generation++;
    this.scanning = false;
    this.notify();
  }

  async refresh(): Promise<void> {
    const generation = ++this.generation;
    this.scanning = true;
    this.scanWarnings = 0;
    this.message = '正在扫描 games…';
    this.notify();
    try {
      if (!this.ctx) throw new Error('请从主页打开游戏库');
      const root = await WineEnvService.getInstance().getGamesDirectory(this.ctx);
      if (generation !== this.generation) return;
      this.root = root.replace(/\/$/, '');
      const queue: ScanDirectory[] = [{ absolutePath: this.root, relativePath: '' }];
      const files: string[] = [];
      const folders: string[] = [];
      let visited = 0;
      while (queue.length) {
        if (generation !== this.generation) return;
        const current = queue.pop() as ScanDirectory;
        let names: string[];
        try { names = await fs.listFile(current.absolutePath); }
        catch (err) {
          if (current.relativePath === '') throw new Error('无法读取 games 目录，请检查授权');
          this.scanWarnings++; continue;
        }
        if (generation !== this.generation) return;
        for (const name of names) {
          if (!name || name === '.' || name === '..' || name.indexOf('/') >= 0 || name.startsWith('.')) continue;
          const relative = current.relativePath ? `${current.relativePath}/${name}` : name;
          const absolute = `${current.absolutePath}/${name}`;
          try {
            const stat = fs.lstatSync(absolute);
            if (stat.isSymbolicLink()) continue; // Do not follow cycles or leave the games tree.
            if (stat.isDirectory()) {
              if (!current.relativePath) folders.push(name);
              queue.push({ absolutePath: absolute, relativePath: relative });
            } else if (stat.isFile() && /\.exe$/i.test(name)) files.push(relative);
          } catch (_) { this.scanWarnings++; }
        }
        if (++visited % 24 === 0) {
          this.message = `已扫描 ${visited} 个目录，发现 ${files.length} 个启动程序…`;
          this.notify();
          await new Promise<void>((resolve) => setTimeout(resolve, 0));
        }
      }
      this.entries = buildGameEntries(files, folders);
      this.message = `共 ${this.entries.length} 个游戏目录 / ${files.length} 个可执行文件` +
        (this.scanWarnings ? `，${this.scanWarnings} 项无法读取` : '');
    } catch (err) {
      if (generation !== this.generation) return;
      this.message = err instanceof Error ? err.message : '扫描失败，请检查游戏目录';
      hilog.error(0, 'GameLibrary', 'Scan: %{public}s', JSON.stringify(err));
    } finally {
      if (generation === this.generation) { this.scanning = false; this.notify(); }
    }
  }

  getProfile(game: GameEntry): GameProfile {
    const saved = this.profiles.get(game.id);
    if (saved) {
      try { return normalizeGameProfile(saved, game); }
      catch (err) { hilog.warn(0, 'GameLibrary', 'Invalid stored profile: %{public}s', JSON.stringify(err)); }
    }
    return defaultGameProfile(game);
  }

  saveProfile(game: GameEntry, profile: GameProfile): GameProfile {
    const normalized = normalizeGameProfile(profile, game);
    if (normalized.executable && game.executables.indexOf(normalized.executable) < 0)
      throw new Error('主程序已不存在，请从候选列表重新选择');
    parseGameArguments(normalized.argumentsText);
    parseGameEnvironment(normalized.environmentText);
    if (!this.store) throw new Error('游戏设置存储不可用，未保存');
    const next = new Map(this.profiles);
    next.set(game.id, normalized);
    this.store.putSync('profiles.v1', JSON.stringify(Array.from(next.values())));
    this.store.flushSync();
    this.profiles = next;
    this.message = `已保存「${normalized.name}」的设置`;
    this.notify();
    return normalized;
  }

  private async waitUntilReady(): Promise<boolean> {
    const env = WineEnvService.getInstance();
    const deadline = Date.now() + 300000;
    while (Date.now() < deadline) {
      if (env.engineState === 'ready') return true;
      if (env.initFailed || env.engineState === 'failed' || env.upgradePending) return false;
      if (env.engineState === 'idle' && !env.initBusy && !env.opBusy) return false;
      await new Promise<void>((resolve) => setTimeout(resolve, 150));
    }
    return false;
  }

  private gameLaunchRoot(): string {
    // Reuse the file browser's C:\games identity only when it refers to the
    // indexed directory. A different or stale C: folder must never redirect a
    // library entry to another copy of a game (and its settings/save files).
    const alias = `${DRIVE_C}/games`;
    try {
      const source = fs.statSync(this.root);
      const candidate = fs.statSync(alias);
      if (source.ino > 0 && source.ino === candidate.ino && candidate.isDirectory()) return alias;
    } catch (_) {}
    return this.root;
  }

  async launch(game: GameEntry, allowRestart: boolean = false): Promise<GameLaunchResult> {
    if (this.scanning) return { accepted: false, requiresRestart: false, message: '正在扫描游戏目录，请稍后再试', pid: 0 };
    if (this.launchingId) return { accepted: false, requiresRestart: false, message: '另一个游戏正在启动', pid: 0 };
    this.launchingId = game.id;
    this.notify();
    const env = WineEnvService.getInstance();
    const wm = WineWindowManager.getInstance();
    try {
      const profile = this.getProfile(game);
      if (!profile.executable || game.executables.indexOf(profile.executable) < 0)
        throw new Error('请先在设置中选择游戏主程序');
      const relativeExe = relativeGamePath(profile.executable);
      const executable = `${this.root}/${relativeExe}`;
      if (!fs.lstatSync(executable).isFile()) throw new Error('游戏主程序不可访问，请刷新游戏库');
      const slash = profile.executable.lastIndexOf('/');
      const relativeWork = profile.workingDirectory || (slash >= 0 ? profile.executable.substring(0, slash) : '');
      const workingDirectory = relativeWork ? `${this.root}/${relativeGamePath(relativeWork)}` : this.root;
      if (!fs.statSync(workingDirectory).isDirectory()) throw new Error('工作目录不存在');
      const argv = parseGameArguments(profile.argumentsText);
      const environment: Record<string, string> = {};
      for (const line of box64DynarecEnvString(profile.compatMode).split(';')) {
        const at = line.indexOf('=');
        if (at > 0) environment[line.substring(0, at)] = line.substring(at + 1);
      }
      const overrides = parseGameEnvironment(profile.environmentText);
      for (const key of Object.keys(overrides)) environment[key] = overrides[key];
      const selectedResolution = profile.resolution === 'inherit' ?
        (AppStorage.get<string>('winehua.phone.desktop.preference') || 'auto') : profile.resolution;
      if (env.engineState === 'stopping') throw new Error('引擎正在停止，请稍后再试');
      if (env.isBusy() || env.engineState === 'ready-degraded') {
        this.message = `正在等待引擎准备完成：${profile.name}`;
        this.notify();
        if (!await this.waitUntilReady()) throw new Error('引擎准备未完成，请返回主页查看状态');
      }
      const mismatch = deviceInfo.deviceType === 'phone' && env.engineState === 'ready' &&
        wm.hasFixedDesktopResolution() !== (selectedResolution === '1024x768');
      if (mismatch && !allowRestart) return { accepted: false, requiresRestart: true,
        message: '该游戏的桌面分辨率与当前会话不同。请保存其他程序后，重启引擎并启动。', pid: 0 };
      this.message = `正在为「${profile.name}」准备引擎…`;
      this.notify();
      let ready = false;
      if (mismatch) {
        wm.setNextDesktopResolution(selectedResolution);
        env.restartEngine();
        ready = await this.waitUntilReady();
      } else {
        if (env.engineState !== 'ready') wm.setNextDesktopResolution(selectedResolution);
        ready = await env.ensureEngineReady();
      }
      if (!ready) throw new Error('引擎尚未就绪，请返回主页查看状态');
      if (env.desktopMode) {
        if (!this.ctx) throw new Error('游戏库窗口已关闭，请重新打开');
        try { await wm.showDesktopForCaller(this.ctx as common.UIAbilityContext); }
        catch (err) { throw new Error('无法打开游戏画面：' + JSON.stringify(err)); }
      }
      // The existing readiness policy distinguishes manual phone desktops
      // from managed/tablet windows. Do not add another surface gate here.
      if (!await env.ensureDesktop()) throw new Error('桌面仍在准备，请返回主页查看状态');
      const backend = profile.d3dBackend === 'inherit' ?
        (AppStorage.get<string>('winehua.d3d.backend') || DEFAULT_D3D_BACKEND) : profile.d3dBackend;
      const launchRoot = this.gameLaunchRoot();
      const launchExe = `${launchRoot}/${relativeExe}`;
      const launchDirectory = relativeWork ? `${launchRoot}/${relativeGamePath(relativeWork)}` : launchRoot;
      const options = wineProgramOptions(launchExe, launchDirectory, backend,
        AppStorage.get<string>('winehua.dxvk.backend') || 'dxvk_legacy', argv, environment, profile.openglRenderer);
      hilog.info(0, 'GameLibrary', 'Launch exe=%{public}s cwd=%{public}s d3d=%{public}s dxvk=%{public}s cpu=%{public}s envKeys=%{public}s resolution=%{public}s',
        launchExe, launchDirectory, options.d3dBackend, options.dxvkBackend,
        profile.compatMode || 'native-default', Object.keys(environment).join(','), selectedResolution);
      const handle = testNapi.runWineProgram(options);
      if (handle.pid <= 0) throw new Error('Wine 未能受理启动请求，请检查主程序及后端设置');
      profile.lastLaunchedAt = Date.now();
      try { this.saveProfile(game, profile); }
      catch (err) { hilog.warn(0, 'GameLibrary', 'Save launch time: %{public}s', JSON.stringify(err)); }
      this.message = `已提交「${profile.name}」的启动请求（PID ${handle.pid}）`;
      return { accepted: true, requiresRestart: false, message: this.message, pid: handle.pid };
    } catch (err) {
      wm.setNextDesktopResolution('');
      hilog.error(0, 'GameLibrary', 'Launch failure: %{public}s', JSON.stringify(err));
      this.message = err instanceof Error ? err.message : '启动失败，请检查游戏设置';
      return { accepted: false, requiresRestart: false, message: this.message, pid: 0 };
    } finally {
      this.launchingId = '';
      this.notify();
    }
  }
}
