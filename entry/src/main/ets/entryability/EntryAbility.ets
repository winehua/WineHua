import { AbilityConstant, ConfigurationConstant, UIAbility, Want } from '@kit.AbilityKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { window } from '@kit.ArkUI';
import { deviceInfo } from '@kit.BasicServicesKit';
import { WineWindowManager } from '../service/WineWindowManager';
import { WineEnvService } from '../service/WineEnvService';
import { SteamHostService } from '../service/SteamHostService';
import { SteamHostHook } from '../service/SteamHostHook';
import { ScreenAwake } from '../service/ScreenAwake';
import { DesktopModeService } from '../service/DesktopModeService';
// [[SMOKE]] 合并到 main-ui: 删本行与 publishLaunchRequest 内的 applyWant 调用
import { SmokeHook } from '../smoke/SmokeHook';
// [[GAME]] Heaven/game Want automation (URI-decode path); delete with ets/game/
import { GameHook } from '../game/GameHook';
import testNapi from 'libentry.so';

const DOMAIN = 0x0000;

export default class EntryAbility extends UIAbility {
  private defaultDxvkBackend(): string {
    return 'dxvk_legacy';
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
    const persistedD3dRaw = AppStorage.get<string>('winehua.d3d.preference') || 'dxvk_legacy';
    const requestedD3d = explicitD3d ? value('winehua.d3d_backend', 'wined3d') : persistedD3dRaw;
    // DXVK 1.10.3 is the product default. DXVK 2.6 remains an explicit
    // opt-in profile; VKD3D is deliberately not accepted from Wants.
    const d3dBackend = requestedD3d === 'wined3d' || requestedD3d === 'dxvk_modern_2_6' ||
      requestedD3d === 'dxvk_legacy' ? requestedD3d : 'dxvk_legacy';
    const explicitDxvk = !!parameters && parameters['winehua.dxvk_backend'] !== undefined &&
      parameters['winehua.dxvk_backend'] !== null;
    const persistedDxvk = AppStorage.get<string>('winehua.dxvk.preference') || 'dxvk_legacy';
    let dxvkBackend = this.resolveDxvkBackend(explicitDxvk ?
      value('winehua.dxvk_backend', 'dxvk_legacy') : persistedDxvk);
    if (d3dBackend === 'dxvk_legacy') dxvkBackend = 'dxvk_legacy';
    if (d3dBackend === 'dxvk_modern_2_6') dxvkBackend = 'dxvk_modern_2_6';
    AppStorage.setOrCreate<string>('winehua.d3d.backend', d3dBackend);
    AppStorage.set<string>('winehua.d3d.backend', d3dBackend);
    AppStorage.setOrCreate<string>('winehua.dxvk.backend', dxvkBackend);
    AppStorage.set<string>('winehua.dxvk.backend', dxvkBackend);
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
    }
    // [[SMOKE]] 合并到 main-ui: 删本行 (want 的模式/套件/前缀等 smoke 参数解析
    // 在 SmokeHook, 模式非 smoke 时零行为)
    SmokeHook.getInstance().applyWant(parameters);
    // [[GAME]] mode=game Want → GameHook
    GameHook.getInstance().applyWant(parameters);
    // mode=steam-host is a controlled Host content action. It never launches
    // Windows Steam and only accepts the paired-content protocol parameters.
    SteamHostHook.getInstance().applyWant(parameters);
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
    testNapi.runWineProgram({
      windowsExePath: windowsPath,
      argv: [],
      environment: {},
      workingDirectory: dir,
      d3dBackend: AppStorage.get<string>('winehua.d3d.backend') ?? 'vkd3d_limited_500k',
      dxvkBackend: AppStorage.get<string>('winehua.dxvk.backend') ?? 'auto'
    });
  }

  onCreate(want: Want, launchParam: AbilityConstant.LaunchParam): void {
    // Default D3D: DXVK 1.10 (legacy). Do not auto-select/init VKD3D.
    PersistentStorage.persistProp<string>('winehua.d3d.preference', 'dxvk_legacy');
    PersistentStorage.persistProp<string>('winehua.dxvk.preference', 'dxvk_legacy');
    // 注: Wine 语言偏好不走 PersistentStorage (冷启动不恢复磁盘值, 实测),
    // 由 WineEnvService.init 用 preferences API 自行加载
    try {
      this.context.getApplicationContext().setColorMode(ConfigurationConstant.ColorMode.COLOR_MODE_DARK);
    } catch (err) {
      hilog.error(DOMAIN, 'testTag', 'Failed to set colorMode. Cause: %{public}s', JSON.stringify(err));
    }
    SteamHostService.getInstance().init(this.context);
    this.publishLaunchRequest(want);
    hilog.info(DOMAIN, 'testTag', '%{public}s', 'Ability onCreate');
  }

  onNewWant(want: Want, launchParam: AbilityConstant.LaunchParam): void {
    this.publishLaunchRequest(want);
    WineWindowManager.getInstance().restoreDesktopIfRunning();
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
    testNapi.setPhoneMode(deviceInfo.deviceType === 'phone');

    // Phone desktop is rendered in a full-window XComponent. Let the system
    // rotate that window with the sensor; its surface callback propagates the
    // resulting size to Wine before the next frame is composited.
    if (deviceInfo.deviceType === 'phone') {
      try {
        const mainWindow = windowStage.getMainWindowSync();
        mainWindow.setPreferredOrientation(window.Orientation.AUTO_ROTATION).then(() => {
          hilog.info(DOMAIN, 'testTag', 'phone auto-rotation enabled');
        }).catch((err: Error) => {
          hilog.warn(DOMAIN, 'testTag', 'phone auto-rotation failed: %{public}s', JSON.stringify(err));
        });
      } catch (err) {
        hilog.warn(DOMAIN, 'testTag', 'phone auto-rotation setup failed: %{public}s', JSON.stringify(err));
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
        // 前台期间保持屏幕常亮: 熄屏会自动锁屏, 而锁屏状态下 aa start 被拒
        // (10106102), 自动化回归与 Steam 会话都会被打断。
        ScreenAwake.attach(mainWin);
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
    // Ability has brought to foreground
    hilog.info(DOMAIN, 'testTag', '%{public}s', 'Ability onForeground');
    ScreenAwake.setKeepScreenOn(true);
  }

  onBackground(): void {
    // Ability has back to background
    hilog.info(DOMAIN, 'testTag', '%{public}s', 'Ability onBackground');
    ScreenAwake.setKeepScreenOn(false);
  }
}
