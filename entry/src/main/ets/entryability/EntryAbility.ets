import { DEFAULT_D3D_BACKEND } from '../common/GraphicsDefaults';
import { hasQualifiedMewgenicsGpu } from '../common/MewgenicsGraphics';
import { PhoneGraphicsSettings } from '../service/PhoneGraphicsSettings';
import { AbilityConstant, ConfigurationConstant, UIAbility, Want } from '@kit.AbilityKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { window } from '@kit.ArkUI';
import { deviceInfo } from '@kit.BasicServicesKit';
import { WineWindowManager } from '../service/WineWindowManager';
import { WineEnvService, FILES_BASE, readTextIfExists } from '../service/WineEnvService';
import { ProcessService } from '../service/ProcessService';
import { DesktopModeService } from '../service/DesktopModeService';
import { InputSettingsService } from '../service/InputSettingsService';
// [[SMOKE]] 合并到 main-ui: 删本行与 publishLaunchRequest 内的 applyWant 调用
import { SmokeHook } from '../smoke/SmokeHook';
import testNapi from 'libentry.so';

const DOMAIN = 0x0000;

export default class EntryAbility extends UIAbility {
  private defaultDxvkBackend(): string {
    /* This is an evidence allowlist, not a Host-to-Guest capability inference.
     * The bundled Guest Mesa/DXVK payload is fixed by the HAP manifest, while
     * the product/build identity below pins the audited Host driver. A system
     * update therefore falls back to 1.10.3 until that identity is qualified. */
    const qualified920 = deviceInfo.productModel === 'VYG-AL00' &&
      deviceInfo.incrementalVersion === '26.0.0.32';
    return qualified920 ? 'dxvk_modern_2_6' : 'dxvk_legacy';
  }

  private resolveDxvkBackend(value: string): string {
    if (value === 'dxvk_legacy' || value === 'dxvk_modern_2_6') return value;
    return this.defaultDxvkBackend();
  }

  // 发布启动状态: want 开屏参数 (d3d/dxvk 覆盖; smoke 请求由 SmokeHook 侧挂)
  // → AppStorage 生效值
  // (Index 的 @StorageLink 'winehua.d3d.backend'/'winehua.dxvk.backend' 与
  // WineEnvService.updateLaunchRequest 依赖)。
  private publishLaunchRequest(want: Want): void {
    const parameters = want.parameters;
    const value = (key: string, fallback: string): string => {
      if (!parameters || parameters[key] === undefined || parameters[key] === null) return fallback;
      return `${parameters[key]}`;
    };
    const explicitD3d = !!parameters && parameters['winehua.d3d_backend'] !== undefined &&
      parameters['winehua.d3d_backend'] !== null;
    const persistedD3dRaw = AppStorage.get<string>('winehua.d3d.preference') || DEFAULT_D3D_BACKEND;
    const requestedD3d = explicitD3d ? value('winehua.d3d_backend', 'wined3d') : persistedD3dRaw;
    const d3dBackend = requestedD3d === 'dxvk_legacy' || requestedD3d === 'dxvk_legacy_d3d9' ||
      requestedD3d === 'dxvk_modern_2_6' || requestedD3d === 'vkd3d_limited_500k' ?
      requestedD3d : 'wined3d';
    const explicitDxvk = !!parameters && parameters['winehua.dxvk_backend'] !== undefined &&
      parameters['winehua.dxvk_backend'] !== null;
    const persistedDxvk = AppStorage.get<string>('winehua.dxvk.preference') || 'auto';
    let dxvkBackend = this.resolveDxvkBackend(explicitDxvk ?
      value('winehua.dxvk_backend', 'auto') : persistedDxvk);
    if (d3dBackend === 'dxvk_legacy' || d3dBackend === 'dxvk_legacy_d3d9') dxvkBackend = 'dxvk_legacy';
    if (d3dBackend === 'dxvk_modern_2_6') dxvkBackend = 'dxvk_modern_2_6';
    AppStorage.setOrCreate<string>('winehua.d3d.backend', d3dBackend);
    AppStorage.set<string>('winehua.d3d.backend', d3dBackend);
    AppStorage.setOrCreate<string>('winehua.dxvk.backend', dxvkBackend);
    AppStorage.set<string>('winehua.dxvk.backend', dxvkBackend);
    const scale = Number(value('winehua.phoneScale', '0'));
    AppStorage.setOrCreate<number>('winehua.phone.scale.override', 0);
    AppStorage.set<number>('winehua.phone.scale.override', scale === 2 || scale === 3 ? scale : 0);
    if (scale === 2 || scale === 3)
      PhoneGraphicsSettings.getInstance().setScale(scale);
    const desktopResolution = value('winehua.desktopResolution', '');
    if (desktopResolution === 'auto' || desktopResolution === '1024x768')
      PhoneGraphicsSettings.getInstance().setDesktopResolution(desktopResolution);
    // Optional, per-session diagnostic A/B for GPU buffer synchronization.
    const hostProfile = value('winehua.hostShadowProfile', '');
    AppStorage.setOrCreate<string>('winehua.hostShadowProfile', '');
    AppStorage.set<string>('winehua.hostShadowProfile',
      hostProfile === 'shadow-precise-legacy-host-sync' || hostProfile === 'baseline' ? hostProfile : '');
    // Pad 多窗口承载调试键: winehua.fusionHost (subwindow|ability) 跨设备强制
    // subWindow 承载 (见 WineWindowManager.fusionUsesSubWindow)。仅显式 want
    // 生效, 不持久化。
    const explicitFusionHost = !!parameters && parameters['winehua.fusionHost'] !== undefined &&
      parameters['winehua.fusionHost'] !== null;
    if (explicitFusionHost) {
      AppStorage.set<string>('winehua.fusionHost', value('winehua.fusionHost', ''));
    }
    // 桌面模式显式覆盖 (fusion|virtual): 仅在 applyModePolicy 判定读取, 不持久化
    // (重启引擎场景仍按持久化值)。经 DesktopModeService 单例传递 — AppStorage
    // 跨 Ability 上下文读取不可靠 (见 DesktopModeService.setOverrideMode)。
    const explicitDesktopMode = !!parameters && parameters['winehua.desktopMode'] !== undefined &&
      parameters['winehua.desktopMode'] !== null;
    if (explicitDesktopMode) {
      DesktopModeService.getInstance().setOverrideMode(value('winehua.desktopMode', ''));
    }
    // 自动化通道 (UI 点击/输入不可注入设备的全自动验证):
    // winehua.autoStart=1 → 主页面就绪后自动拉起引擎 (一次消费)
    if (parameters && parameters['winehua.autoStart'] === '1') {
      WineEnvService.getInstance().autoStartPending = true;
    }
    // winehua.program=<wine 路径> → 引擎就绪后自动启动该程序
    if (parameters && parameters['winehua.program'] !== undefined &&
      parameters['winehua.program'] !== null) {
      WineEnvService.getInstance().setAutoProgram(value('winehua.program', ''));
      AppStorage.setOrCreate<boolean>('winehua.program.openglDiag', false);
      AppStorage.set<boolean>('winehua.program.openglDiag', value('winehua.openglDiag', '') === '1');
      AppStorage.setOrCreate<string>('winehua.program.envFile', '');
      AppStorage.set<string>('winehua.program.envFile', value('winehua.program_env_file', ''));
    }
    // [[SMOKE]] 合并到 main-ui: 删本行 (want 的模式/套件/前缀等 smoke 参数解析
    // 在 SmokeHook, 模式非 smoke 时零行为)
    SmokeHook.getInstance().applyWant(parameters);
  }

  // 自动化通道: winehua.program → 引擎就绪后直启该程序。单次消费 (成功才
  // 消费, 见 consumeAutoProgram)。冷启动时 loadContent 回调早于引擎初始化
  // (wineboot 数秒), 故轮询等待就绪 — 与人工操作时序无关, 只服务自动化。
  private autoRunIfRequested(): void {
    const env = WineEnvService.getInstance();
    if (env.autoProgram.length === 0) {
      return;
    }
    let tries = 0;
    const tick = (): void => {
      const p = env.autoProgram;
      if (p.length === 0) {
        return;  // 已被消费
      }
      env.ensureEngineReady().then((ok: boolean) => {
        if (ok) {
          const prog = env.consumeAutoProgram();
          if (prog) {
            hilog.info(DOMAIN, 'testTag', 'autoRun: launch %{public}s', prog);
            this.runProgram(prog);
          }
          return;
        }
        if (++tries < 60) {  // ~60s 上限 (首次初始化含 wineboot)
          setTimeout(tick, 1000);
        } else {
          hilog.warn(DOMAIN, 'testTag', 'autoRun: engine not ready after %{public}d tries, skip %{public}s',
            tries, p);
        }
      }).catch((e: Object) => {
        hilog.warn(DOMAIN, 'testTag', 'autoRun ensureEngineReady: %{public}s', JSON.stringify(e));
        if (++tries < 60) setTimeout(tick, 1000);
      });
    };
    setTimeout(tick, 300);
  }

  // 程序直启 (与 Index.doRun 同款通道; 目录/参数由调用方给出)
  private runProgram(windowsPath: string): void {
    const slash = windowsPath.lastIndexOf('/');
    const dir = slash > 0 ? windowsPath.substring(0, slash) : windowsPath;
    const environment: Record<string, string> = {};
    const envFile = AppStorage.get<string>('winehua.program.envFile') || '';
    // Device diagnostics may supply per-launch settings from the app's files.
    if (envFile.startsWith(`${FILES_BASE}/`) && !envFile.split('/').includes('..')) {
      try {
        const overrides = JSON.parse(readTextIfExists(envFile)) as Record<string, string>;
        for (const key of Object.keys(overrides)) {
          if (typeof overrides[key] === 'string') environment[key] = overrides[key];
        }
      } catch (err) {
        hilog.error(DOMAIN, 'testTag', 'program environment invalid: %{public}s', JSON.stringify(err));
        return;
      }
    }
    if (AppStorage.get<boolean>('winehua.program.openglDiag')) {
      environment['WINEHUA_OPENGL_DIAG'] = '1';
      environment['WINEHUA_WINEDEBUG'] = '-all,err+all,warn+module,warn+wgl,trace+wgl';
    }
    testNapi.runWineProgram({
      windowsExePath: windowsPath,
      argv: [],
      environment: environment,
      workingDirectory: dir,
      d3dBackend: AppStorage.get<string>('winehua.d3d.backend') ?? DEFAULT_D3D_BACKEND,
      dxvkBackend: AppStorage.get<string>('winehua.dxvk.backend') ?? 'auto'
    });
  }

  onCreate(want: Want, launchParam: AbilityConstant.LaunchParam): void {
    PhoneGraphicsSettings.getInstance().init(this.context);
    // 输入设置 (触控板/虚拟键盘/自定义按键) 在首个 Ability 入口初始化一次;
    // init 幂等, 其他页面/Ability 可安全重复调用。
    InputSettingsService.getInstance().init(this.context);
    PersistentStorage.persistProp<string>('winehua.d3d.preference', DEFAULT_D3D_BACKEND);
    PersistentStorage.persistProp<string>('winehua.dxvk.preference', 'auto');
    // Older phone installs persisted the experimental VKD3D default before
    // the user selected a backend. Migrate it once; later selections survive.
    PersistentStorage.persistProp<boolean>('winehua.phone.graphicsBaseline.v1', false);
    if (deviceInfo.deviceType === 'phone' &&
      !AppStorage.get<boolean>('winehua.phone.graphicsBaseline.v1')) {
      if (AppStorage.get<string>('winehua.d3d.preference') === 'vkd3d_limited_500k') {
        AppStorage.set<string>('winehua.d3d.preference', DEFAULT_D3D_BACKEND);
      }
      AppStorage.set<boolean>('winehua.phone.graphicsBaseline.v1', true);
    }
    // 注: Wine 语言偏好不走 PersistentStorage (冷启动不恢复磁盘值, 实测),
    // 由 WineEnvService.init 用 preferences API 自行加载
    try {
      this.context.getApplicationContext().setColorMode(ConfigurationConstant.ColorMode.COLOR_MODE_DARK);
    } catch (err) {
      hilog.error(DOMAIN, 'testTag', 'Failed to set colorMode. Cause: %{public}s', JSON.stringify(err));
    }
    this.publishLaunchRequest(want);
    hilog.info(DOMAIN, 'testTag', '%{public}s', 'Ability onCreate');
  }

  onNewWant(want: Want, launchParam: AbilityConstant.LaunchParam): void {
    this.publishLaunchRequest(want);
    if (deviceInfo.deviceType !== 'phone') WineWindowManager.getInstance().restoreDesktopIfRunning();
    WineEnvService.getInstance().onNewWant();
    // 自动化通道 (运行中会话): winehua.program → 直启该程序
    this.autoRunIfRequested();
  }

  onDestroy(): void {
    hilog.info(DOMAIN, 'testTag', '%{public}s', 'Ability onDestroy');
  }

  onWindowStageCreate(windowStage: window.WindowStage): void {
    // Main window is created, set main page for this ability
    hilog.info(DOMAIN, 'testTag', '%{public}s', 'Ability onWindowStageCreate');

    // 手机走 fork-shim，必须在 WineEnvService.startSession 的首次 NCP 调用前设置。
    testNapi.setPhoneMode(deviceInfo.deviceType === 'phone', hasQualifiedMewgenicsGpu());

    // Phone games and the desktop share a fixed landscape coordinate space.
    if (deviceInfo.deviceType === 'phone') {
      try {
        const mainWindow = windowStage.getMainWindowSync();
        mainWindow.setPreferredOrientation(window.Orientation.LANDSCAPE).then(() => {
          hilog.info(DOMAIN, 'testTag', 'phone landscape enabled');
        }).catch((err: Error) => {
          hilog.warn(DOMAIN, 'testTag', 'phone landscape failed: %{public}s', JSON.stringify(err));
        });
      } catch (err) {
        hilog.warn(DOMAIN, 'testTag', 'phone landscape setup failed: %{public}s', JSON.stringify(err));
      }
    }

    // 初始化多窗口管理器
    WineWindowManager.getInstance().init(windowStage, this.context);

    windowStage.loadContent('pages/Index', (err) => {
      if (err.code) {
        hilog.error(DOMAIN, 'testTag', 'Failed to load the content. Cause: %{public}s', JSON.stringify(err));
        return;
      }
      // loadContent 成功后隐藏窗口装饰
      let mainWin = windowStage.getMainWindowSync();
      if (mainWin) {
        mainWin.setWindowDecorVisible(false);
      }
      hilog.info(DOMAIN, 'testTag', 'Succeeded in loading the content.');
      // 自动化通道 (冷启动): winehua.program → 引擎就绪后直启该程序
      // (winehua.autoStart 的消费在 Index.aboutToAppear)
      this.autoRunIfRequested();
    });
  }

  onWindowStageDestroy(): void {
    // Main window is destroyed, release UI related resources
    hilog.info(DOMAIN, 'testTag', '%{public}s', 'Ability onWindowStageDestroy');
  }

  onForeground(): void {
    hilog.info(DOMAIN, 'testTag', '%{public}s', 'Ability onForeground');
    // Phone users return here to manage or stop a stuck game. Keep this
    // window foreground until they explicitly choose "enter foreground".
    if (deviceInfo.deviceType !== 'phone') WineWindowManager.getInstance().restoreDesktopIfRunning();
    ProcessService.getInstance().refresh();
  }

  onBackground(): void {
    // Ability has back to background
    hilog.info(DOMAIN, 'testTag', '%{public}s', 'Ability onBackground');
  }
}
