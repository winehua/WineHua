import { AbilityConstant, UIAbility, Want } from '@kit.AbilityKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { window } from '@kit.ArkUI';
import { deviceInfo } from '@kit.BasicServicesKit';
import { WineWindowManager } from '../service/WineWindowManager';
import { DEFAULT_D3D_BACKEND, WineEnvService } from '../service/WineEnvService';
import { DisplaySettingsService } from '../service/DisplaySettingsService';
import { DesktopModeService } from '../service/DesktopModeService';
import { ThemeService } from '../service/ThemeService';
import { AppLibraryService } from '../service/AppLibraryService';
import testNapi from 'libentry.so';

const DOMAIN = 0x0000;

export default class EntryAbility extends UIAbility {
  // 默认档统一 DXVK 1.10 (DEFAULT_D3D_BACKEND); 2.6 仅在用户显式选择时启用。
  // 停用了原"设备审计白名单" (VYG-AL00 26.0.0.32 自动 2.6, 否则 1.10.3):
  // 它服务于"默认 2.6"时期, 默认档反转后自动上浮已无意义, 统一回落 1.10。
  private resolveDxvkBackend(value: string): string {
    if (value === 'dxvk_legacy' || value === 'dxvk_modern_2_6') return value;
    return DEFAULT_D3D_BACKEND;
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
    const persistedD3dRaw = AppStorage.get<string>('winehua.d3d.preference') || DEFAULT_D3D_BACKEND;
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
    // Pad 多窗口承载调试键: winehua.fusionHost (subwindow|ability) 跨设备强制
    // subWindow 承载 (2in1 真机验证 subWindow 路线用, 见 WineWindowManager.
    // fusionUsesSubWindow)。仅显式 want 生效, 不持久化。
    const explicitFusionHost = !!parameters && parameters['winehua.fusionHost'] !== undefined &&
      parameters['winehua.fusionHost'] !== null;
    if (explicitFusionHost) {
      AppStorage.set<string>('winehua.fusionHost', value('winehua.fusionHost', ''));
    }
    // 桌面模式显式覆盖 (fusion|virtual, 设置页切换前的测试通道): 仅在
    // applyModePolicy 判定读取, 不持久化 (重启引擎场景仍按持久化值)。
    // 经 DesktopModeService 单例传递 — AppStorage 读取实测不可靠。
    const explicitDesktopMode = !!parameters && parameters['winehua.desktopMode'] !== undefined &&
      parameters['winehua.desktopMode'] !== null;
    if (explicitDesktopMode) {
      DesktopModeService.getInstance().setOverrideMode(value('winehua.desktopMode', ''));
    }
    // 自动化测试通道: winehua.autoStart=1 → 主页面就绪后自动拉起引擎
    // (UI 点击不可注入的设备上验证用, 一次消费)
    if (parameters && parameters['winehua.autoStart'] === '1') {
      WineEnvService.getInstance().autoStartPending = true;
    }
    // 自动化测试通道: winehua.program=<wine路径> → 引擎就绪后自动启动该程序
    if (parameters && parameters['winehua.program'] !== undefined &&
      parameters['winehua.program'] !== null) {
      WineEnvService.getInstance().setAutoProgram(value('winehua.program', ''));
    }
    hilog.info(DOMAIN, 'testTag', 'publish d3d=%{public}s dxvk=%{public}s explicit=%{public}d',
      d3dBackend, dxvkBackend, explicitD3d ? 1 : 0);
  }

  onCreate(want: Want, launchParam: AbilityConstant.LaunchParam): void {
    // d3d/dxvk 偏好的读回不在本文 (与 Wine 语言同款): PersistentStorage
    // 冷启动不恢复磁盘值 (实测, 见 WineEnvService 注释), 由 WineEnvService.init
    // 用 preferences API 读盘后覆盖 AppStorage, publishLaunchRequest 此处只
    // 负责"显式 want 覆盖 > 持久化偏好 > 默认值"协议与 dxvk 联动
    // 外观设置 (浅色/深色/跟随系统) 在此只读盘, 不在此下发: setColorMode 要求
    // 窗口已创建且页面 loadContent 完成 (API 文档), 实际下发见 onWindowStageCreate
    ThemeService.getInstance().init(this.context);
    this.publishLaunchRequest(want);
    hilog.info(DOMAIN, 'testTag', '%{public}s', 'Ability onCreate');
  }

  onNewWant(want: Want, launchParam: AbilityConstant.LaunchParam): void {
    this.publishLaunchRequest(want);
    // 桌面还原不走 master 的 restoreDesktopIfRunning (DesktopAbility 已随
    // 悬浮窗重构删除): Pad/phone 桌面即主窗口内 DesktopLayer, resume 自动
    // 回前台; PC 独立虚拟桌面窗口由用户点悬浮窗打开
    // 自动化测试通道 (运行中会话): winehua.program → 直启该程序 (warm
    // ensureEngineReady 直返, 不重启会话)
    this.autoRunIfRequested();
  }

  // 自动化测试通道: winehua.autoStart=1 (冷启动) / winehua.program=<wine路径>
  // (冷启动或运行中) → 引擎就绪后自动启动程序。UI 点击不可注入的设备上
  // 全自动验证用; 单次消费 (autoProgram 消费一次后清空)。冷启动时 loadContent
  // 回调早于引擎初始化 (wineboot 数秒), 故轮询等待就绪 — 只服务自动化。
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
            AppLibraryService.getInstance().launch(prog);
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
      // 外观设置下发: 此处才满足 setColorMode 的前置条件 (页面已加载);
      // 幂等, 切换时由设置页 ThemeService.setMode 各自触发
      ThemeService.getInstance().apply();
      // 自动化测试通道 (冷启动): 主页面就绪后可自动拉起引擎/程序
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
  }

  onBackground(): void {
    // Ability has back to background
    hilog.info(DOMAIN, 'testTag', '%{public}s', 'Ability onBackground');
  }
}
