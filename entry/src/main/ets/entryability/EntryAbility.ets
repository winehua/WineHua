import { AbilityConstant, ConfigurationConstant, UIAbility, Want } from '@kit.AbilityKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { window } from '@kit.ArkUI';
import { deviceInfo } from '@kit.BasicServicesKit';
import { WineWindowManager } from '../service/WineWindowManager';
import { DisplaySettingsService } from '../service/DisplaySettingsService';
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

  // 发布正常会话所需的最小启动状态: want 开屏参数里的 d3d/dxvk 覆盖项优先
  // (winehua.d3d_backend/winehua.dxvk_backend), 否则按持久化 preference →
  // AppStorage 生效值 (Index 的 @StorageLink 'winehua.d3d.backend'/'winehua.dxvk.
  // backend' 与 WineEnvService.updateLaunchRequest 依赖)。自动化 Want 协议
  // (smoke/game/experiment/click/perf-profile) 已随 smoke 设施整体拆除,
  // 重建后按 docs/SMOKE_INFRASTRUCTURE.md §4 的三层设计重新引入。
  private publishLaunchRequest(want: Want): void {
    const parameters = want.parameters;
    const value = (key: string, fallback: string): string => {
      if (!parameters || parameters[key] === undefined || parameters[key] === null) return fallback;
      return `${parameters[key]}`;
    };
    const explicitD3d = !!parameters && parameters['winehua.d3d_backend'] !== undefined &&
      parameters['winehua.d3d_backend'] !== null;
    const persistedD3dRaw = AppStorage.get<string>('winehua.d3d.preference') || 'dxvk_modern_2_6';
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
    hilog.info(DOMAIN, 'testTag', 'publish d3d=%{public}s dxvk=%{public}s explicit=%{public}d',
      d3dBackend, dxvkBackend, explicitD3d ? 1 : 0);
  }

  onCreate(want: Want, launchParam: AbilityConstant.LaunchParam): void {
    // d3d/dxvk 偏好的读回不在本文 (与 Wine 语言同款): PersistentStorage
    // 冷启动不恢复磁盘值 (实测, 见 WineEnvService 注释), 由 WineEnvService.init
    // 用 preferences API 读盘后覆盖 AppStorage, publishLaunchRequest 此处只
    // 负责"显式 want 覆盖 > 持久化偏好 > 默认值"协议与 dxvk 联动
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
    // 桌面还原不走 master 的 restoreDesktopIfRunning (DesktopAbility 已随
    // 悬浮窗重构删除): Pad/phone 桌面即主窗口内 DesktopLayer, resume 自动
    // 回前台; PC 独立虚拟桌面窗口由用户点悬浮窗打开
  }

  onDestroy(): void {
    hilog.info(DOMAIN, 'testTag', '%{public}s', 'Ability onDestroy');
  }

  onWindowStageCreate(windowStage: window.WindowStage): void {
    // Main window is created, set main page for this ability
    hilog.info(DOMAIN, 'testTag', '%{public}s', 'Ability onWindowStageCreate');

    // 手机走 fork-shim，必须在 WineEnvService.startSession 的首次 NCP 调用前设置。
    testNapi.setPhoneMode(deviceInfo.deviceType === 'phone');

    // Phone desktop is rendered in a full-window XComponent. Force the window
    // to landscape (module.json5 orientation=landscape 亦静态锁定), so the
    // surface callback hands Wine a landscape rect before the first frame is
    // composited — 手机/pad 都保持横屏, 不随传感器变竖屏。
    if (deviceInfo.deviceType === 'phone') {
      try {
        const mainWindow = windowStage.getMainWindowSync();
        mainWindow.setPreferredOrientation(window.Orientation.LANDSCAPE).then(() => {
          hilog.info(DOMAIN, 'testTag', 'phone forced landscape');
        }).catch((err: Error) => {
          hilog.warn(DOMAIN, 'testTag', 'phone forced landscape failed: %{public}s', JSON.stringify(err));
        });
      } catch (err) {
        hilog.warn(DOMAIN, 'testTag', 'phone forced landscape setup failed: %{public}s', JSON.stringify(err));
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
        // 保持屏幕常亮 (设置项): 启动时应用持久化值;
        // 引用存入 AppStorage 供设置页切换时立即应用 (getLastWindow 多窗口下不可靠)
        mainWin.setWindowKeepScreenOn(DisplaySettingsService.getInstance().keepScreenOn)
          .catch((e: Object) => {
            hilog.warn(DOMAIN, 'testTag', 'setWindowKeepScreenOn fail: %{public}s', JSON.stringify(e));
          });
        AppStorage.setOrCreate<window.Window>('winehua.mainWindow', mainWin);
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
