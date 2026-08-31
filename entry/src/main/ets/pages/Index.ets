import { hilog } from '@kit.PerformanceAnalysisKit';
import testNapi from 'libentry.so';
import { fileIo as fs } from '@kit.CoreFileKit';
import { BusinessError, deviceInfo } from '@kit.BasicServicesKit';
import { inputMethod } from '@kit.IMEKit';
import { SegmentButton, SegmentButtonOptions } from '@kit.ArkUI';
import { WineEnvService } from '../service/WineEnvService';
import { ProcessService, ProcessInfo } from '../service/ProcessService';
import { WineWindowManager } from '../service/WineWindowManager';
import { DesktopModeService, MODE_VIRTUAL, MODE_FUSION } from '../service/DesktopModeService';
import { SmokeRunner } from '../service/SmokeRunner';

const DOMAIN = 0x0000;

// 文件浏览器以 C: 盘为根 (与 WineEnvService 的 DRIVE_C 同值)
const DRIVE_C = '/data/storage/el2/base/files/.wine/drive_c';

@Entry
@Component
struct Index {
  @StorageLink('winehua.d3d.backend') d3dBackendText: string = 'vkd3d_limited_500k';
  @StorageLink('winehua.d3d.preference') d3dPreferenceText: string = 'vkd3d_limited_500k';
  @StorageLink('winehua.dxvk.backend') dxvkBackendText: string = 'dxvk_legacy';
  @StorageLink('winehua.dxvk.preference') dxvkPreferenceText: string = 'auto';
  private env = WineEnvService.getInstance();
  // -- 页面镜像 (WineEnvService notify 驱动) --
  @State state: string = 'idle';
  // 会话存活派生 (desktopActive || 用户进程在跑, service 内重算) —
  // "运行中/已就绪"文案与配色判据 (对齐 main-ui 状态机设计)
  @State sessionAlive: boolean = false;
  @State prefixReady: boolean = false;
  @State statusText: string = '检测中...';
  @State initOverlayVisible: boolean = false;
  @State initOverlayText: string = '';
  @State initFailed: boolean = false;
  @State initBusy: boolean = false;
  @State opBusy: boolean = false;
  @State upgradePending: boolean = false;
  // -- 文件浏览器 --
  @State curDir: string = DRIVE_C;
  @State entries: string[] = [];
  @State dirStack: string[] = [];
  // -- 运行中进程 (ProcessService 镜像) --
  @State processList: ProcessInfo[] = [];
  // -- smoke 回归 (SmokeRunner 镜像: 侧边栏开发测试区) --
  @State smokeStatus: string = '';
  @State smokeResult: string = '';
  // -- 桌面模式 (仅 PC): 融合/虚拟桌面设置 (镜像 DesktopModeService) --
  @State desktopModeSel: number[] = [0];
  @State desktopMode: string = MODE_FUSION;
  @State desktopModeRestartHint: boolean = false;
  // -- PC 虚拟桌面窗口状态 (镜像 WineWindowManager) --
  @State desktopActive: boolean = false;
  @State desktopWindowActive: boolean = false;
  private wm: WineWindowManager = WineWindowManager.getInstance();
  private d3dBackend: string = 'vkd3d_limited_500k';
  private dxvkBackend: string = 'dxvk_legacy';
  // 升级提示日志只打一次 (master 版无弹卡 UI, 检测到不一致仅留痕 +
  // 快速操作按钮转为"应用更新"语义, 见 quickStart)
  private upgradeLogged: boolean = false;
  // -- IME 桥接 (Wine 文本框聚焦 → attach 弹软键盘 → insertText 注入 Wine) --
  private imeController: inputMethod.InputMethodController | null = null;

  // 注册 C++ 侧激活回调: active=1 → attach 弹键盘, active=0 → 收起
  private setupIme(): void {
    testNapi.setImeCallback((active: number, x: number, y: number, w: number, h: number) => {
      hilog.info(DOMAIN, 'IME-TEST', '[cb] active=%{public}d rect=[%{public}d,%{public}d %{public}dx%{public}d]',
        active, x, y, w, h);
      if (active === 1) {
        this.imeAttach();
      } else {
        this.imeDetach();
      }
    });
  }

  private imeAttach(): void {
    hilog.info(DOMAIN, 'IME-TEST', '[step] imeAttach begin');
    try {
      const controller = inputMethod.getController();
      this.imeController = controller;
      // 每次激活都会 attach; 先清掉可能已注册的回调, 否则 on() 累积
      // 导致一个 insertText 触发多次注入 (重复字符)
      controller.off('insertText');
      controller.off('deleteLeft');
      controller.off('deleteRight');
      controller.off('sendKeyboardStatus');
      controller.off('setPreviewText');
      controller.off('sendFunctionKey');
      const attr: inputMethod.InputAttribute = {
        textInputType: inputMethod.TextInputType.TEXT,
        // NEWLINE (API 12+): 软键盘回车显示"换行"键 (DONE 显示"完成")。
        // 实测回车不发 insertText('\n') — 输入法始终走 sendFunctionKey 功能键
        // (enterKeyType=8), 且系统吞 KeyDown 只派发 KeyUp; 换行由
        // WineWindow.ets onKeyEvent 补发 down+up 兜底 (见该处注释)
        enterKeyType: inputMethod.EnterKeyType.NEWLINE
      };
      const cfg: inputMethod.TextConfig = { inputAttribute: attr };
      controller.attach(true, cfg).then(() => {
        hilog.info(DOMAIN, 'IME-TEST', '[attach] OK, registering on() callbacks');
        // attach 成功后注册事件 (12800009 detached 说明 on() 须在绑定后注册)
        controller.on('insertText', (text: string) => {
          hilog.info(DOMAIN, 'IME-TEST', '[insertText] "%{public}s"', text);
          testNapi.sendImeCommit(text);   // 提交文本 → Wine commit_string
        });
        controller.on('deleteLeft', (len: number) => {
          hilog.info(DOMAIN, 'IME-TEST', '[deleteLeft] len=%{public}d', len);
          testNapi.imeBackspace();        // 退格 → Wine KEY_BACKSPACE
        });
        controller.on('deleteRight', (len: number) => {
          hilog.info(DOMAIN, 'IME-TEST', '[deleteRight] len=%{public}d', len);
          testNapi.imeBackspace();
        });
        controller.on('sendKeyboardStatus', (status: inputMethod.KeyboardStatus) => {
          hilog.info(DOMAIN, 'IME-TEST', '[keyboardStatus] %{public}d (NONE=0 HIDE=1 SHOW=2)', status);
        });
        controller.on('setPreviewText', (text: string, range: inputMethod.Range) => {
          hilog.info(DOMAIN, 'IME-TEST', '[previewText] "%{public}s" range=[%{public}d,%{public}d]', text, range.start, range.end);
          testNapi.sendImePreedit(text, range.start, range.end);  // 预上屏 → Wine preedit_string
        });
        // 回车补齐在 WineWindow.ets onKeyEvent 统一兜底 (系统吞 KeyDown 只派
        // KeyUp 时补发 down+up, 覆盖 IME 激活/非激活所有场景), 此处只留日志
        controller.on('sendFunctionKey', (fk: inputMethod.FunctionKey) => {
          hilog.info(DOMAIN, 'IME-TEST', '[sendFunctionKey] enterKeyType=%{public}d (0 UNSPEC 6 DONE 8 NEWLINE)',
            fk.enterKeyType);
        });
      }).catch((err: BusinessError) => {
        hilog.error(DOMAIN, 'IME-TEST', '[attach] FAILED code=%{public}d msg=%{public}s', err.code, err.message);
      });
    } catch (e) {
      hilog.error(DOMAIN, 'IME-TEST', '[step] EXCEPTION: %{public}s', JSON.stringify(e));
    }
  }

  private imeDetach(): void {
    hilog.info(DOMAIN, 'IME-TEST', '[step] imeDetach');
    const c = this.imeController;
    if (c) {
      try {
        c.off('insertText');
        c.off('deleteLeft');
        c.off('deleteRight');
        c.off('sendKeyboardStatus');
        c.off('setPreviewText');
        c.off('sendFunctionKey');
        c.hideTextInput();
        c.detach();
      } catch (e) {
        hilog.error(DOMAIN, 'IME-TEST', '[detach] EXCEPTION: %{public}s', JSON.stringify(e));
      }
      this.imeController = null;
    }
  }

  aboutToAppear(): void {
    this.d3dBackend = this.d3dBackendText === 'dxvk_legacy' ||
      this.d3dBackendText === 'dxvk_modern_2_6' ||
      this.d3dBackendText === 'vkd3d_limited_500k' ? this.d3dBackendText : 'wined3d';
    this.dxvkBackend = this.dxvkBackendText === 'dxvk_modern_2_6' ?
      'dxvk_modern_2_6' : 'dxvk_legacy';
    this.setupIme();
    const env = this.env;
    const mirror = (): void => {
      this.state = env.engineState;
      this.sessionAlive = env.sessionAlive;
      // prefixReady 跳变驱动文件区: 1.0.5 靠 refreshPrefix/waitForPrefix 在
      // 就绪时 listDir, engine service 化后该联动丢失 (回归) — 就绪瞬间
      // (含 aboutToAppear 时已就绪的首次镜像) 重置浏览位置并加载 C: 根目录;
      // 变为未就绪 (重置 Wine 删 prefix) 时清空, 避免展示已失效的目录内容
      const wasReady = this.prefixReady;
      this.prefixReady = env.prefixReady;
      if (this.prefixReady && !wasReady) {
        this.curDir = DRIVE_C;
        this.dirStack = [];
        this.listDir(DRIVE_C);
      } else if (!this.prefixReady) {
        this.entries = [];
      }
      this.statusText = env.statusText;
      this.initOverlayText = env.initOverlayText;
      this.initOverlayVisible = env.initOverlayVisible;
      this.initFailed = env.initFailed;
      this.initBusy = env.initBusy;
      this.opBusy = env.opBusy;
      this.upgradePending = env.upgradePending;
      if (env.upgradePending && !this.upgradeLogged) {
        this.upgradeLogged = true;
        hilog.warn(DOMAIN, 'wine',
          'upgrade pending: packaged wine runtime differs from extracted data. ' +
          'Use "重置 Wine" to apply the update (full factory reset).');
      }
    };
    env.subscribe(mirror);
    // 进程列表镜像 + 引擎事件驱动刷新 (WineEnvService 内部已绑定时序)
    const proc = ProcessService.getInstance();
    proc.subscribe(() => {
      this.processList = proc.processList;
    });
    proc.bindEnv(env);
    // PC 虚拟桌面窗口状态镜像 (WineWindowManager notify 驱动): desktopActive
    // / desktopWindowActive → 「打开桌面」按钮显隐 (关窗后台运行, 按钮复现)
    const wm = this.wm;
    wm.subscribe(() => {
      this.desktopActive = wm.desktopActive;
      this.desktopWindowActive = wm.isDesktopWindowActive();
    });
    // smoke 状态镜像 (侧边栏开发测试区): 运行中显示进度, 完成后显示 PASS/FAIL
    const smoke = SmokeRunner.getInstance();
    const mirrorSmoke = (): void => {
      this.smokeStatus = smoke.statusText();
      this.smokeResult = smoke.resultText();
    };
    smoke.subscribe(mirrorSmoke);
    mirrorSmoke();
    // 引擎初始化 (幂等): prefix 就绪自动拉起 (含升级检测拦截);
    // 首启无 prefix 自动 startSession (与 master 一致)
    env.init(getContext(this));
    mirror();
  }

  private selectD3dBackend = (backend: string): void => {
    const selected = backend === 'dxvk_legacy' || backend === 'dxvk_modern_2_6' ||
      backend === 'vkd3d_limited_500k' ? backend : 'wined3d';
    this.d3dBackend = selected;
    this.d3dBackendText = selected;
    this.d3dPreferenceText = selected;
    if (selected === 'dxvk_legacy' || selected === 'dxvk_modern_2_6') {
      this.dxvkBackend = selected;
      this.dxvkBackendText = selected;
      this.dxvkPreferenceText = selected;
    }
    this.env.setD3dPreference(selected);
    this.statusText = selected === 'dxvk_legacy' ? 'DXVK Legacy' :
      (selected === 'dxvk_modern_2_6' ? 'DXVK Modern 2.6' :
        (selected === 'vkd3d_limited_500k' ? 'VKD3D 2.6 Limited 500K' : 'WineD3D'));
    hilog.info(DOMAIN, 'GameTest', 'D3D backend selected=%{public}s', selected);
  }

  private selectDxvkBackend = (backend: string): void => {
    const selected = backend === 'dxvk_modern_2_6' ? 'dxvk_modern_2_6' : 'dxvk_legacy';
    this.dxvkBackend = selected;
    this.dxvkBackendText = selected;
    this.dxvkPreferenceText = selected;
    if (this.d3dBackend === 'wined3d' || this.d3dBackend.indexOf('dxvk_') === 0) {
      this.d3dBackend = selected;
      this.d3dBackendText = selected;
      this.d3dPreferenceText = selected;
    }
    this.env.setDxvkPreference(selected);
    this.statusText = selected === 'dxvk_modern_2_6' ?
      'D3D11 / DXGI: DXVK 2.6.2' : 'D3D11 / DXGI: DXVK 1.10.3';
  }

  // 桌面模式切换 (仅 PC): 写配置, 重启 Wine 引擎后生效 (explorer 是否启动/
  // 合成器策略在引擎启动时按此值判定, 运行中切换不影响当前会话)
  private selectDesktopMode(mode: string): void {
    if (mode == this.desktopMode) {
      return;
    }
    DesktopModeService.getInstance().setMode(mode);
    this.desktopMode = mode;
    this.desktopModeRestartHint = true;
  }

  // 设置项可见性: 功能仅对 PC 可用 (Pad/phone 恒虚拟桌面, 无融合模式可切)
  private isPcDevice(): boolean {
    const t = deviceInfo.deviceType;
    return t !== 'tablet' && t !== 'phone';
  }

  // 引擎操作统一入口: 忙时 (初始化/停止编排中) 由 WineEnvService 自身拦截。
  // 升级检测到不一致时 (upgradePending) 快速操作转为"应用更新" —
  // master 开发版无升级弹卡 UI, doReset 完整恢复出厂 (重装数据+清 C 盘)
  // 即 main-ui 升级卡"立即应用"同语义; 日常路径走 restartEngine 编排
  private quickStart = (): void => {
    if (this.upgradePending) {
      this.env.doReset();
    } else {
      this.env.restartEngine();
    }
  }

  // 重置确认弹窗 (对齐 main-ui 设置页): 重置会清空整个 Wine 容器, 误触不可逆
  private quickReset = (): void => {
    AlertDialog.show({
      title: '重置 Wine 引擎',
      message: '将清空整个 Wine 容器（包括所有已安装程序和存档），且不可恢复。确定继续？',
      primaryButton: {
        value: '取消',
        action: () => {}
      },
      secondaryButton: {
        value: '重置',
        fontColor: '#ff5555',
        action: () => {
          this.env.doReset();
        }
      }
    });
  }

  private quickRestart = (): void => {
    this.env.restartEngine();
  }

  // smoke 手动入口: 只跑 core 单套件 (固定帧视觉回归, 与脚本 --suite core 同链)。
  // 结果写到 C:\smoke\results\manual-<ts>\suite-summary.json, 界面镜像进度/汇总
  private startSmoke = (): void => {
    SmokeRunner.getInstance().start({
      suite: 'core',
      runId: `manual-${Date.now()}`,
      prefix: 'reuse',
      source: 'ui'
    });
  }

  private doRun = (name: string): void => {
    this.statusText = '启动中...';
    let exePath = this.curDir + '/' + name;
    testNapi.runWineProgram({
      windowsExePath: exePath,
      argv: [],
      // DXVK 稳定化 env 默认已收口 native (BuildSessionEnv stableDesktopOverlay,
      // 与桌面会话链同源), ArkTS 不再平行维护; 无 per-run 覆盖项时传空表
      environment: {},
      workingDirectory: this.curDir,
      d3dBackend: this.d3dBackend,
      dxvkBackend: this.dxvkBackend
      // presentBackend 不传: native 按 d3dBackend 派生 (DerivePresentBackend)
    });
  }

  private relPath = (): string => {
    if (this.curDir == DRIVE_C) return 'C:\\';
    return 'C:\\' + this.curDir.substring(DRIVE_C.length + 1).replace(/\//g, '\\');
  }

  private listDir = (path: string): void => {
    try {
      hilog.info(DOMAIN, 'wine', 'listDir: path=%{public}s', path);
      let names = fs.listFileSync(path);
      hilog.info(DOMAIN, 'wine', 'listDir: got %{public}d entries', names.length);
      names.sort((a, b) => {
        let aIsDir = this.isDir(path + '/' + a);
        let bIsDir = this.isDir(path + '/' + b);
        if (aIsDir && !bIsDir) return -1;
        if (!aIsDir && bIsDir) return 1;
        if (!aIsDir && !bIsDir) {
          let aExe = a.toLowerCase().endsWith('.exe') ? 0 : 1;
          let bExe = b.toLowerCase().endsWith('.exe') ? 0 : 1;
          return aExe - bExe || a.localeCompare(b);
        }
        return a.localeCompare(b);
      });
      this.entries = names;
    } catch (e) {
      this.entries = [];
    }
  }

  private isDir = (path: string): boolean => {
    try { return fs.statSync(path).isDirectory(); } catch (_) { return false; }
  }

  private isExe = (name: string): boolean => {
    return name.toLowerCase().endsWith('.exe');
  }

  private enterDir = (name: string): void => {
    let newPath = this.curDir + '/' + name;
    this.dirStack.push(this.curDir);
    this.curDir = newPath;
    this.listDir(newPath);
  }

  private goUp = (): void => {
    let parent = this.dirStack.pop();
    if (parent) {
      this.curDir = parent;
      this.listDir(parent);
    }
  }

  // 状态配色 (对齐 main-ui 状态机设计): 失败红 / 运行绿 / 就绪·进行中蓝 / 未启动灰。
  // ready 按 sessionAlive 分色: 有会话=运行中绿, 无会话=已就绪蓝;
  // ready-degraded (桌面准备中) 是进行中而非就绪, 用蓝不用绿
  private statusColor(): string {
    if (this.state == 'failed') return '#f84';
    if (this.state == 'ready') {
      return this.sessionAlive ? '#4f4' : '#8cf';
    }
    if (this.state == 'starting' || this.state == 'stopping' || this.state == 'ready-degraded') {
      return '#8cf';
    }
    return '#888';
  }

  // 状态指示 (对齐 main-ui): 引擎 busy (初始化/解压/编排) 或 starting/stopping
  // 用转圈动画 (语义=不确定时长等待); 其余状态回退 8vp 状态圆点
  @Builder
  statusIndicator() {
    if (this.state == 'starting' || this.state == 'stopping' || this.initBusy || this.opBusy) {
      LoadingProgress()
        .width(14)
        .height(14)
        .color('#7db8ff')
        .margin({ right: 6 })
    } else {
      Circle({ width: 8, height: 8 })
        .fill(this.statusColor())
        .margin({ right: 6 })
    }
  }

  // 按钮使能判据 (对齐 main-ui 状态机设计): 统一走 service 的 isBusy()
  // (initBusy/opBusy + starting/stopping 状态串), 不在页面里自造判据
  private engineBusy(): boolean {
    return this.env.isBusy();
  }

  build() {
    Stack() {
    Row() {
      // ============================================================
      //  侧边栏
      // ============================================================
      Column() {
        // -- Wine 环境状态 --
        Text('Wine 环境')
          .fontWeight(FontWeight.Bold).fontColor('#999')
          .width('100%').padding({ top: 12, bottom: 6 })

        Row() {
          this.statusIndicator()
          Text(this.statusText)
            .fontColor(this.statusColor())
            .layoutWeight(1)
            .maxLines(2).textOverflow({ overflow: TextOverflow.Ellipsis })
        }
        .width('100%').padding({ bottom: 12 })
        .alignItems(VerticalAlign.Center)

        Divider().color('#2a2a2a').width('100%')

        Text('D3D11 / DXGI')
          .fontWeight(FontWeight.Bold).fontColor('#999')
          .width('100%').padding({ top: 12, bottom: 8 })
        Row({ space: 4 }) {
          Button('DXVK 2.6.2')
            .layoutWeight(1).height(34)
            .backgroundColor(this.dxvkBackend === 'dxvk_modern_2_6' ? '#285' : '#333')
            .onClick(() => this.selectDxvkBackend('dxvk_modern_2_6'))
          Button('DXVK 1.10.3')
            .layoutWeight(1).height(34)
            .backgroundColor(this.dxvkBackend === 'dxvk_legacy' ? '#285' : '#333')
            .onClick(() => this.selectDxvkBackend('dxvk_legacy'))
        }
        .width('100%').margin({ bottom: 6 })
        Button('WineD3D fallback')
          .width('100%').height(32)
          .backgroundColor(this.d3dBackend === 'wined3d' ? '#285' : '#333')
          .onClick(() => this.selectD3dBackend('wined3d'))
          .margin({ bottom: 8 })
        Text('D3D12')
          .fontWeight(FontWeight.Bold).fontColor('#999')
          .width('100%').padding({ top: 2, bottom: 8 })
        Button('VKD3D 2.6 (supported, 500K)')
          .width('100%').height(34)
          .backgroundColor(this.d3dBackend === 'vkd3d_limited_500k' ? '#854' : '#333')
          .onClick(() => this.selectD3dBackend('vkd3d_limited_500k'))
          .margin({ bottom: 6 })
        if (this.d3dBackend === 'vkd3d_limited_500k') {
          Text(`混合路由: D3D12 → VKD3D; D3D11/DXGI → ${this.dxvkBackend === 'dxvk_modern_2_6' ? 'DXVK 2.6.2' : 'DXVK 1.10.3'}; x64 D3D12, 500K descriptor。`)
            .fontSize(11).fontColor('#d9a').maxLines(3)
            .width('100%').margin({ bottom: 6 })
        }

        // -- 桌面模式 (仅 PC): 融合模式 = 每个程序独立 OHOS 窗口 (多窗口, 默认);
        // 虚拟桌面 = 独立窗口承载 Wine 桌面。切换后重启引擎生效
        if (this.isPcDevice()) {
          Divider().color('#2a2a2a').width('100%')
          Text('桌面模式')
            .fontWeight(FontWeight.Bold).fontColor('#999')
            .width('100%').padding({ top: 12, bottom: 8 })
          SegmentButton({
            options: SegmentButtonOptions.capsule({
              buttons: [{ text: '融合模式' }, { text: '虚拟桌面' }],
              multiply: false
            }),
            selectedIndexes: $desktopModeSel,
            onItemClicked: (index: number) => {
              this.selectDesktopMode(index == 0 ? MODE_FUSION : MODE_VIRTUAL);
            }
          })
          .width('100%')
          Text('虚拟桌面: Wine 桌面在独立窗口运行; 融合模式: 每个程序独立窗口')
            .fontSize(12).fontColor('#777')
            .width('100%').padding({ top: 4 })
          if (this.desktopModeRestartHint) {
            Text('桌面模式已更改, 重启 Wine 引擎后生效')
              .fontSize(12).fontColor('#f84')
              .width('100%').padding({ top: 4 })
          }
        }

        // -- 快速操作 --
        Text('快速操作')
          .fontWeight(FontWeight.Bold).fontColor('#999')
          .width('100%').padding({ top: 12, bottom: 8 })

        if (!this.prefixReady) {
          Button('初始化 Wine')
            .width('100%').backgroundColor('#285')
            .enabled(!this.engineBusy())
            .onClick(this.quickStart)
            .margin({ bottom: 8 })
        } else if (this.upgradePending) {
          // 引擎数据与 HAP 不一致: 引擎未拉起, 唯一动作是应用更新
          // (完整恢复出厂 = main-ui 升级卡"立即应用"同语义)
          Button('应用引擎更新（重置）')
            .width('100%').backgroundColor('#633')
            .enabled(!this.engineBusy())
            .onClick(this.quickReset)
            .margin({ bottom: 8 })
        } else {
          Button('重置 Wine')
            .width('100%').backgroundColor('#633')
            .enabled(!this.engineBusy())
            .onClick(this.quickReset)
            .margin({ bottom: 8 })
          Button('重启 Wine')
            .width('100%').backgroundColor('#652')
            .enabled(!this.engineBusy())
            .onClick(this.quickRestart)
            .margin({ bottom: 8 })
          // PC 虚拟桌面: 桌面窗口已关闭 (引擎后台继续) 时提供重开入口
          if (this.desktopActive && this.wm.isSeparateDesktopWindow() && !this.desktopWindowActive) {
            Button('打开桌面')
              .width('100%').backgroundColor('#246')
              .enabled(!this.engineBusy())
              .onClick(() => this.wm.openDesktopWindow())
              .margin({ bottom: 8 })
          }
        }

        Divider().color('#2a2a2a').width('100%')

        // -- 开发测试 (smoke 回归) --
        Text('开发测试')
          .fontWeight(FontWeight.Bold).fontColor('#999')
          .width('100%').padding({ top: 12, bottom: 8 })
        Button('Smoke 回归 (core)')
          .width('100%').height(32).backgroundColor('#394')
          .enabled(!this.engineBusy() && this.prefixReady && !SmokeRunner.getInstance().isRunning())
          .onClick(this.startSmoke)
          .margin({ bottom: 6 })
        if (this.smokeStatus) {
          Text(this.smokeStatus)
            .fontSize(12).fontColor(this.smokeResult === 'PASS' ? '#4f4' :
              (this.smokeResult === 'FAIL' || this.smokeResult === 'ERROR' ? '#f55' : '#8cf'))
            .width('100%').maxLines(3)
            .textOverflow({ overflow: TextOverflow.Ellipsis })
            .margin({ bottom: 4 })
          Text('结果: C:\\smoke\\results\\' + (SmokeRunner.getInstance().runId() || '') +
            '\\suite-summary.json')
            .fontSize(11).fontColor('#777')
            .width('100%').maxLines(1)
            .textOverflow({ overflow: TextOverflow.Ellipsis })
        }

        Divider().color('#2a2a2a').width('100%')

        // -- 运行中进程 --
        Row() {
          Text('运行中进程')
            .fontWeight(FontWeight.Bold).fontColor('#999')
            .layoutWeight(1)
          Button({ type: ButtonType.Circle, stateEffect: true }) {
            Text('↻')
          }
          .width(28).height(28).backgroundColor('#333')
          .onClick(() => ProcessService.getInstance().refresh())
        }
        .width('100%').padding({ top: 12, bottom: 8 })
        .alignItems(VerticalAlign.Center)

        if (this.processList.length > 0) {
          List({ space: 1 }) {
            ForEach(this.processList, (proc: ProcessInfo) => {
              ListItem() {
                Row() {
                  Column() {
                    Text(proc.name)
                      .fontColor('#dfd')
                      .maxLines(1).textOverflow({ overflow: TextOverflow.Ellipsis })
                  }
                  .layoutWeight(1).alignItems(HorizontalAlign.Start)

                  if (proc.state === 'running') {
                    Circle({ width: 6, height: 6 }).fill('#4f4').margin({ right: 10 })
                  }
                  if (ProcessService.getInstance().isKillable(proc)) {
                    Text('✕')
                      .fontColor('#f55')
                      .onClick(() => ProcessService.getInstance().kill(proc.pid))
                  }
                }
                .width('100%').padding({ left: 14, right: 14, top: 10, bottom: 10 })
                .alignItems(VerticalAlign.Center)
              }
              .backgroundColor('#1e1e1e')
            })
          }
          .width('100%').layoutWeight(1)
          .margin({ left: -14, right: -14 })
          .scrollBar(BarState.Off)
        } else {
          Text(this.prefixReady ? '无运行中进程' : 'Wine 未初始化')
            .fontColor('#555')
            .width('100%').padding({ top: 8 })
        }
      }
      .width(300).height('100%')
      .backgroundColor('#141414')
      .padding({ left: 14, right: 14, top: 10, bottom: 10 })

      // -- 侧边栏分隔线 --
      Divider().vertical(true).color('#2a2a2a').height('100%')

      // ============================================================
      //  右侧主内容区
      // ============================================================
      Column() {
        // -- 文件浏览器 --
        if (this.prefixReady) {
          Row() {
            if (this.curDir != DRIVE_C) {
              Button('← 上级目录')
                .backgroundColor('#333')
                .onClick(this.goUp)
            }
            Text(this.relPath())
              .fontColor('#888')
              .margin({ left: 10 }).layoutWeight(1)
              .maxLines(1).textOverflow({ overflow: TextOverflow.Ellipsis })
          }
          .width('100%').padding({ left: 14, right: 14, top: 10, bottom: 6 })

          List({ space: 1 }) {
            ForEach(this.entries, (name: string) => {
              ListItem() {
                Row() {
                  Text(this.isDir(this.curDir + '/' + name) ? '📁' : (this.isExe(name) ? '▶' : '  '))
                    .width(28)
                  Text(name)
                    .fontColor(this.isExe(name) ? '#dfd' :
                               this.isDir(this.curDir + '/' + name) ? '#8cf' : '#888')
                    .layoutWeight(1)
                    .maxLines(1).textOverflow({ overflow: TextOverflow.Ellipsis })
                  if (this.isExe(name)) {
                    Button('启动')
                      .height(32)
                      .enabled(!this.engineBusy())
                      .onClick(() => this.doRun(name))
                  }
                }
                .width('100%').height(44).padding({ left: 14, right: 14 })
                .alignItems(VerticalAlign.Center)
                .onClick(() => {
                  if (this.isDir(this.curDir + '/' + name)) {
                    this.enterDir(name);
                  }
                })
              }
              .backgroundColor('#1e1e1e')
            })
          }
          .width('100%').layoutWeight(1)
        } else {
          Column() {
            Text(this.initFailed ? 'Wine 初始化失败，请检查状态后重试或重置' : '需要先初始化 Wine 环境')
              .fontColor('#555')
          }
          .width('100%').layoutWeight(1).justifyContent(FlexAlign.Center)
        }
      }
      .layoutWeight(1).height('100%')
      .backgroundColor('#111')
    }
    .width('100%').height('100%')
    // init overlay 显隐只由状态机管理 (state:ready 才隐藏) — 曾附加
    // !prefixReady 条件, 暖启动/热重启时整个编排过程没有任何可见反馈。
    // 模态化: 全屏半透明遮罩吞掉所有触摸, 初始化/停止编排期间防止点重
    // 背后的操作按钮 (重入风险)。失败给 重试/重置 自救入口 (对齐 main-ui)
    if (this.initOverlayVisible) {
      Column() {
      Column() {
      if (this.initFailed) {
        Text('初始化失败')
          .fontSize(20)
          .fontWeight(FontWeight.Medium)
          .fontColor('#ff5555')
          .margin({ bottom: 8 })
        Text(this.initOverlayText)
          .fontSize(15)
          .fontColor('#aeb8c6')
          .maxLines(2)
          .textOverflow({ overflow: TextOverflow.Ellipsis })
          .margin({ bottom: 18 })
        Row({ space: 12 }) {
          Button('重试')
            .layoutWeight(1)
            // 走编排重启而非裸 startSession: 失败时可能已有半个会话
            // (explorer 已起但 prefix 轮询超时等), 裸 startSession 会双开桌面
            .onClick(() => this.env.restartEngine())
          Button('重置环境')
            .layoutWeight(1)
            .backgroundColor('#633')
            .onClick(this.quickReset)
        }
        .width('100%')
        // 失败终态的出口: overlay 常驻会把用户困在卡片上; 关闭后由侧栏
        // 按钮矩阵承接自救 (重置/重启/初始化) — 同 main-ui "返回启动器"语义
        Text('关闭')
          .fontSize(14)
          .fontColor('#aeb8c6')
          .margin({ top: 16 })
          .onClick(() => {
            this.env.dismissFailedInit();
          })
      } else {
        LoadingProgress()
          .width(42)
          .height(42)
          .color('#7db8ff')
          .margin({ bottom: 18 })
        Text('正在准备 Wine')
          .fontSize(20)
          .fontWeight(FontWeight.Medium)
          .fontColor('#f4f7fb')
          .margin({ bottom: 8 })
        Text(this.initOverlayText)
          .fontSize(15)
          .fontColor('#aeb8c6')
          .maxLines(2)
          .textOverflow({ overflow: TextOverflow.Ellipsis })
      }
      }
      .width(360)
      .padding({ left: 28, right: 28, top: 26, bottom: 26 })
      .backgroundColor('#20242b')
      .borderRadius(8)
      .alignItems(HorizontalAlign.Center)
      .shadow({ radius: 18, color: '#66000000', offsetX: 0, offsetY: 8 })
      }
      .width('100%').height('100%')
      .justifyContent(FlexAlign.Center)
      .backgroundColor('#99000000')
    }
    }
    .width('100%').height('100%')
  }
}
