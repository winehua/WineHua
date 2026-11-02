import { AbilityConstant, UIAbility, Want } from '@kit.AbilityKit';
import { DEFAULT_D3D_BACKEND } from '../common/GraphicsDefaults';
import { hasQualifiedMewgenicsGpu } from '../common/MewgenicsGraphics';
import { PhoneGraphicsSettings } from '../service/PhoneGraphicsSettings';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { window } from '@kit.ArkUI';
import { deviceInfo } from '@kit.BasicServicesKit';
import { WineWindowManager } from '../service/WineWindowManager';
import { DisplaySettingsService } from '../service/DisplaySettingsService';
import { WineEnvService, FILES_BASE, readTextIfExists } from '../service/WineEnvService';
import { DesktopModeService } from '../service/DesktopModeService';
import { ThemeService } from '../service/ThemeService';
import { AppLibraryService } from '../service/AppLibraryService';
import { DisplayRouteService } from '../service/DisplayRouteService';
import { InputSettingsService } from '../service/InputSettingsService';
import { ProcessService } from '../service/ProcessService';
import { SmokeHook } from '../smoke/SmokeHook';
import testNapi from 'libentry.so';

const DOMAIN = 0x0000;

export default class EntryAbility extends UIAbility {
  // 默认档统一 DXVK 1.10 (DEFAULT_D3D_BACKEND); 2.6 仅在用户显式选择时启用。
  // 停用了原"设备审计白名单" (VYG-AL00 26.0.0.32 自动 2.6, 否则 1.10.3):
  // 它服务于"默认 2.6"时期, 默认档反转后自动上浮已无意义, 统一回落 1.10。
  // 显示路线首施标记: onCreate 应用缺省/显式值后置位, onNewWant 无参重拉
  // 不再重复应用 (见 publishLaunchRequest 的 route 段注释)
  private displayRouteApplied: boolean = false;
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
      hostProfile === 'shadow-precise-legacy-host-sync' || hostProfile === 'shadow-precise' ||
      hostProfile === 'baseline' ? hostProfile : '');
    // Pad 多窗口承载调试键: winehua.fusionHost (subwindow|ability) 跨设备强制
    // subWindow 承载 (2in1 真机验证 subWindow 路线用, 见 WineWindowManager.
    // fusionUsesSubWindow)。仅显式 want 生效, 不持久化。
    const explicitFusionHost = !!parameters && parameters['winehua.fusionHost'] !== undefined &&
      parameters['winehua.fusionHost'] !== null;
    if (explicitFusionHost) {
      // M4d-review I2 实测: 原 AppStorage.set 对不存在的键是静默 no-op（全仓
      // 无人 setOrCreate/'@StorageLink' 建过该键），调试键从未生效 —— 2026-10-11
      // Pad 真机: --ps winehua.fusionHost ability 冷启后 FWM 仍建 subWindow。
      // 改 setOrCreate（与上方 d3d/dxvk.backend 同款双写模式）。
      AppStorage.setOrCreate<string>('winehua.fusionHost', value('winehua.fusionHost', ''));
    }
    // [[任务#129 装饰实验]] 裸 decor subWindow 测试开关 (消费在
    // onWindowStageCreate 的 loadContent 回调, 仅显式 want 生效不持久化)
    const explicitTestDecor = !!parameters && parameters['winehua.testDecor'] !== undefined &&
      parameters['winehua.testDecor'] !== null;
    if (explicitTestDecor) {
      AppStorage.setOrCreate<string>('winehua.testDecor', value('winehua.testDecor', ''));
    }
    // 桌面模式显式覆盖 (fusion|virtual, 设置页切换前的测试通道): 仅在
    // applyModePolicy 判定读取, 不持久化 (重启引擎场景仍按持久化值)。
    // 经 DesktopModeService 单例传递 — AppStorage 读取实测不可靠。
    const explicitDesktopMode = !!parameters && parameters['winehua.desktopMode'] !== undefined &&
      parameters['winehua.desktopMode'] !== null;
    if (explicitDesktopMode) {
      DesktopModeService.getInstance().setOverrideMode(value('winehua.desktopMode', ''));
    }
    // 显示路线 (wayland|x11): 优先级 = 显式 want 覆盖 > 设置页持久化选择
    // > 缺省 x11 (2026-10-10 拍板 x11 为主线; 2026-10-11 起设置页「显示路线
    // （实验）」可选并持久化, UI 可见故无僵尸态; --ps winehua.displayRoute
    // 仍为自动化最高优先)。消费点白名单见 DisplayRouteService 头注释。
    // native 镜像同步写一次 (guest env stamp 的唯一读点, 见 wine_env.cpp)。
    DisplayRouteService.getInstance().init(this.context);
    // 桌面模式持久化值必须在下方 shouldUseMultiwindow() 查询前就绪：x11 链
    // 早启用它的返回值决定 native multiwindow 位，而 WineWindowManager
    // .init 里的 init 在此之后才跑（2026-10-11 实测时序：displayRoute applied
    // 40.756 早于 mode policy 40.782，26ms）。不提前的后果：查询读到
    // DEFAULT_MODE（tablet='virtual'）→ chain_start multiwindow=false → 会话
    // 落虚拟桌面链，而 ArkTS 实时查询（此时持久化已加载='fusion'）判 fusion
    // 不开画布 → 程序窗走 g_clients 画布路径却无宿主，窗口不可见（用户实测：
    // 文件管理启动 explorer 窗口不出；smoke 冷启带显式 winehua.desktopMode
    // 的 setOverrideMode 在查询前执行，故绕开了此时序——两条路径差异即此）。
    DesktopModeService.getInstance().init(this.context);
    const explicitDisplayRoute = !!parameters && parameters['winehua.displayRoute'] !== undefined &&
      parameters['winehua.displayRoute'] !== null;
    const routeValue = explicitDisplayRoute ? value('winehua.displayRoute', '')
      : DisplayRouteService.getInstance().preferredRoute();
    // 本函数 onCreate/onNewWant 双入口: 缺省 x11 只在首施时应用 (无参重拉
    // 不得把显式切到 wayland 的运行中会话拉回缺省); 带显式参数时随时应用
    // (切换/测试通道)。应用动作全部收口 DisplayRouteService.applyRoute
    // (#172 单点: 盖 env 章 + 拆旧 x11 链 + 起 x11 链), 本处只做解析与首施门。
    if (explicitDisplayRoute || !this.displayRouteApplied) {
      this.displayRouteApplied = true;
      try {
        // x11 链起尺寸 = 桌面分辨率 (屏幕/scale, 与 wayland setOutputSize
        // 同判据 — WineWindowManager.desktopOutputSize 唯一定义处)
        const outSize = WineWindowManager.getInstance().desktopOutputSize();
        DisplayRouteService.getInstance().applyRoute(routeValue, 'entry',
          WineWindowManager.getInstance().shouldUseMultiwindow(routeValue),
          outSize[0], outSize[1]);
      } catch (e) {
        // 不得吞异常: 印章/起链失败 = 后续 wine 进程全部走错路线且零痕迹
        // (2026-10-04 xwedge3 实测: 排查多耗一轮构建)
        hilog.error(DOMAIN, 'testTag', 'displayRoute apply FAILED: %{public}s %{public}s',
          routeValue, JSON.stringify(e));
      }
    }
    // 自动化通道 (UI 点击/输入不可注入设备的全自动验证):
    // winehua.autoStart=1 → 主页面就绪后自动拉起引擎 (一次消费)
    if (parameters && parameters['winehua.autoStart'] === '1') {
      WineEnvService.getInstance().autoStartPending = true;
    }
    // 自动化测试通道: winehua.program=<wine路径> → 引擎就绪后自动启动该程序
    if (parameters && parameters['winehua.program'] !== undefined &&
      parameters['winehua.program'] !== null) {
      WineEnvService.getInstance().setAutoProgram(value('winehua.program', ''));
      AppStorage.setOrCreate<boolean>('winehua.program.openglDiag', false);
      AppStorage.set<boolean>('winehua.program.openglDiag', value('winehua.openglDiag', '') === '1');
      AppStorage.setOrCreate<string>('winehua.program.envFile', '');
      AppStorage.set<string>('winehua.program.envFile', value('winehua.program_env_file', ''));
    }
    hilog.info(DOMAIN, 'testTag', 'publish d3d=%{public}s dxvk=%{public}s explicit=%{public}d',
      d3dBackend, dxvkBackend, explicitD3d ? 1 : 0);
    // winehua.env=<K=V;K=V> → 直启程序追加环境 (调试通道, 见 runProgram)
    if (parameters && parameters['winehua.env'] !== undefined &&
      parameters['winehua.env'] !== null) {
      AppStorage.setOrCreate<string>('winehua.env', value('winehua.env', ''));
    }
    // want 的模式/套件/前缀等 smoke 参数解析在 SmokeHook, 模式非 smoke 时零行为
    SmokeHook.getInstance().applyWant(parameters, this.context);
  }

  onCreate(want: Want, launchParam: AbilityConstant.LaunchParam): void {
    // d3d/dxvk 偏好的读回不在本文 (与 Wine 语言同款): PersistentStorage
    // 冷启动不恢复磁盘值 (实测, 见 WineEnvService 注释), 由 WineEnvService.init
    // 用 preferences API 读盘后覆盖 AppStorage, publishLaunchRequest 此处只
    // 负责"显式 want 覆盖 > 持久化偏好 > 默认值"协议与 dxvk 联动
    // 外观设置 (浅色/深色/跟随系统) 在此只读盘, 不在此下发: setColorMode 要求
    // 窗口已创建且页面 loadContent 完成 (API 文档), 实际下发见 onWindowStageCreate
    ThemeService.getInstance().init(this.context);
    PhoneGraphicsSettings.getInstance().init(this.context);
    InputSettingsService.getInstance().init(this.context);
    this.publishLaunchRequest(want);
    hilog.info(DOMAIN, 'testTag', '%{public}s', 'Ability onCreate');
  }

  onNewWant(want: Want, launchParam: AbilityConstant.LaunchParam): void {
    this.publishLaunchRequest(want);
    WineEnvService.getInstance().onNewWant();
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
        // 外层重试只是兜底 ensureEngineReady 返回 false 的情形 (busy/failed);
        // 引擎 starting 时 ensureEngineReady 自己会 restartEngine 并等 300s。
        // 上限与其 deadline 对齐: 60s 曾早于全新安装首启实测耗时 (74s,
        // 2026-10-06 23:20:39 'skip' 复现), 直启被静默跳过。
        if (++tries < 300) {
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
    // winehua.env=<K=V;K=V>: 直启程序的追加环境。调试通道 (D50 WINEDEBUG
    // 等排查), 非产品语义 —— ';' 分隔, 值内不容 ';'。
    const environment: Record<string, string> = {};
    const envSpec = AppStorage.get<string>('winehua.env') ?? '';
    for (const pair of envSpec.split(';')) {
      const eq = pair.indexOf('=');
      if (eq > 0) {
        environment[pair.substring(0, eq)] = pair.substring(eq + 1);
      }
    }
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

  onDestroy(): void {
    hilog.info(DOMAIN, 'testTag', '%{public}s', 'Ability onDestroy');
  }

  onWindowStageCreate(windowStage: window.WindowStage): void {
    // Main window is created, set main page for this ability
    hilog.info(DOMAIN, 'testTag', '%{public}s', 'Ability onWindowStageCreate');

    // 手机走 fork-shim，必须在 WineEnvService.startSession 的首次 NCP 调用前设置。
    testNapi.setPhoneMode(deviceInfo.deviceType === 'phone', hasQualifiedMewgenicsGpu());

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
      // [[任务#129 装饰实验]] winehua.testDecor=1 → 弹两个带真实内容的装饰
      // 测试窗对照 (无内容子窗整窗不合成 —— 前几轮「shown 但不可见」即此):
      // A) stage 子窗 createSubWindowWithOptions(decorEnabled:true)
      // B) window.createWindow TYPE_DIALOG + decorEnabled:true
      // decor 参数之外只有让窗可见的最小配置 (尺寸/位置/静态内容页)。
      // 测试代码, 结论落地后随 DecorTestPage 一并删除。
      if (AppStorage.get<string>('winehua.testDecor') === '1') {
        windowStage.createSubWindowWithOptions('decor_test', {
          title: 'DECOR TEST',
          decorEnabled: true
        }).then(async (sw: window.Window) => {
          hilog.info(DOMAIN, 'testTag', '[DECOR-TEST] A stage subWindow created id=%{public}d', sw.getWindowProperties().id);
          await sw.resize(700, 500);
          await sw.moveWindowToGlobal(200, 300);
          await sw.loadContent('pages/DecorTestPage');
          await sw.showWindow();
          hilog.info(DOMAIN, 'testTag', '[DECOR-TEST] A stage subWindow shown');
        }).catch((e: Error) => {
          hilog.error(DOMAIN, 'testTag', '[DECOR-TEST] A FAILED: %{public}s', JSON.stringify(e));
        });
        window.createWindow({
          name: 'decor_test_dialog',
          windowType: window.WindowType.TYPE_DIALOG,
          ctx: this.context,
          decorEnabled: true,
          title: 'DIALOG DECOR TEST'
        } as window.Configuration).then(async (dw: window.Window) => {
          hilog.info(DOMAIN, 'testTag', '[DECOR-TEST] B dialog created id=%{public}d', dw.getWindowProperties().id);
          await dw.resize(700, 500);
          await dw.moveWindowToGlobal(900, 700);
          await dw.loadContent('pages/DecorTestPage');
          await dw.showWindow();
          hilog.info(DOMAIN, 'testTag', '[DECOR-TEST] B dialog shown');
        }).catch((e: Error) => {
          hilog.error(DOMAIN, 'testTag', '[DECOR-TEST] B FAILED: %{public}s', JSON.stringify(e));
        });
      }
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
