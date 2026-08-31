import { AbilityConstant, ConfigurationConstant, UIAbility, Want } from '@kit.AbilityKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { window } from '@kit.ArkUI';
import { deviceInfo } from '@kit.BasicServicesKit';
import { WineWindowManager } from '../service/WineWindowManager';
import { WineEnvService } from '../service/WineEnvService';
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
    const persistedD3dRaw = AppStorage.get<string>('winehua.d3d.preference') || 'vkd3d_limited_500k';
    const requestedD3d = explicitD3d ? value('winehua.d3d_backend', 'wined3d') : persistedD3dRaw;
    const d3dBackend = requestedD3d === 'dxvk_legacy' ||
      requestedD3d === 'dxvk_modern_2_6' || requestedD3d === 'vkd3d_limited_500k' ?
      requestedD3d : 'wined3d';
    const explicitDxvk = !!parameters && parameters['winehua.dxvk_backend'] !== undefined &&
      parameters['winehua.dxvk_backend'] !== null;
    const persistedDxvk = AppStorage.get<string>('winehua.dxvk.preference') || 'auto';
    let dxvkBackend = this.resolveDxvkBackend(explicitDxvk ?
      value('winehua.dxvk_backend', 'auto') : persistedDxvk);
    if (d3dBackend === 'dxvk_legacy') dxvkBackend = 'dxvk_legacy';
    if (d3dBackend === 'dxvk_modern_2_6') dxvkBackend = 'dxvk_modern_2_6';
    AppStorage.setOrCreate<string>('winehua.d3d.backend', d3dBackend);
    AppStorage.set<string>('winehua.d3d.backend', d3dBackend);
    AppStorage.setOrCreate<string>('winehua.dxvk.backend', dxvkBackend);
    AppStorage.set<string>('winehua.dxvk.backend', dxvkBackend);
    // [[SMOKE]] 合并到 main-ui: 删本行 (want 的模式/套件/前缀等 smoke 参数解析
    // 在 SmokeHook, 模式非 smoke 时零行为)
    SmokeHook.getInstance().applyWant(parameters);
  }

  onCreate(want: Want, launchParam: AbilityConstant.LaunchParam): void {
    PersistentStorage.persistProp<string>('winehua.d3d.preference', 'vkd3d_limited_500k');
    PersistentStorage.persistProp<string>('winehua.dxvk.preference', 'auto');
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
    WineWindowManager.getInstance().restoreDesktopIfRunning();
    WineEnvService.getInstance().onNewWant();
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
      }
      hilog.info(DOMAIN, 'testTag', 'Succeeded in loading the content.');
    });
  }

  onWindowStageDestroy(): void {
    // Main window is destroyed, release UI related resources
    hilog.info(DOMAIN, 'testTag', '%{public}s', 'Ability onWindowStageDestroy');
  }

  onForeground(): void {
    // Ability has brought to foreground
    hilog.info(DOMAIN, 'testTag', '%{public}s', 'Ability onForeground');
  }

  onBackground(): void {
    // Ability has back to background
    hilog.info(DOMAIN, 'testTag', '%{public}s', 'Ability onBackground');
  }
}
