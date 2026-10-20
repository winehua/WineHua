import { AbilityConstant, ConfigurationConstant, UIAbility, Want } from '@kit.AbilityKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { window } from '@kit.ArkUI';
import { deviceInfo } from '@kit.BasicServicesKit';
import { WineWindowManager } from '../service/WineWindowManager';
import { WineEnvService } from '../service/WineEnvService';
import { DesktopModeService } from '../service/DesktopModeService';
import { DisplayRouteService } from '../service/DisplayRouteService';

// [[SMOKE]] 合并到 main-ui: 删本行与 publishLaunchRequest 内的 applyWant 调用
import { SmokeHook } from '../smoke/SmokeHook';
import testNapi from 'libentry.so';

const DOMAIN = 0x0000;

export default class EntryAbility extends UIAbility {
  // 显示路线首施标记: onCreate 应用缺省/显式值后置位, onNewWant 无参重拉
  // 不再重复应用 (见 publishLaunchRequest 的 route 段注释)
  private displayRouteApplied: boolean = false;
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
    /* 产品默认档 = DXVK 2.6.2 (2026-10-11 用户拍板, 原 vkd3d_limited_500k
     * 组合降为手动可选)。选 dxvk 档时 dxvk 轴随之强制 modern (见下方),
     * dxvk 轴的 auto 白名单仅在 vkd3d 组合档下参与解析。 */
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
    // Pad 多窗口承载调试键: winehua.fusionHost (subwindow|ability) 跨设备强制
    // subWindow 承载 (见 WineWindowManager.fusionUsesSubWindow)。仅显式 want
    // 生效, 不持久化。
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
    // 桌面模式显式覆盖 (fusion|virtual): 仅在 applyModePolicy 判定读取, 不持久化
    // (重启引擎场景仍按持久化值)。经 DesktopModeService 单例传递 — AppStorage
    // 跨 Ability 上下文读取不可靠 (见 DesktopModeService.setOverrideMode)。
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
    // winehua.program=<wine 路径> → 引擎就绪后自动启动该程序
    if (parameters && parameters['winehua.program'] !== undefined &&
      parameters['winehua.program'] !== null) {
      WineEnvService.getInstance().setAutoProgram(value('winehua.program', ''));
    }
    // winehua.env=<K=V;K=V> → 直启程序追加环境 (调试通道, 见 runProgram)
    if (parameters && parameters['winehua.env'] !== undefined &&
      parameters['winehua.env'] !== null) {
      AppStorage.setOrCreate<string>('winehua.env', value('winehua.env', ''));
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
    testNapi.runWineProgram({
      windowsExePath: windowsPath,
      argv: [],
      environment: environment,
      workingDirectory: dir,
      d3dBackend: AppStorage.get<string>('winehua.d3d.backend') ?? 'dxvk_modern_2_6',
      dxvkBackend: AppStorage.get<string>('winehua.dxvk.backend') ?? 'auto'
    });
  }

  onCreate(want: Want, launchParam: AbilityConstant.LaunchParam): void {
    PersistentStorage.persistProp<string>('winehua.d3d.preference', 'dxvk_modern_2_6');
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
      }
      hilog.info(DOMAIN, 'testTag', 'Succeeded in loading the content.');
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
    // Ability has brought to foreground
    hilog.info(DOMAIN, 'testTag', '%{public}s', 'Ability onForeground');
  }

  onBackground(): void {
    // Ability has back to background
    hilog.info(DOMAIN, 'testTag', '%{public}s', 'Ability onBackground');
  }
}
