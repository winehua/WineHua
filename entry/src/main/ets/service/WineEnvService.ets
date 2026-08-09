import { hilog } from '@kit.PerformanceAnalysisKit';
import { common } from '@kit.AbilityKit';
import { preferences } from '@kit.ArkData';
import testNapi from 'libentry.so';
import { fileIo as fs, fileUri, picker } from '@kit.CoreFileKit';
import { zlib } from '@kit.BasicServicesKit';
import { ProcessService, ProcessInfo } from './ProcessService';
import { WineWindowManager } from './WineWindowManager';
import { SmokeLaunchRequest, SmokeRunner, defaultSmokeRequest, parseSmokeRequest,
  syncManagedSmoke } from './SmokeRunner';

const DOMAIN = 0x0000;
const FILES_BASE = '/data/storage/el2/base/files';
// doInit 与页面 doRun 连同一个 wineserver socket (reuse 模式;
// 自动化 clean 模式使用 this.sockPath, 见 updateLaunchRequest)
export const SOCK = '/data/storage/el2/base/files/.wine/wine-wayland';

// Wine 运行时从 rawfile 解压到 files/
const WINE_ROOT = `${FILES_BASE}/wine`;
export const WINE_BIN = `${WINE_ROOT}/bin`;
export const WINE_LIB = `${WINE_BIN}/x86_64-unix`;
const BIN_DIR = WINE_BIN;
const BOX64 = `${WINE_BIN}/box64`;

// 页面文件浏览器以 C: 盘为根
export const DRIVE_C = '/data/storage/el2/base/files/.wine/drive_c';
const WINE_SMOKE_PREFIX = `${FILES_BASE}/.wine-smoke`;
// 自动化模式的受管 HOME (Z: 映射目标): 沙箱内路径, 永不弹 picker
const WINE_AUTOMATION_HOME = `${FILES_BASE}/home`;

export function pathExists(path: string): boolean {
  // accessSync 是布尔语义: 文件不存在返回 false, 只有权限/参数类错误才抛
  // 异常 — 不能写成 "try 住就算存在" (那样缺失文件会被误判为存在)
  try {
    return fs.accessSync(path);
  } catch (_) {
    return false;
  }
}

export function ensureDirectory(path: string): boolean {
  try {
    if (!pathExists(path)) {
      fs.mkdirSync(path, true);
    }
    return pathExists(path);
  } catch (e) {
    hilog.warn(DOMAIN, 'wine', 'mkdir failed path=%{public}s err=%{public}s', path, JSON.stringify(e));
    return false;
  }
}

export function readTextIfExists(path: string): string {
  try {
    if (!pathExists(path)) return '';
    const file = fs.openSync(path, fs.OpenMode.READ_ONLY);
    try {
      const buf = new ArrayBuffer(65536);
      const count = fs.readSync(file.fd, buf);
      const bytes = new Uint8Array(buf, 0, count);
      let text = '';
      for (let i = 0; i < bytes.length; ++i) text += String.fromCharCode(bytes[i]);
      return text.trim();
    } finally {
      fs.closeSync(file);
    }
  } catch (_) {
    return '';
  }
}

export function writeTextFile(path: string, text: string): void {
  const file = fs.openSync(path, fs.OpenMode.CREATE | fs.OpenMode.WRITE_ONLY | fs.OpenMode.TRUNC);
  try {
    fs.writeSync(file.fd, text);
  } finally {
    fs.closeSync(file);
  }
}

export function removePathRecursive(path: string): void {
  const st = fs.statSync(path);
  if (st.isSymbolicLink() || !st.isDirectory()) {
    fs.unlinkSync(path);
    return;
  }
  const names = fs.listFileSync(path);
  for (const name of names) {
    removePathRecursive(`${path}/${name}`);
  }
  fs.rmdirSync(path);
}

// Wine 环境生命周期服务: 检测/解压 wine 数据、启动/重置 wineserver、跟踪 prefix 状态。
// 持有引擎状态机 (engineState, 仅 native state:* 消息迁移) 与会话存活派生
// (sessionAlive = 桌面 root || 进程在跑), UI 的状态文案与按钮使能全部以此为准;
// native evt:* 瞬时事件经 subscribeEvents 转发 (ProcessService/启动反馈),
// 事件除 desktop-ready 补票升级 ready-degraded 外永不迁移状态机。
// 自动化 (smoke/game) 启动参数来自 EntryAbility.publishLaunchRequest
// 写入的 AppStorage, 在 init 时读取并参数化 doInit (免 picker 的 Z: home、clean prefix、
// D3D 后端等)。
export class WineEnvService {
  private static instance: WineEnvService | null = null;

  static getInstance(): WineEnvService {
    if (!WineEnvService.instance) {
      WineEnvService.instance = new WineEnvService();
    }
    return WineEnvService.instance;
  }

  // -- 页面镜像的状态 (任何变化后 notify) --
  // 引擎持久状态机: 仅被 native state:* 消息迁移 (idle/starting/ready/stopping/failed)。
  // evt:* 瞬时事件 (进程进出/启动受理) 永不触碰它 — 历史 bug: 事件写进状态字段,
  // process-updated 到达后 state != 'wine-running', 运行中的程序旁重启/重置按钮意外解锁
  engineState: string = 'idle';
  // engineState 细节: starting → wineserver/wineboot; failed → 失败阶段
  engineStage: string = '';
  prefixReady: boolean = false;
  statusText: string = '检测中...';
  initOverlayText: string = '';
  initOverlayVisible: boolean = false;
  // 初始为空串 = "未设置"；唯一写入入口 acquireZHome (doInit 开头调用),
  // 重跑 doInit 时复用已设置的 / 结尾真实路径。切勿给非空默认值 —
  // 会导致 !zHome 永假、picker 被跳过、gamesDir 拼出 .../Downloadgames 错误路径
  zHome: string = '';
  // 初始化失败标记: doInit 各失败分支与 state:failed 置位, 首启引导据此显示
  // 重试/重置 入口 (仅靠 statusText 一行文字用户无法自救); doInit 开始 / state:ready 时清除
  initFailed: boolean = false;
  // 会话存活派生: 桌面 root 存在 || 用户进程在跑 (镜像自 WineWindowManager /
  // ProcessService, 见 init 的订阅)。按钮使能判据, 不再拿 state 串当代理
  sessionAlive: boolean = false;
  desktopActive: boolean = false;
  // 桌面 root 的 renderer surface 校准完成（WineWindowManager 镜像）—
  // ensureDesktop 放行程序的判据: root 出现 ≠ 桌面真正显示
  desktopSurfaceReady: boolean = false;
  // 桌面模式 (Tablet/Phone compositor shell, WineWindowManager 镜像)。
  // PC 窗口模式 (managed) 无桌面壳, 程序独立窗口 — ensureDesktop 只在桌面模式下
  // 有意义, managed 模式直接放行 (旧实现拉起的只是文件管理器窗口, 且等不到
  // desktop_root/desktopSurfaceReady 判据, 25s 超时误报"桌面启动失败")
  desktopMode: boolean = false;
  runningCount: number = 0;
  // doInit 防重入: 覆盖 picker→解压→launchClient 全异步段 (首启解压可达数分钟),
  // 由 state:ready / state:failed / failInit 清除
  initBusy: boolean = false;
  // 升级待处理标记: 启动检测到已解压运行时与 HAP 打包不一致 (升级) 时置位,
  // 不自动解压 (静默换数据会与旧 prefix 不兼容), 由 UI 卡片问用户是否应用。
  // 用户选择应用 → doReset 完整恢复出厂 (重装数据+清 C 盘); 稍后 → dismiss。
  // 解压成功后自动清除 (数据已最新)
  upgradePending: boolean = false;
  // 已跳过提醒的升级 sha (preferences 持久化): 点"稍后"记录, 绑定具体版本 —
  // 同一版本不再重复提醒 (每次启动都弹很烦), app 又更新 (sha 变) 时重新提醒
  private skippedUpgradeSha: string = '';
  // 重启/重置/停止编排进行中 (stopAll → 等 state:stopped → 后续动作), 期间禁止一切会话操作
  opBusy: boolean = false;
  // 后台自动启动 (二次进入 app 且 prefix 已就绪时, init 自动拉起引擎):
  // 抑制 init overlay 模态弹窗 — 进度只经 statusText/EngineGuide 呈现,
  // 失败也不弹 overlay, 由 EngineGuide 的 failed 分支承接自救 (重试/重置)。
  // 仅 init 的自动拉起置位, enterReady/failInit/ready-degraded 时清除
  private bgStart: boolean = false;

  private hasRawfileWineData: boolean = false;
  private ctx: common.Context | null = null;
  private initialized: boolean = false;
  private listeners: Array<() => void> = [];
  // 事件订阅 (evt:* 瞬时消息, 参数为去掉 'evt:' 前缀的事件体, 如 'proc-exited:1234')
  private eventListeners: Array<(evt: string) => void> = [];
  // 等待 state:stopped 的一次性回调 (重启/重置/停止编排), 由 handleEngineState 兑现
  private stopWaiters: Array<() => void> = [];

  // -- 自动化启动参数 (EntryAbility want → AppStorage → init 读取) --
  private smokeRequest: SmokeLaunchRequest = defaultSmokeRequest();
  private automationMode: boolean = false;
  private gameTestMode: boolean = false;
  private prefixMode: string = 'reuse';
  private prefixRoot: string = `${FILES_BASE}/.wine`;
  private driveC: string = DRIVE_C;
  private sockPath: string = SOCK;
  private d3dBackend: string = 'dxvk_legacy';
  // Wine locale 语言 ("zh_CN"/"en_US"), 设置页可切, 重启引擎后生效。
  // 持久化走 preferences API (同 DisplaySettingsService) — 不用 PersistentStorage:
  // 实测本应用 persistProp 在冷启动时不从磁盘恢复值 (haps/entry/files/
  // persistent_storage 明明有值仍返回默认值), 原因未查明, 绕开
  private wineLang: string = 'zh_CN';
  private settingsStore: preferences.Preferences | null = null;
  private perfProfile: string = 'baseline';
  private smokeStarted: boolean = false;
  private cleanPrefixPrepared: boolean = false;
  private gameStarted: boolean = false;
  // game 模式参数 (winehua.game.*)
  private gamePath: string = '';
  private gameArguments: string[] = [];
  private d3dEnvKeys: string[] = [];
  private d3dEnvValues: string[] = [];
  private gameClickTitlePrefix: string = '';
  private gameClickButtonText: string = '';
  private gameClickDelayMs: number = 1500;
  private gameClickClientXPermille: number = -1;
  private gameClickClientYPermille: number = -1;

  // 极简订阅: 状态变化后逐个调用
  subscribe(listener: () => void): void {
    this.listeners.push(listener);
  }

  // 退订: EngineGuide 这类随状态反复挂载/卸载的组件必须 aboutToDisappear
  // 退订, 否则每次进出引导页都往 listeners 里积一份闭包
  unsubscribe(listener: () => void): void {
    const i = this.listeners.indexOf(listener);
    if (i >= 0) {
      this.listeners.splice(i, 1);
    }
  }

  // 事件订阅: native evt:* 瞬时消息 (进程进出/启动受理/启动失败)
  subscribeEvents(listener: (evt: string) => void): void {
    this.eventListeners.push(listener);
  }

  private emitEvent(evt: string): void {
    for (const listener of this.eventListeners) {
      listener(evt);
    }
  }

  private notify(): void {
    // 派生状态集中在这里重算, 保证任何镜像读到的都是最新值
    this.sessionAlive = this.desktopActive || this.runningCount > 0;
    this.recomputeStatus();
    for (const listener of this.listeners) {
      listener();
    }
  }

  // 引擎状态文案派生: 单一事实来源是 engineState + sessionAlive, 瞬时操作回执
  // 不再写 statusText (历史问题: "已停止全部会话" 等回执永久驻留标题栏)。
  // 自动化/游戏模式的进度文案由 SmokeRunner/game 自管, 不覆盖
  private recomputeStatus(): void {
    if (this.automationMode || this.gameTestMode) {
      return;
    }
    if (this.engineState == 'starting' || this.initBusy) {
      this.statusText = this.initOverlayText || '正在初始化...';
    } else if (this.engineState == 'ready') {
      this.statusText = this.sessionAlive ? '运行中' : '已就绪';
    } else if (this.engineState == 'ready-degraded') {
      this.statusText = '桌面准备中...';
    } else if (this.engineState == 'stopping') {
      this.statusText = '正在停止...';
    } else if (this.engineState == 'failed') {
      this.statusText = '启动失败';
    } else {
      // idle (含懒初始化后未启动): prefix 就绪只说明初始化过, 引擎并未在跑
      this.statusText = this.prefixReady ? '未启动' : '未初始化';
    }
  }

  // 按钮使能判据 (设置页 / 应用库 / 文件页统一走这两个, 不再各自比较 state 串)
  isBusy(): boolean {
    return this.initBusy || this.opBusy || this.engineState == 'starting' ||
      this.engineState == 'stopping';
  }

  canLaunch(): boolean {
    return this.engineState == 'ready' && !this.isBusy();
  }

  // 环境页渲染策略/开发者分区展示当前生效值 (updateLaunchRequest 写入)
  getD3dBackend(): string { return this.d3dBackend; }
  getWineLang(): string { return this.wineLang; }
  getPerfProfile(): string { return this.perfProfile; }
  isAutomation(): boolean { return this.automationMode; }

  // 设置页"Wine 语言"切换: 持久化 (preferences API, putSync+flush 直写磁盘)。
  // 仅写入配置 — 已在运行的 Wine 进程不受影响, 重启引擎后生效 (UI 负责提示)
  setWineLangPreference(lang: string): void {
    const selected = lang === 'en_US' ? 'en_US' : 'zh_CN';
    this.wineLang = selected;
    if (this.settingsStore) {
      try {
        this.settingsStore.putSync('lang', selected);
        this.settingsStore.flush();
      } catch (e) {
        hilog.error(DOMAIN, 'wine', 'lang persist fail: %{public}s', JSON.stringify(e));
      }
    }
    hilog.info(DOMAIN, 'wine', 'Wine language selected=%{public}s', selected);
    this.notify();
  }

  // 环境页"渲染策略"切换: 持久化偏好 + 同步 AppStorage 生效值 + 重读启动参数。
  // 仅写入配置 — 已在运行的 Wine 不受影响, 重启引擎后生效 (UI 负责提示)
  setD3dPreference(backend: string): void {
    const selected = backend === 'dxvk_legacy' ? 'dxvk_legacy' : 'wined3d';
    PersistentStorage.persistProp<string>('winehua.d3d.preference', selected);
    AppStorage.set<string>('winehua.d3d.preference', selected);
    AppStorage.set<string>('winehua.d3d.backend', selected);
    this.updateLaunchRequest();
    hilog.info(DOMAIN, 'GameTest', 'D3D backend selected=%{public}s', selected);
    this.notify();
  }

  // 读取 EntryAbility 发布到 AppStorage 的启动请求 (mode=smoke/game/'')
  private updateLaunchRequest(): void {
    this.smokeRequest = parseSmokeRequest(AppStorage.get<string>('winehua.smoke.request') ?? '');
    this.automationMode = this.smokeRequest.mode === 'smoke';
    this.gameTestMode = this.smokeRequest.mode === 'game';
    this.prefixMode = this.smokeRequest.prefixMode;
    this.prefixRoot = this.prefixMode === 'clean' ? WINE_SMOKE_PREFIX : `${FILES_BASE}/.wine`;
    this.driveC = `${this.prefixRoot}/drive_c`;
    this.sockPath = `${this.prefixRoot}/wine-wayland`;
    const backend = AppStorage.get<string>('winehua.d3d.backend') ?? 'dxvk_legacy';
    this.d3dBackend = backend === 'dxvk_legacy' ? 'dxvk_legacy' : 'wined3d';
    this.perfProfile = AppStorage.get<string>('winehua.perf.profile') ?? 'baseline';
    this.gamePath = AppStorage.get<string>('winehua.game.path') ?? '';
    this.gameArguments = AppStorage.get<string[]>('winehua.game.args') ?? [];
    this.d3dEnvKeys = AppStorage.get<string[]>('winehua.game.d3d_env_keys') ?? [];
    this.d3dEnvValues = AppStorage.get<string[]>('winehua.game.d3d_env_values') ?? [];
    this.gameClickTitlePrefix = AppStorage.get<string>('winehua.game.click_title_prefix') ?? '';
    this.gameClickButtonText = AppStorage.get<string>('winehua.game.click_button_text') ?? '';
    this.gameClickDelayMs = AppStorage.get<number>('winehua.game.click_delay_ms') ?? 1500;
    this.gameClickClientXPermille = AppStorage.get<number>('winehua.game.click_client_x_permille') ?? -1;
    this.gameClickClientYPermille = AppStorage.get<number>('winehua.game.click_client_y_permille') ?? -1;
  }

  // 原 aboutToAppear 的初始化流程; 幂等, 重复调用直接返回 (native 回调只注册一次)
  init(ctx: common.Context): void {
    if (this.initialized) {
      return;
    }
    this.initialized = true;
    this.ctx = ctx;
    // 语言偏好: preferences API 直读 (同 DisplaySettingsService 模式), 缺省中文
    try {
      this.settingsStore = preferences.getPreferencesSync(ctx, { name: 'wine_settings' });
      const savedLang = this.settingsStore.getSync('lang', 'zh_CN') as string;
      this.wineLang = savedLang === 'en_US' ? 'en_US' : 'zh_CN';
      this.skippedUpgradeSha = this.settingsStore.getSync('skipped_upgrade_sha', '') as string;
    } catch (e) {
      hilog.error(DOMAIN, 'wine', 'lang settings load fail: %{public}s', JSON.stringify(e));
    }
    this.updateLaunchRequest();

    this.hasRawfileWineData = this.detectRawfileWineData();

    testNapi.setStateCallback(this.onNativeState);
    this.refreshPrefix();
    // 会话存活镜像: 进程列表 / 桌面 root 变化 → notify 内重算 sessionAlive。
    // 这两项是"运行中/已就绪"与按钮使能的判据来源
    const proc = ProcessService.getInstance();
    proc.subscribe(() => {
      this.runningCount = proc.processList.filter((p: ProcessInfo) => p.state === 'running').length;
      this.notify();
    });
    const wm = WineWindowManager.getInstance();
    wm.subscribe(() => {
      this.desktopActive = wm.desktopActive;
      this.desktopSurfaceReady = wm.desktopSurfaceReady;
      this.desktopMode = wm.isDesktopMode;
      this.notify();
    });
    // 自动初始化分三种情形:
    // 1) 自动化/游戏测试模式 (want 驱动无人值守): 无条件立即 doInit;
    // 2) 普通模式且 prefix 已就绪 (二次进入): 后台自动拉起引擎 (bgStart
    //    抑制 overlay, 用户无感), 就绪前点 exe 由 UI 侧 toast 拦截;
    // 3) 普通模式无 prefix (首启): 懒初始化 — 首启解压 249MB 可达数分钟,
    //    由 EngineGuide 引导用户显式启动, 前台 overlay 阻塞至 prefix 就绪
    if (this.automationMode || this.gameTestMode) {
      setTimeout(() => this.doInit(), 0);
    } else if (this.prefixReady) {
      // 升级检测提前到自动拉起之前: 数据与 HAP 不一致时不拉起引擎 —
      // 静默换数据会与旧 prefix 不兼容; 且引擎启动中选"立即应用"会撞
      // doReset 防重入 (并发操作运行时目录 = 数据损坏)。跳过拉起, 置
      // upgradePending 弹卡片, 由用户决定: 应用=完整重置 (引擎静止无竞态)
      // / 稍后=dismiss 补拉起继续用旧数据
      // 守卫 (与自动拉起同款防御): 幸存会话不可见 (sessionAlive 恒 false),
      // 此时弹卡→立即应用会删活引擎二进制 — 不弹卡, 旧会话继续跑;
      // wine.inf 缺失不算升级 (数据缺失走 doInit 自动解压, 不弹卡)
      if (!this.sessionAlive &&
          pathExists(`${WINE_ROOT}/share/wine/wine.inf`) &&
          this.upgradeNeedsAttention()) {
        this.upgradePending = true;
        this.notify();
        return;
      }
      setTimeout(() => {
        this.autoStartIfIdle();
      }, 0);
    }
  }

  // 自动化/游戏模式专用的状态文案直写 (SmokeRunner/game 自管进度文案;
  // 普通模式 statusText 由 recomputeStatus 派生, 直写会在下一次 notify 被覆盖)
  setStatusText(text: string): void {
    this.statusText = text;
    this.notify();
  }

  // native 消息入口: 按协议前缀分发 — state:* 迁移引擎状态机,
  // evt:* 作为瞬时事件转发给事件订阅方 (ProcessService 刷新 / 启动反馈);
  // 唯一例外: evt:desktop-ready 把 ready-degraded 补票升级为 ready (蓝图 §2.2)
  private onNativeState = (s: string): void => {
    hilog.info(DOMAIN, 'wine', 'state: %{public}s', s);
    if (s.startsWith('state:')) {
      this.handleEngineState(s.substring(6));
    } else if (s.startsWith('evt:')) {
      const body = s.substring(4);
      // "evt 永不迁移状态机"的唯一例外 (蓝图 §2.2): desktop-ready 把
      // ready-degraded 补票升级为正式 ready; 其余状态收到它是无害空转
      if (body == 'desktop-ready' && this.engineState == 'ready-degraded') {
        this.enterReady();
      }
      this.emitEvent(body);
    } else {
      hilog.warn(DOMAIN, 'wine', 'unknown engine message: %{public}s', s);
    }
    this.notify();
  }

  private handleEngineState(body: string): void {
    const sep = body.indexOf(':');
    const name = sep >= 0 ? body.substring(0, sep) : body;
    const detail = sep >= 0 ? body.substring(sep + 1) : '';
    if (name == 'starting') {
      this.engineState = 'starting';
      this.engineStage = detail;
      this.initFailed = false;
      // bgStart (后台自动拉起) 不弹模态 overlay, 进度只进 statusText
      this.setInitStatus('Wine 正在初始化...', !this.bgStart);
    } else if (name == 'ready') {
      this.enterReady();
    } else if (name == 'ready-degraded') {
      // 桌面根 15s 未就绪 (仅 desktop 模式): 引擎活着但桌面没起来 — 进行中
      // 而非就绪。canLaunch 不放行 (程序抢跑会死在 DXGI 初始化); root 出现后
      // native 补发 evt:desktop-ready 升级。注意 overlay 必须隐去而非留驻:
      // 模态 overlay 会挡住环境页重启按钮, 桌面一直没起来时用户将无自救入口
      // (进行中文案由 recomputeStatus 经 statusText 呈现)
      this.engineState = 'ready-degraded';
      this.engineStage = '';
      this.initFailed = false;
      this.initBusy = false;
      this.opBusy = false;
      this.bgStart = false;
      this.setInitStatus('桌面准备中...', false);
    } else if (name == 'failed') {
      // 覆盖启动失败 (wineserver spawn/wineboot) 与运行中主 wineserver 非预期
      // 死亡 (ProcMon 上报) — 失败统一进 failInit: overlay 给 重试/重置 入口,
      // 此前首启失败会永久停在初始化 spinner 无自救入口
      this.engineState = 'failed';
      this.engineStage = detail;
      this.failInit(`Wine 引擎启动失败 (${detail || 'unknown'})`);
    } else if (name == 'stopping') {
      this.engineState = 'stopping';
    } else if (name == 'stopped') {
      // native 停止编排完成 (注册表进程全死 zombie 感知 + wineserver 死 +
      // Wayland 停)。注意不在此清 opBusy: 重启/重置编排还有后续动作
      // (doInit/删 prefix), 由各编排自己在完成时清
      this.engineState = 'idle';
      this.engineStage = '';
      const waiters = this.stopWaiters.splice(0);
      for (const resolve of waiters) {
        resolve();
      }
    } else {
      hilog.warn(DOMAIN, 'wine', 'unknown engine state: %{public}s', body);
    }
  }

  // 进入正式 ready 的统一定式: state:ready 与 evt:desktop-ready (degraded
  // 补票升级) 共用 — 清 busy/失败标记, 隐去 overlay, 轮询等 drive_c 就绪
  private enterReady(): void {
    this.engineState = 'ready';
    this.engineStage = '';
    this.initFailed = false;
    this.initBusy = false;
    this.opBusy = false;
    this.bgStart = false;
    this.setInitStatus('已就绪', false);
    // wineboot 可能还在后台初始化 prefix，轮询等待 drive_c 就绪
    this.waitForPrefix();
  }

  private detectRawfileWineData(): boolean {
    const rm = this.ctx!.resourceManager;
    try {
      const rawFd = rm.getRawFdSync('wine-data.zip');
      hilog.info(DOMAIN, 'wine', 'rawfile: wine-data.zip exists len=%{public}d', rawFd.length);
      rm.closeRawFdSync('wine-data.zip');
      return true;
    } catch (_) {
      hilog.info(DOMAIN, 'wine', 'rawfile: wine-data.zip not packaged');
      return false;
    }
  }

  private readRawfileText(name: string): string {
    const rm = this.ctx!.resourceManager;
    try {
      const rawFd = rm.getRawFdSync(name);
      try {
        const data = new ArrayBuffer(rawFd.length);
        const count = fs.readSync(rawFd.fd, data, { offset: rawFd.offset, length: rawFd.length });
        const bytes = new Uint8Array(data, 0, count);
        let text = '';
        for (let i = 0; i < bytes.length; ++i) text += String.fromCharCode(bytes[i]);
        return text.trim();
      } finally {
        rm.closeRawFdSync(name);
      }
    } catch (_) {
      return '';
    }
  }

  // 打包 manifest 与已解压运行时不一致 → 需要刷新 (覆盖安装后升级 wine 数据)
  private runtimeNeedsRefresh(): boolean {
    if (!pathExists(`${WINE_ROOT}/share/wine/wine.inf`)) return true;
    const packaged = this.packagedManifestText();
    if (!packaged) return false;
    return packaged !== readTextIfExists(`${WINE_ROOT}/.winehua-runtime-manifest.json`);
  }

  // HAP 内打包的 manifest 文本 (会话内缓存: 启动路径被 runtimeNeedsRefresh/
  // packagedRuntimeSha/dismiss 多处读取, rawfile 是 HAP 常量本会话不可能
  // 变化, 一次读避免重复 getRawFdSync 开销)
  private cachedPackagedManifest: string = '';
  private manifestTextLoaded: boolean = false;
  private packagedManifestText(): string {
    if (!this.manifestTextLoaded) {
      this.cachedPackagedManifest = this.readRawfileText('wine-runtime-manifest.json');
      this.manifestTextLoaded = true;
    }
    return this.cachedPackagedManifest;
  }

  // HAP 内打包的运行时 sha (manifest payloadSha256), 用作跳过提醒的版本标识
  private packagedRuntimeSha(): string {
    const text = this.packagedManifestText();
    if (!text) return '';
    try {
      return (JSON.parse(text) as Record<string, string>)['payloadSha256'] ?? '';
    } catch (_) {
      return '';
    }
  }

  // 升级是否需要提醒: 运行时不一致, 且不是用户已跳过的同一版本。
  // sha 取不到时保守返回 true — 宁可重复提醒, 不可让升级静默失效
  private upgradeNeedsAttention(): boolean {
    if (!this.runtimeNeedsRefresh()) return false;
    const sha = this.packagedRuntimeSha();
    if (sha === '') return true;
    return sha !== this.skippedUpgradeSha;
  }

  private setInitStatus(text: string, visible: boolean): void {
    this.statusText = text;
    this.initOverlayText = text;
    this.initOverlayVisible = visible;
    this.notify();
  }

  // 自动拉起公共判据 (init 自动拉起 / 升级"稍后"补拉起共用一份):
  // sessionAlive 防御 — app 进程重启但 wineserver 会话仍在时, 不编排重启
  private autoStartIfIdle(): void {
    if (!this.isBusy() && this.engineState == 'idle' && !this.sessionAlive) {
      this.bgStart = true;
      this.restartEngine();
    }
  }

  // 升级提示卡"稍后": 记录跳过 sha (同一版本不再提醒, app 又更新时重新提醒);
  // 同时补上被升级检测跳过的自动拉起 — 用旧数据正常启动。
  // 顺序约定: 「立即应用」路径必须先 doReset (同步置 opBusy) 再调本方法,
  // 否则 autoStartIfIdle 会在 doReset 编排期间误补拉起
  dismissUpgradeNotice(): void {
    this.upgradePending = false;
    const sha = this.packagedRuntimeSha();
    if (sha !== '') {
      this.skippedUpgradeSha = sha;
      if (this.settingsStore) {
        try {
          this.settingsStore.putSync('skipped_upgrade_sha', sha);
          this.settingsStore.flush();
        } catch (e) {
          hilog.warn(DOMAIN, 'wine', 'skip upgrade sha persist fail: %{public}s', JSON.stringify(e));
        }
      }
    }
    this.autoStartIfIdle();
    this.notify();
  }

  // Pad: 从 rawfile 取出 wine-data.zip → zlib.decompressFile 解压到 files/wine/
  // 使用 getRawFdSync 流式读取，避免 249MB 一次加载到内存导致 OOM
  private async extractWineData(): Promise<void> {
    const marker = `${WINE_ROOT}/share/wine/wine.inf`;
    const packagedManifest = this.readRawfileText('wine-runtime-manifest.json');
    hilog.info(DOMAIN, 'wine', 'extract: check marker=%{public}s', marker);
    if (!this.runtimeNeedsRefresh()) {
      hilog.info(DOMAIN, 'wine', 'extract: runtime manifest matches');
      return;
    }
    hilog.info(DOMAIN, 'wine', 'extract: runtime missing or changed, start managed refresh');

    const rm = this.ctx!.resourceManager;
    const rawFd = rm.getRawFdSync('wine-data.zip');

    try {
      // 流式拷贝: getRawFdSync 获取 fd → 分块读取 → 写入临时文件
      hilog.info(DOMAIN, 'wine', 'extract: rawFd fd=%{public}d offset=%{public}d length=%{public}d',
        rawFd.fd, rawFd.offset, rawFd.length);

      const tmpZip = `${FILES_BASE}/wine-data.zip`;
      hilog.info(DOMAIN, 'wine', 'extract: writing tmpZip=%{public}s (stream)', tmpZip);
      const dst = fs.openSync(tmpZip, fs.OpenMode.CREATE | fs.OpenMode.WRITE_ONLY | fs.OpenMode.TRUNC);

      const CHUNK = 4 * 1024 * 1024; // 4MB 分块
      const buf = new ArrayBuffer(CHUNK);
      let pos = rawFd.offset;
      const end = rawFd.offset + rawFd.length;
      let totalWritten = 0;

      while (pos < end) {
        const toRead = Math.min(CHUNK, end - pos);
        const n = fs.readSync(rawFd.fd, buf, { offset: pos, length: toRead });
        if (n <= 0) break;
        fs.writeSync(dst.fd, buf, { length: n });
        pos += n;
        totalWritten += n;
      }
      fs.closeSync(dst);
      hilog.info(DOMAIN, 'wine', 'extract: stream copy ok, totalWritten=%{public}d', totalWritten);

      // files/wine 是受管运行时: 目录级原子刷新, 不碰 .wine / .wine-smoke prefix
      if (pathExists(WINE_ROOT)) removePathRecursive(WINE_ROOT);
      // 确保目标目录存在
      fs.mkdirSync(WINE_ROOT, true);
      hilog.info(DOMAIN, 'wine', 'extract: calling decompressFile → %{public}s', WINE_ROOT);
      await zlib.decompressFile(tmpZip, WINE_ROOT, {});
      hilog.info(DOMAIN, 'wine', 'extract: decompress ok');

      // 验证
      if (fs.accessSync(marker)) {
        hilog.info(DOMAIN, 'wine', 'extract: verified, marker exists');
        if (packagedManifest) writeTextFile(`${WINE_ROOT}/.winehua-runtime-manifest.json`, packagedManifest);
      } else {
        hilog.warn(DOMAIN, 'wine', 'extract: marker still missing after decompress!');
      }
      // 解压成功 = 运行时已最新, 清除升级待处理标记与跳过记忆
      // (重置路径也在 doInit 内解压)
      this.upgradePending = false;
      this.skippedUpgradeSha = '';
      if (this.settingsStore) {
        try {
          this.settingsStore.deleteSync('skipped_upgrade_sha');
          this.settingsStore.flush();
        } catch (e) {
          hilog.warn(DOMAIN, 'wine', 'skip upgrade sha clear fail: %{public}s', JSON.stringify(e));
        }
      }
      this.notify();
      // 清理流式拷贝的临时 zip — 不删每次升级会残留 250MB
      try {
        fs.unlinkSync(tmpZip);
      } catch (_) {}
    } catch (e) {
      // 解压失败不直写状态文案: 继续走下去 native 会因运行时缺失发
      // state:failed, 由状态机统一呈现失败 + 重试入口
      hilog.error(DOMAIN, 'wine', 'extract FAIL: %{public}s', JSON.stringify(e));
      // 失败也要清跳过记忆与临时 zip: 否则「立即应用」重置后解压失败 → sha
      // 残留 → 同版本升级被永久抑制, 且 250MB tmpZip 每次失败都残留
      this.upgradePending = false;
      this.skippedUpgradeSha = '';
      if (this.settingsStore) {
        try {
          this.settingsStore.deleteSync('skipped_upgrade_sha');
          this.settingsStore.flush();
        } catch (e2) {
          hilog.warn(DOMAIN, 'wine', 'skip upgrade sha clear fail: %{public}s', JSON.stringify(e2));
        }
      }
      try {
        fs.unlinkSync(`${FILES_BASE}/wine-data.zip`);
      } catch (_) {}
      this.notify();
    } finally {
      rm.closeRawFdSync('wine-data.zip');
    }
  }

  private refreshPrefix = (): void => {
    try {
      this.prefixReady = testNapi.checkWinePrefix(this.prefixMode);
    } catch (e) {
      this.prefixReady = false;
    }
    // prefixReady 变化后的目录联动 (curDir/dirStack/listDir/清空 entries)
    // 由页面镜像时检测边沿自行处理, service 不碰页面字段
    this.notify();
  }

  // wineboot 后台异步初始化 prefix，轮询直到 registry 和核心目录完整
  private waitForPrefix = async (): Promise<void> => {
    for (let i = 0; i < 90; i++) {
      try {
        if (testNapi.checkWinePrefix(this.prefixMode)) {
          this.prefixReady = true;
          this.initOverlayVisible = false;
          this.notify();
          this.startRequestedSmoke();
          this.startRequestedGame();
          return;
        }
      } catch (e) { /* prefix 还没初始化完成 */ }
      await new Promise<void>(r => setTimeout(r, 1000));
    }
    // 超时，仍然尝试刷新一次
    this.refreshPrefix();
    if (this.prefixReady) {
      this.initOverlayVisible = false;
      this.notify();
      this.startRequestedSmoke();
      this.startRequestedGame();
    } else {
      this.failInit('Wine 前缀初始化超时');
      hilog.error(DOMAIN, 'wine', 'prefix remained unavailable after initialization timeout');
    }
  }

  // 初始化失败统一定式: 标记 initFailed + overlay 保持可见 —
  // 首启引导 overlay 据此把 spinner 换成 重试/重置 入口 (设计 4.8:
  // 每个失败点给自救入口, 不允许只显示一行错误文字)。
  // engineState 一并落成 'failed': 失败是终态, 否则 'stopping'/'starting'
  // 残留的 busy 判据会把所有按钮永久锁死 (软锁)
  private failInit(text: string): void {
    this.initFailed = true;
    this.initBusy = false;
    this.opBusy = false;
    if (this.engineState == 'starting' || this.engineState == 'stopping') {
      this.engineState = 'failed';
    }
    // 后台自动启动 (bgStart) 失败不弹模态 overlay — 对用户无感的承诺;
    // 启动器内 EngineGuide 的 failed 分支承接 重试/重置 自救
    this.setInitStatus(text, !this.bgStart);
    this.bgStart = false;
  }

  // prefix 是否已在盘上 (drive_c 存在): 引导"首次/二次进入"文案分野 —
  // 首次启动含解压数据+建 prefix, 耗时显著多于暖启动, 需提前给耗时预期
  hasPrefixOnDisk(): boolean {
    return pathExists(this.driveC);
  }

  // 失败 overlay 的"返回启动器"出口: 仅失败终态且无任何编排时可关 —
  // busy 期间 overlay 必须常驻 (模态防重入); initFailed 保持置真,
  // 关闭后启动器内 EngineGuide 承接 failed 引导 (重试/重置环境)
  dismissFailedInit(): void {
    if (this.initFailed && !this.isBusy()) {
      this.initOverlayVisible = false;
      this.notify();
    }
  }

  // zHome (Z: 映射目标) 的唯一获取入口, doInit 开头调用一次, 其它代码只读不写。
  // 契约: 返回 true 时 zHome 必为非空 "/"-结尾路径; 三个来源 — 自动化模式用
  // 沙箱内受管 HOME 永不弹 picker; game 模式指到桌面会话同一受管目录
  // (Z: 可见已安装游戏); 普通模式经 DocumentViewPicker 取 Download 路径,
  // 已设置过则直接复用 (重启引擎/已就绪补拉桌面不再走 picker)。
  // 赋值后必须 notify: prefixReady 可能经 waitForPrefix 先于本方法完成置真
  // (桌面会话存活时几乎秒回), 常驻页面已在用 '' 渲染, 需一次刷新拿真实路径
  private async acquireZHome(ctx: common.Context): Promise<boolean> {
    if (this.automationMode) {
      this.zHome = `${WINE_AUTOMATION_HOME}/`;
    } else if (this.gameTestMode) {
      this.zHome = '/storage/Users/currentUser/Download/app.hackeris.winehua/';
    } else if (!this.zHome) {
      const documentViewPicker = new picker.DocumentViewPicker(ctx);
      // DOWNLOAD 模式直达 Download/包名/ 不弹界面; 仍防御取消/异常 —
      // 此前空结果会在 FileUri(undefined) 处抛成未捕获 rejection, initBusy 卡死
      let documentSaveResult: string[] = [];
      try {
        documentSaveResult = await documentViewPicker.save({ pickerMode: picker.DocumentPickerMode.DOWNLOAD });
      } catch (e) {
        hilog.warn(DOMAIN, 'wine', 'Z home picker failed: %{public}s', JSON.stringify(e));
      }
      if (!documentSaveResult || documentSaveResult.length == 0) {
        return false;
      }
      const pickerHome = new fileUri.FileUri(documentSaveResult[0]).path;
      this.zHome = pickerHome.endsWith('/') ? pickerHome : `${pickerHome}/`;
    }
    hilog.info(DOMAIN, 'wine', 'auto-init: Z home=%{public}s', this.zHome);
    this.notify();
    return true;
  }

  doInit = async (): Promise<void> => {
    if (!this.ctx) {
      return;
    }
    // 防重入: picker/解压/startServer 全程可达数分钟 (首启解压 249MB),
    // 期间重复进入会并发解压、目录级刷新 runtime, 后果是运行时损坏
    if (this.initBusy || this.opBusy || this.engineState == 'starting') {
      hilog.warn(DOMAIN, 'wine', 'doInit re-entry blocked (busy)');
      return;
    }
    this.initBusy = true;
    this.initFailed = false;
    if (!await this.acquireZHome(this.ctx)) {
      this.failInit('获取下载目录失败');
      return;
    }
    // HDC shell 无权删除应用沙箱文件, clean 烟测 prefix 必须由 App 自己重置,
    // 且要在 server/socket 启动之前完成
    if (this.automationMode && this.prefixMode === 'clean' && !this.cleanPrefixPrepared) {
      this.setInitStatus('正在创建干净测试环境...', true);
      const resetOk = testNapi.resetWinePrefix('clean') as boolean;
      if (!resetOk) {
        this.failInit('干净测试环境重置失败');
        hilog.error(DOMAIN, 'SmokeRunner', 'App-owned clean prefix reset failed');
        return;
      }
      this.cleanPrefixPrepared = true;
      this.prefixReady = false;
      hilog.info(DOMAIN, 'SmokeRunner', 'App-owned clean prefix reset completed');
    }
    const gamesDir = `${this.zHome}games`;
    if (!ensureDirectory(gamesDir)) {
      this.failInit('创建 games 目录失败');
      return;
    }
    hilog.info(DOMAIN, 'wine', 'auto-init: games directory=%{public}s', gamesDir);
    // 检查是否需要解压 wine 数据文件:
    // - 数据缺失 (首启/重置后, wine.inf 不存在): 必须解压, 否则引擎起不来
    // - 数据存在但与 HAP 打包不一致 (升级): 不自动解压 — 静默换数据会与旧
    //   prefix 不兼容; 置 upgradePending 由用户主动选择应用 (完整重置)
    hilog.info(DOMAIN, 'wine', 'auto-init: check marker=%{public}s', `${WINE_ROOT}/share/wine/wine.inf`);
    if (!pathExists(`${WINE_ROOT}/share/wine/wine.inf`)) {
      this.setInitStatus('解压数据...', !this.bgStart);
      hilog.info(DOMAIN, 'wine', 'auto-init: extracting wine data (missing)...');
      await this.extractWineData();
    } else if (this.upgradeNeedsAttention() && !this.isAutomation()) {
      hilog.info(DOMAIN, 'wine', 'auto-init: runtime outdated, NOT auto-extracting (user-initiated)');
      // 中止初始化: 引擎保持未启动, 弹卡等用户决定 (应用=doReset 引擎静止
      // 无竞态; 稍后=dismiss 记跳过 sha 后补拉起, 同版本不再拦截)。
      // 状态复位必须完整: initBusy 不清 → 后续 doInit 防重入卡死; overlay/
      // initFailed 不清 → 与升级卡片叠层 (失败重试路径); notify 必须在全部
      // 复位之后, 否则 Index 镜像读到中间状态 (标题栏转圈卡住)
      this.engineState = 'idle';
      this.engineStage = '';
      this.initFailed = false;
      this.initOverlayVisible = false;
      this.initBusy = false;
      this.upgradePending = true;
      this.notify();
      return;
    } else if (this.runtimeNeedsRefresh() && this.isAutomation()) {
      // 自动化/游戏测试模式 (无人值守): 升级不弹卡 — 没有用户可点,
      // 中止会让 smoke/game 等 ready 永久挂起; 保持旧行为自动解压自愈。
      // 注意必须带 isAutomation 条件: 普通模式"已跳过的升级"
      // (upgradeNeedsAttention=false) 也会走到这里, 若不区分会被静默
      // 应用用户明确跳过的更新 — 该场景应落空继续, 用旧数据启动
      this.setInitStatus('解压数据...', !this.bgStart);
      hilog.info(DOMAIN, 'wine', 'auto-init: automation extracting wine data...');
      await this.extractWineData();
    }
    /* C:\smoke 位于持久 prefix 内而非可替换的 files/wine 运行时里。
     * 每次启动刷新这棵 manifest 树, 防止覆盖安装后继续跑旧的测试二进制。 */
    if (!syncManagedSmoke(this.prefixMode)) {
      this.failInit('测试程序同步失败');
      hilog.error(DOMAIN, 'wine', 'auto-init: managed C:\\smoke synchronization failed');
      return;
    }
    hilog.info(DOMAIN, 'wine', 'auto-init: starting Wine...');
    this.setInitStatus('启动服务...', !this.bgStart);
    testNapi.setHostShadowProfile(this.perfProfile);
    const ok: boolean = testNapi.startServer(this.sockPath);
    if (!ok) {
      this.failInit('启动服务失败');
      return;
    }
    testNapi.launchClient(BOX64, [], this.sockPath, `${BIN_DIR}:${WINE_LIB}`, this.zHome,
      this.automationMode, this.prefixMode, this.d3dBackend, this.wineLang);
  }

  // 重置 prefix (编排式): 有会话先停止并等 state:stopped (native 确认注册表进程
  // 全死 zombie 感知 + wineserver 死), 再删 prefix, 最后自动重新初始化。
  // 全程 opBusy, 按钮矩阵据此禁用一切会话操作 — 旧版运行中可直接重置,
  // 等于在活着的 wineserver/explorer 脚下删 prefix。
  // 返回 false = 被 busy 防重入拦截 (调用方须保留升级卡片, 勿记跳过)
  doReset = (): boolean => {
    if (this.isBusy()) {
      hilog.warn(DOMAIN, 'wine', 'doReset blocked (busy)');
      return false;
    }
    this.opBusy = true;
    (async (): Promise<void> => {
      try {
        if (this.sessionAlive) {
          this.engineState = 'stopping';
          this.setInitStatus('正在停止会话...', true);
          try {
            testNapi.stopAll();
          } catch (e) {
            hilog.error(DOMAIN, 'wine', 'reset stopAll error: %{public}s', JSON.stringify(e));
          }
          await this.waitForEngineStopped(10000);
        }
        this.setInitStatus('正在清空...', true);
        try {
          // 完整恢复出厂: 清 C 盘 (prefix) + 重装 wine 数据。
          // 删掉运行时目录后, 随后的 doInit 会按"数据缺失"路径重新解压
          if (pathExists(WINE_ROOT)) removePathRecursive(WINE_ROOT);
          testNapi.resetWinePrefix(this.prefixMode);
        } catch (e) {
          hilog.error(DOMAIN, 'wine', 'reset error: %{public}s', JSON.stringify(e));
          this.failInit('清空失败');
          return;
        }
        this.engineState = 'idle';
        this.engineStage = '';
        this.prefixReady = false;
        ProcessService.getInstance().clear();
        // state:stopped 已确认主 wineserver 被杀干净, 随后的 doInit 会 spawn
        // 全新 wineserver — 阶段1 遗留的"旧 wineserver 内存 registry 复用
        // 旧 prefix"问题自此消除
        this.setInitStatus('已清空', false);
        setTimeout(() => this.doInit(), 500);
      } finally {
        this.opBusy = false;
      }
    })();
    // 编排启动后才返回 (同步置 opBusy 防重入): 返回 false = busy 拦截
    return true;
  }

  // 重启引擎 (环境页按钮): 有会话先编排停止再初始化, 无会话直接初始化。
  // 修历史 bug: 桌面存活时直接 doInit 会再 spawn 一套 wineserver/explorer
  // → 双桌面同时渲染, 界面卡死
  restartEngine(): void {
    if (this.isBusy()) {
      return;
    }
    if (!this.sessionAlive) {
      this.doInit();
      return;
    }
    this.opBusy = true;
    (async (): Promise<void> => {
      this.engineState = 'stopping';
      this.setInitStatus('正在停止会话...', true);
      try {
        testNapi.stopAll();
      } catch (e) {
        hilog.error(DOMAIN, 'wine', 'restart stopAll error: %{public}s', JSON.stringify(e));
      }
      await this.waitForEngineStopped(10000);
      this.engineState = 'idle';
      this.opBusy = false;
      this.doInit();
    })();
  }

  // 按需拉起引擎 (懒初始化入口): 标题栏"启动引擎"按钮与启动 app 前调用。
  // ready 直返; idle/failed 走 restartEngine 编排后等 state:ready — 首启含
  // 249MB 解压可达数分钟, 300s 超时仅作安全网, 正常失败路径都经 failInit
  // 给出 initFailed/failed 信号提前返回。busy (初始化/停止编排进行中)
  // 直返 false — 调用方 UI 已做防重入, 不与编排抢跑
  async ensureEngineReady(): Promise<boolean> {
    if (this.engineState == 'ready') {
      return true;
    }
    if (this.isBusy()) {
      return false;
    }
    this.restartEngine();
    const deadline = Date.now() + 300000;
    while (this.engineState != 'ready' && Date.now() < deadline) {
      if (this.initFailed || this.engineState == 'failed') {
        return false;
      }
      await new Promise<void>((r) => setTimeout(r, 200));
    }
    return this.engineState == 'ready';
  }

  // 已就绪但无桌面会话时，先拉起桌面外壳（explorer desktop shell），
  // 供启动程序前调用：避免程序窗口单独出现而没有桌面。
  // 复用 doInit 热重启路径（sessionAlive=false → launchClient spawn 新 explorer
  // 连现有 wineserver，桌面 root 出现后 native 发 state:ready）。
  // 返回 true = 桌面已就绪；false = 拉起失败/超时。
  async ensureDesktop(): Promise<boolean> {
    // PC 窗口模式 (managed, 非 tablet/phone): 程序是独立窗口, 没有桌面壳概念,
    // 不需要拉起 explorer desktop shell — 直接放行 (旧实现调 doInit 只拉起文件
    // 管理器窗口, 且等不到 desktop_root/desktopSurfaceReady, 25s 超时误报失败)
    if (!this.desktopMode) {
      return true;
    }
    if (this.sessionAlive && this.desktopSurfaceReady) {
      return true;
    }
    if (this.engineState != 'ready') {
      return false;
    }
    await this.doInit();
    // 就绪判据 = root 出现（desktopActive → sessionAlive）&& renderer surface
    // 校准完成（desktopSurfaceReady）。只等 root 不够: desktop_root 是 toplevel
    // 出现, renderer 就绪晚数秒 — 立即放行程序会抢占桌面渲染初始化导致程序
    // 闪退 (实测: 程序窗口出现在 [MW-Resize] renderer resized OK 之前即被
    // Wine 销毁)。两信号都经 WineWindowManager → 镜像 → notify 重算。
    // 失败信号用 initFailed（doInit 各失败分支与 native state:failed 都经 failInit 置位；
    // engineState 在 doInit 早期失败时可能仍是 ready，不能作失败判据）
    const timeoutMs = 25000;
    const deadline = Date.now() + timeoutMs;
    while ((!this.sessionAlive || !this.desktopSurfaceReady) && Date.now() < deadline) {
      if (this.initFailed) {
        return false;
      }
      await new Promise<void>((r) => setTimeout(r, 150));
    }
    return this.sessionAlive && this.desktopSurfaceReady;
  }

  // 停止完成判定 (阶段2 起为事件): native stopAll 在注册表进程全死
  // (zombie 感知) + 主 wineserver 死 + Wayland 停之后发一次 state:stopped,
  // 这里挂一次性回调等它 — 取代阶段1 的"桌面销毁 + 进程表空"轮询
  // (ProcMon 1Hz + ArkTS 300ms 节流的近似判据)。超时硬放行仅作安全网:
  // native 卡死时不至于把 UI 软锁在 stopping
  private waitForEngineStopped(timeoutMs: number): Promise<void> {
    return new Promise<void>((resolve) => {
      let done = false;
      const finish = (): void => {
        if (!done) {
          done = true;
          resolve();
        }
      };
      this.stopWaiters.push(finish);
      setTimeout(() => {
        if (!done) {
          hilog.warn(DOMAIN, 'wine', 'waitForEngineStopped timeout, proceeding anyway');
          finish();
        }
      }, timeoutMs);
    });
  }

  // 停止全部 Windows 会话 (环境页"会话维护"卡): native stopAll 杀全部登记进程 +
  // 主 wineserver + 停 Wayland server, 桌面 root 随之销毁 → WineWindowManager
  // 翻回启动器。等 state:stopped 后状态归零; 期间 opBusy 禁用重启/重置
  stopAllSessions(): void {
    if (this.opBusy) {
      return;
    }
    this.opBusy = true;
    (async (): Promise<void> => {
      this.engineState = 'stopping';
      this.notify();
      try {
        testNapi.stopAll();
      } catch (e) {
        hilog.error(DOMAIN, 'wine', 'stopAll error: %{public}s', JSON.stringify(e));
      }
      await this.waitForEngineStopped(10000);
      this.engineState = 'idle';
      this.opBusy = false;
      this.notify();
    })();
  }

  // ---- 自动化烟测 / 游戏启动 (waitForPrefix 就绪后触发, 各只执行一次) ----

  // master 开发版专用: 开发者烟测入口 (发布版 main-ui 无此功能)。
  // 原 master Index.startDeveloperSmoke 迁入 — 设置 smoke 请求后走 doInit,
  // automationMode 使 doInit 免 picker (WINE_AUTOMATION_HOME), prefix 就绪后
  // waitForPrefix 自动触发 startRequestedSmoke
  startDeveloperSmoke(): void {
    this.smokeRequest = {
      mode: 'smoke', runId: `developer-${Date.now()}`, suite: 'core',
      prefixMode: 'reuse', longSeconds: 3600, batchMappedFlush: false
    };
    this.automationMode = true;
    this.gameTestMode = false;
    this.prefixMode = 'reuse';
    this.prefixRoot = `${FILES_BASE}/.wine`;
    this.driveC = `${this.prefixRoot}/drive_c`;
    this.sockPath = `${this.prefixRoot}/wine-wayland`;
    this.smokeStarted = false;
    this.doInit();
  }

  private startRequestedSmoke = (): void => {
    if (!this.automationMode || this.smokeStarted) return;
    this.smokeStarted = true;
    const runner = new SmokeRunner(this.smokeRequest, (text: string) => {
      this.statusText = text;
      this.initOverlayText = text;
      this.notify();
    }, this.perfProfile);
    runner.run().catch((error: Object) => {
      this.setStatusText('Smoke: infrastructure failure');
      hilog.error(DOMAIN, 'SmokeRunner', 'suite failed: %{public}s', JSON.stringify(error));
    });
  }

  private startRequestedGame = (): void => {
    if (!this.gameTestMode || this.gameStarted || this.gamePath.length === 0) return;
    this.gameStarted = true;
    this.setStatusText(`Launching ${this.d3dBackend}: ${this.gamePath}`);
    const gamePath = this.gamePath;
    const result = testNapi.runWineProgram({
      windowsExePath: gamePath,
      // AppStorage 取出的数组是可观察代理, 过 N-API 前切一刀成普通数组,
      // 保证 napi_is_array 能枚举全部游戏参数
      argv: this.gameArguments.slice(),
      environment: this.d3dLaunchEnvironment(),
      workingDirectory: this.windowsParentDirectory(gamePath),
      prefixMode: 'reuse',
      d3dBackend: this.d3dBackend,
      presentBackend: this.d3dPresentBackend(),
      automationMode: false
    }) as ProcessInfo;
    hilog.info(DOMAIN, 'GameTest',
      'game launch path=%{public}s argc=%{public}d env=%{public}d backend=%{public}s perf=%{public}s '
        + 'pid=%{public}d status=%{public}s',
      gamePath, this.gameArguments.length, this.d3dEnvKeys.length,
      this.d3dBackend, this.perfProfile,
      result.pid, result.state);
    if (this.gameClickTitlePrefix.length > 0 && this.gameClickButtonText.length > 0) {
      const titlePrefix = this.gameClickTitlePrefix;
      const buttonText = this.gameClickButtonText;
      const delayMs = this.gameClickDelayMs;
      const clickXPermille = this.gameClickClientXPermille;
      const clickYPermille = this.gameClickClientYPermille;
      setTimeout(() => {
        const driverArguments = ['--attach', '--title-prefix', titlePrefix,
          '--button-text', buttonText, '--timeout-ms', '60000'];
        if (clickXPermille >= 0 && clickYPermille >= 0) {
          driverArguments.push('--client-x-permille', `${clickXPermille}`,
            '--client-y-permille', `${clickYPermille}`);
        }
        const driverResult = testNapi.runWineProgram({
          windowsExePath: 'C:\\smoke\\x64\\winehua_win32_driver.exe',
          argv: driverArguments,
          environment: {},
          workingDirectory: 'C:\\smoke\\x64',
          prefixMode: 'reuse',
          d3dBackend: this.d3dBackend,
          presentBackend: this.d3dPresentBackend(),
          automationMode: false
        }) as ProcessInfo;
        hilog.info(DOMAIN, 'GameTest',
          'click driver title=%{public}s button=%{public}s relative=%{public}d,%{public}d '
            + 'pid=%{public}d status=%{public}s',
          titlePrefix, buttonText, clickXPermille, clickYPermille,
          driverResult.pid, driverResult.state);
      }, delayMs);
    }
  }

  private windowsParentDirectory(path: string): string {
    const separator = Math.max(path.lastIndexOf('\\'), path.lastIndexOf('/'));
    return separator > 2 ? path.substring(0, separator) : 'C:\\';
  }

  // DXVK Legacy 启动环境: 日志 + Venus 共享内存 ring 的 Box64 屏障约束 +
  // perf profile 对应的一组 VN_/VKR_/DXVK_ 开关, 最后叠加 want 携带的显式覆盖项
  private d3dLaunchEnvironment(): Record<string, string> {
    if (this.d3dBackend !== 'dxvk_legacy') return {};
    ensureDirectory(`${this.driveC}/smoke/results/desktop-dxvk`);
    const environment: Record<string, string> = {
      'DXVK_LOG_LEVEL': 'warn',
      'DXVK_LOG_PATH': 'C:\\smoke\\results\\desktop-dxvk',
      'WINEDEBUG': '-all',
      /* Venus 在 x86_64 guest 与 ARM64 renderer 之间用共享内存命令环。
       * Box64 激进的弱屏障模式可能破坏 ring 期望的 x86 定序, 最终损坏随机
       * Vulkan 命令。该约束只作用于 DXVK/Venus 进程。 */
      'BOX64_DYNAREC_WEAKBARRIER': '0',
      'WINEHUA_PERF_PROFILE': this.perfProfile
    };
    if (this.perfProfile === 'shadow-precise-strong-ring-trace')
      environment['VN_WINEHUA_PRESENT_TRACE'] = '1';
    if (this.perfProfile === 'direct-fence-wait')
      environment['VN_WINEHUA_DIRECT_FENCE_WAIT'] = '1';
    else if (this.perfProfile === 'no-remote-sync')
      environment['VN_WINEHUA_REMOTE_MEMORY_SYNC'] = '0';
    else if (this.perfProfile === 'no-dynamic-flush')
      environment['DXVK_WINEHUA_FLUSH_DYNAMIC_MAPPED'] = '0';
    else if (this.perfProfile === 'fence-feedback')
      environment['VN_PERF'] = '';
    else if (this.perfProfile === 'shadow-none') {
      environment['VKR_WINEHUA_SHADOW_FROM_HOST'] = 'none';
      environment['VKR_WINEHUA_SHADOW_TRACE'] = '0';
    } else if (this.perfProfile === 'shadow-trace') {
      environment['VKR_WINEHUA_SHADOW_FROM_HOST'] = 'full';
      environment['VKR_WINEHUA_SHADOW_TRACE'] = '1';
      /* game Want 在部分 Harmony 版本上无法可靠携带任意 env 数组,
       * 复用显式诊断 profile 作为有界 trace 开关, 保证 Heaven
       * render-pass 排查可复现。 */
      environment['DXVK_WINEHUA_TRACE_SAMPLED'] = '1';
      environment['DXVK_WINEHUA_TRACE_FLOW'] = '1';
    } else if (this.perfProfile === 'shadow-to-host-explicit') {
      environment['VKR_WINEHUA_SHADOW_FROM_HOST'] = 'to-host-explicit';
      environment['VKR_WINEHUA_SHADOW_TRACE'] = '0';
    } else if (this.perfProfile === 'shadow-precise' ||
      this.perfProfile === 'shadow-precise-single-ring' ||
      this.perfProfile === 'shadow-precise-sync-submit' ||
      this.perfProfile === 'shadow-precise-strong-ring' ||
      this.perfProfile === 'shadow-precise-legacy-host-sync' ||
      this.perfProfile === 'shadow-precise-dirty-ring' ||
      this.perfProfile === 'shadow-precise-dirty-ring-no-upload' ||
      this.perfProfile === 'shadow-precise-dirty-ring-no-upload-fast' ||
      this.perfProfile === 'shadow-precise-dirty-ring-inline-upload' ||
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
      environment['DXVK_WINEHUA_PRECISE_SHADOW'] = '1';
      environment['DXVK_WINEHUA_FLUSH_DYNAMIC_MAPPED'] = '1';
      environment['VN_WINEHUA_REMOTE_MEMORY_SYNC'] = '1';
      environment['VKR_WINEHUA_SHADOW_FROM_HOST'] = 'precise';
      environment['VKR_WINEHUA_SHADOW_TRACE'] =
        this.perfProfile === 'shadow-precise-strong-ring-trace' ? '1' : '0';
      if (this.perfProfile === 'shadow-precise-single-ring')
        environment['VN_PERF'] = 'no_fence_feedback,no_query_feedback,no_multi_ring';
      if (this.perfProfile === 'shadow-precise-sync-submit')
        environment['VN_PERF'] =
          'no_fence_feedback,no_query_feedback,no_async_queue_submit';
      if (this.perfProfile === 'shadow-precise-strong-ring' ||
        this.perfProfile === 'shadow-precise-legacy-host-sync' ||
        this.perfProfile === 'shadow-precise-dirty-ring' ||
        this.perfProfile === 'shadow-precise-dirty-ring-no-upload' ||
        this.perfProfile === 'shadow-precise-dirty-ring-no-upload-fast' ||
        this.perfProfile === 'shadow-precise-dirty-ring-inline-upload' ||
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
        environment['VN_WINEHUA_STRONG_RING_BARRIER'] = '1';
      if (this.perfProfile === 'shadow-precise-strong-ring-perf' ||
        this.perfProfile === 'shadow-precise-dirty-ring-perf')
        environment['VN_WINEHUA_PERF_SUMMARY'] = '1';
      if (this.perfProfile === 'shadow-precise-direct-fence')
        environment['VN_WINEHUA_DIRECT_FENCE_WAIT'] = '1';
      if (this.perfProfile === 'shadow-precise-dirty-ring-frame-assoc-trace')
        environment['WINEHUA_DXVK_TRACE_CAMERA'] = '1';
      if (this.perfProfile === 'shadow-precise-dirty-ring-present-image-trace')
        environment['WINEHUA_DXVK_TRACE_PRESENT_IMAGE'] = '1';
      if (this.perfProfile === 'shadow-precise-retain-shmem')
        environment['VN_WINEHUA_DEFER_SHMEM_UNREF'] = '1';
      if (this.perfProfile === 'shadow-precise-cpu-upload')
        environment['VKR_WINEHUA_GPU_UPLOAD'] = '0';
    }
    const overrideCount = Math.min(this.d3dEnvKeys.length, this.d3dEnvValues.length);
    for (let index = 0; index < overrideCount; ++index) {
      environment[this.d3dEnvKeys[index]] = this.d3dEnvValues[index];
    }
    return environment;
  }

  private d3dPresentBackend(): string {
    return this.d3dBackend === 'dxvk_legacy' ? 'venus_broker_present' : 'virgl_compositor';
  }
}
