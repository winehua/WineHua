import testNapi from 'libentry.so';
import { inputMethod } from '@kit.IMEKit';
import { WineEnvService } from '../service/WineEnvService';
import { FontImportService } from '../service/FontImportService';
import { ProcessService } from '../service/ProcessService';
import { AppLibraryService } from '../service/AppLibraryService';
import { AppLibraryView } from '../components/AppLibraryView';
import { TaskListView } from '../components/TaskListView';
import { FileBrowserView } from '../components/FileBrowserView';
import { SettingsView } from '../components/SettingsView';
import { AboutView } from '../components/AboutView';
import { WineWindowManager } from '../service/WineWindowManager';
import { DesktopLayer } from '../components/DesktopLayer';
import { FloatingBar } from '../components/FloatingBar';
import { GamepadOverlay } from '../components/GamepadOverlay';
import { GamepadLayoutEditor } from '../components/GamepadLayoutEditor';
import { KeyboardOverlay } from '../components/KeyboardOverlay';
import { InputSettingsService } from '../service/InputSettingsService';
import { Breakpoint, BreakpointSystem } from '../common/Breakpoint';
import { window, display, promptAction } from '@kit.ArkUI';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { BusinessError } from '@kit.BasicServicesKit';

const DOMAIN = 0x0000;
const TAG = 'Index';

@Entry
@Component
struct Index {
  // -- 镜像自 service 的状态 (订阅驱动, 见 aboutToAppear) --
  @State state: string = 'idle';
  @State sessionAlive: boolean = false;
  @State prefixReady: boolean = false;
  @State statusText: string = '检测中...';
  @State initOverlayVisible: boolean = false;
  @State initOverlayText: string = '';
  // 本次初始化含解压 (首启/重置): overlay 显示"1-2 分钟"; 暖启动无解压不给
  @State initExpectsUnpack: boolean = false;
  // 初始化失败 (首启引导 4.8): overlay 把 spinner 换成 重试/重置 入口
  @State initFailed: boolean = false;
  @State currentIndex: number = 0;
  // -- 桌面三层 (Step 3.5 改): 启动器置底 / 桌面层容器 (悬浮窗态小窗缩略 /
  // 全屏态铺满) / FloatingBar+覆盖层 --
  // desktopActive 由 WineWindowManager 驱动: desktop_root 出现置 true (不再
  // 自动全屏 — 桌面以悬浮窗缩略显示, 点悬浮窗/启动程序才进全屏),
  // root 销毁 (会话停止) 翻回 false 回到启动器
  @State desktopActive: boolean = false;
  @State desktopRootId: number = 0;
  // PC 虚拟桌面: 独立窗口存活 (镜像 WineWindowManager.isDesktopWindowActive)
  @State desktopWindowActive: boolean = false;
  @State @Watch('onLauncherVisibleChange') launcherVisible: boolean = true;
  // -- 桌面悬浮窗 (Pad): 默认右上角缩略显示, 可拖动挪位 --
  // 位置用 position 绝对坐标 (vp, 相对根 Stack 左上角), 不用 align+offset:
  // align 对根 Stack 的非全尺寸子组件偶发不生效 (悬浮窗落到屏幕中央), 且
  // offset 钳制按"右上角基准"算会拖不到最右。position 坐标直接表达实际
  // 位置, 钳制基于屏幕范围, 无歧义。
  @State floatPosX: number = 0;
  @State floatPosY: number = 0;
  private floatPanStartX: number = 0;
  private floatPanStartY: number = 0;
  // 缩放过渡动画进行中 (setLauncherVisible 期间): 屏蔽 PanGesture 拖动,
  // 防止动画中改写 floatPosX/Y 打断 position 补间或残留污染坐标
  private transitioning: boolean = false;
  private screenW: number = 0;
  private screenH: number = 0;
  private static readonly FLOAT_W: number = 216;
  // 悬浮窗高度 (vp): 不写死 — 按屏幕比例随 FLOAT_W 折算 (aboutToAppear),
  // 与 Wine 桌面帧比例严格一致 (桌面分辨率 = setOutputSize(物理屏/scale),
  // 比例恒等于物理屏比例)。比例不匹配时 renderer letterbox (ComputeFitRect)
  // 保比例缩放会在小窗内补出黑边。初值 135 仅作屏幕查询失败的回落
  // (对应回落分辨率 640x400 的比例)。
  @State floatH: number = 135;
  private static readonly FLOAT_MARGIN_TOP: number = 80;
  private static readonly FLOAT_MARGIN_RIGHT: number = 12;
  // -- 断点镜像 (Step 4): sm 底部 Tabs / md·lg 左侧导航 --
  @State bp: Breakpoint = 'lg';
  // 应用库"打开所在目录"联动: 目录路径经此 @State 传给 FileBrowserView (@Link), 消费后自清空
  @State filesDirRequest: string = '';
  // 扫描全窗遮罩 (镜像自 AppLibraryService): 与 init overlay 同款条件层方案 —
  // 页面内 bindContentCover 实测不弹出且盖不住侧栏, 故状态上提到服务、
  // 遮罩渲染在根 Stack (init overlay 保持最顶)
  @State scanning: boolean = false;
  @State scanLabel: string = '';
  // 字体导入遮罩 (镜像自 WineEnvService): 与扫描遮罩共用同一条件层
  @State fontImporting: boolean = false;
  // 引擎初始化/编排进行中 (镜像自 WineEnvService): 解压数据/清空 prefix 等
  // 阶段 engineState 尚未迁移到 starting (native state:消息在 startServer 后),
  // 标题栏转圈须按 initBusy/opBusy 判定, 否则只有文字没有动画
  @State initBusy: boolean = false;
  @State opBusy: boolean = false;
  // 升级待处理 (镜像自 WineEnvService): 启动检测到运行时与 HAP 不一致,
  // 不自动解压, 就绪后弹卡片问用户是否应用 (应用=完整重置)
  @State upgradePending: boolean = false;
  // 标题栏右侧避让系统按钮 (PC/2in1): 动态取系统窗口按钮宽度,
  // Pad 无系统按钮 (getTitleButtonRect 失败/width=0) 回落默认 14
  @State titleBarRightPadding: number = 14;

  private env: WineEnvService = WineEnvService.getInstance();
  private proc: ProcessService = ProcessService.getInstance();
  private wm: WineWindowManager = WineWindowManager.getInstance();
  private tabsController: TabsController = new TabsController();
  // 五导航图标 (与 tabBarItem/sideNavItem 的 index 一一对应):
  // 应用库/运行中/文件/设置/关于 — 系统符号, 替代纯文字导航
  private readonly navIcons: Resource[] = [
    $r('sys.symbol.square_grid_2x2'),
    $r('sys.symbol.play_circle'),
    $r('sys.symbol.folder'),
    $r('sys.symbol.gearshape'),
    $r('sys.symbol.info_circle')
  ];

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
    this.setupIme();
    const env = this.env;
    const mirror = () => {
      this.state = env.engineState;
      this.sessionAlive = env.sessionAlive;
      this.prefixReady = env.prefixReady;
      this.statusText = env.statusText;
      this.initOverlayText = env.initOverlayText;
      this.initOverlayVisible = env.initOverlayVisible;
      this.initExpectsUnpack = env.initExpectsUnpack;
      this.initFailed = env.initFailed;
      this.initBusy = env.initBusy;
      this.opBusy = env.opBusy;
      this.upgradePending = env.upgradePending;
    };
    env.subscribe(mirror);
    // 启动程序自动回桌面: 桌面已激活时新程序被受理 (evt:launch-accepted) → 切回桌面层。
    // 走事件而非状态串: 状态机不再携带 pid 前缀, 也避免 process-updated 误触发。
    // PC 虚拟桌面: 启动程序时自动打开虚拟桌面独立窗口 (openDesktopWindow 有
    // starting/active 双守卫, 重复触发无副作用) — 桌面在独立窗口, 不隐藏启动器
    env.subscribeEvents((evt: string) => {
      if (evt.startsWith('launch-accepted') && this.desktopActive) {
        if (this.wm.isSeparateDesktopWindow()) {
          this.wm.openDesktopWindow();
        } else {
          // Pad: 隐藏启动器, 桌面内嵌全屏
          this.setLauncherVisible(false);
        }
      }
    });
    // 桌面状态订阅: desktopActive false→true 时保持启动器可见 — 桌面以
    // 悬浮窗缩略显示 (点悬浮窗/启动程序才进全屏), 不再像旧版
    // "桌面出现即盖住启动器"; root 销毁时翻回启动器, rootId 镜像进 @State
    const wm = WineWindowManager.getInstance();
    wm.subscribe(() => {
      this.desktopActive = wm.desktopActive;
      this.desktopRootId = wm.getDesktopRootId();
      // PC 虚拟桌面: 独立窗口存活状态镜像 (悬浮窗显隐: VDA 存活 → 悬浮窗隐藏)
      this.desktopWindowActive = wm.isDesktopWindowActive();
      if (!this.desktopActive) {
        // 桌面 root 销毁 (会话停止/退出): 翻回启动器, 否则 DesktopLayer
        // 卸载后只剩黑屏
        this.setLauncherVisible(true);
      }
    });
    this.proc.bindEnv(env);
    // 字体导入遮罩镜像 (FontImportService): 拆分自 WineEnvService 的发布版服务
    const fontSvc = FontImportService.getInstance();
    fontSvc.subscribe(() => {
      this.fontImporting = fontSvc.fontImporting;
    });
    env.init(getContext(this));
    const lib = AppLibraryService.getInstance();
    lib.init(getContext(this));
    lib.subscribe(() => {
      this.scanning = lib.scanning;
      this.scanLabel = lib.scanLabel;
    });
    InputSettingsService.getInstance().init(getContext(this));
    // 断点镜像: 订阅 BreakpointSystem (update 由根 Stack onAreaChange 喂入)
    const bps = BreakpointSystem.getInstance();
    bps.subscribe(() => {
      this.bp = bps.current;
    });
    this.bp = bps.current;
    mirror();
    // 标题栏右侧避让系统窗口控制按钮 (PC/2in1): 动态取系统按钮宽度,
    // Pad 无系统按钮 (getTitleButtonRect 失败/width=0) 回落默认 14
    try {
      const tbmw = wm.getMainWindow();
      if (tbmw) {
        const tbRect = tbmw.getTitleButtonRect();
        if (tbRect && tbRect.width > 0) {
          this.titleBarRightPadding = tbRect.width + 4;
        }
        tbmw.on('windowTitleButtonRectChange', (r: window.TitleButtonRect) => {
          this.titleBarRightPadding = (r && r.width > 0) ? r.width + 4 : 14;
        });
        // 窗口 resize (2in1 拖动改尺寸): 刷新悬浮窗钳制/定位基准与比例
        tbmw.on('windowSizeChange', (size: window.Size) => {
          if (size.width > 0) {
            this.screenW = px2vp(size.width);
            this.screenH = px2vp(size.height);
            this.floatH = Math.round(Index.FLOAT_W * this.screenH / this.screenW);
          }
        });
      }
    } catch (e) {
      hilog.warn(DOMAIN, TAG, 'titleButtonRect query failed: %{public}s', JSON.stringify(e));
    }
    // 按当前 launcherVisible 应用一次沉浸 (初始 true = 显示状态栏)
    this.applyImmersiveMode();
    // 悬浮窗拖动钳制尺寸 (vp) + 初始位置右上角 — 用窗口尺寸而非屏幕尺寸:
    // position 相对 Index 页面 (窗口) 定位, PC/2in1 窗口可能非全屏,
    // 按屏幕尺寸计算会超出窗口边界 (悬浮窗整体在窗口外不可见)。
    // Pad 窗口=全屏, 两者一致, 行为不变。
    try {
      const tbmw = WineWindowManager.getInstance().getMainWindow();
      if (tbmw) {
        const props = tbmw.getWindowProperties();
        const wRect = props.windowRect;
        if (wRect && wRect.width > 0) {
          this.screenW = px2vp(wRect.width);
          this.screenH = px2vp(wRect.height);
        }
      }
    } catch (e) {
      hilog.warn(DOMAIN, TAG, 'window size query failed: %{public}s', JSON.stringify(e));
    }
    if (this.screenW <= 0) {
      // 兜底: 窗口信息取不到时回退屏幕尺寸
      try {
        const d = display.getDefaultDisplaySync();
        this.screenW = px2vp(d.width);
        this.screenH = px2vp(d.height);
      } catch (e) {
        hilog.warn(DOMAIN, TAG, 'screen size query failed: %{public}s', JSON.stringify(e));
        this.screenW = 640;
        this.screenH = 400;
      }
    }
    // 悬浮窗高度随屏幕比例折算: 与桌面帧比例一致, 消除 letterbox 黑边
    this.floatH = Math.round(Index.FLOAT_W * this.screenH / this.screenW);
    this.floatPosX = this.screenW - Index.FLOAT_W - Index.FLOAT_MARGIN_RIGHT;
    this.floatPosY = Index.FLOAT_MARGIN_TOP;
    hilog.info(DOMAIN, TAG, '[Index] float init: win=%{public}dx%{public}d screenW=%{public}d floatPos=(%{public}d,%{public}d) floatH=%{public}d',
      this.screenW, this.screenH, this.screenW, this.floatPosX, this.floatPosY, this.floatH);
  }

  // 返回键: 一律走系统默认 (退出 app)。悬浮窗模式不再回全屏桌面 —
  // 进全屏只通过显式入口: 点悬浮窗 / 启动程序自动全屏
  onBackPress(): boolean {
    return false;
  }

  // 缩放过渡动画: 显式动画包裹 launcherVisible 翻转, 闭包内状态驱动
  // 宽/高/位置/圆角/边框/阴影全部属性一次过渡 (FastOutSlowIn=窗口展开/
  // 收起标准曲线)。拖动改 floatPosX/Y 不经此闭包 → 无补间拖尾, 保持跟手。
  // onFinish 只清 transitioning 防护位 (拖动屏蔽), 不含时序依赖逻辑。
  private setLauncherVisible(v: boolean): void {
    this.transitioning = true;
    this.getUIContext().animateTo(
      {
        duration: 300,
        // 凌厉快展: LinearOutSlowIn (cubic-bezier(0,0,0.2,1)) 启动即全速、
        // 尾部缓停 — 对比 FastOutSlowIn (慢-快-慢) 启动偏慢, 展开没劲
        curve: Curve.LinearOutSlowIn,
        onFinish: () => {
          this.transitioning = false;
        }
      },
      () => {
        this.launcherVisible = v;
      }
    );
  }

  private onLauncherVisibleChange(): void {
    // 镜像给 DesktopLayer 判据: 启动器可见时窗口退出全屏, XComponent 高度
    // 波动, 桌面 root 不应跟随 resize (否则热重启桌面高度残留错误值)。
    // PC 虚拟桌面模式: Index 不挂 DesktopLayer, 该值归独立窗口所有
    // (VirtualDesktop 页面置 false), 启动器显隐切换不能回写覆盖。
    const wm = WineWindowManager.getInstance();
    if (!wm.isSeparateDesktopWindow()) {
      wm.setDesktopLauncherVisible(this.launcherVisible);
    }
    this.applyImmersiveMode();
  }

  // 沉浸切换 (照抄旧桌面 Ability 的 try/catch + .catch 日志模式, tag 改 'Index')
  private applyImmersiveMode(): void {
    try {
      const mw = WineWindowManager.getInstance().getMainWindow();
      if (mw) {
        if (this.launcherVisible) {
          // 显示启动器: status/navigationIndicator 显示 + 退出全屏布局
          mw.setSpecificSystemBarEnabled('status', true).catch((e: BusinessError) => {
            hilog.error(DOMAIN, TAG, 'statusBar show failed: %{public}s', JSON.stringify(e));
          });
          mw.setSpecificSystemBarEnabled('navigationIndicator', true).catch((e: BusinessError) => {
            hilog.error(DOMAIN, TAG, 'navBar show failed: %{public}s', JSON.stringify(e));
          });
          mw.setWindowLayoutFullScreen(false).catch((e: BusinessError) => {
            hilog.error(DOMAIN, TAG, 'exit fullScreen failed: %{public}s', JSON.stringify(e));
          });
        } else {
          // 进入桌面: 全屏 + 隐藏状态栏和导航栏
          mw.setWindowLayoutFullScreen(true).catch((e: BusinessError) => {
            hilog.error(DOMAIN, TAG, 'fullScreen failed: %{public}s', JSON.stringify(e));
          });
          mw.setSpecificSystemBarEnabled('status', false).catch((e: BusinessError) => {
            hilog.error(DOMAIN, TAG, 'statusBar hide failed: %{public}s', JSON.stringify(e));
          });
          mw.setSpecificSystemBarEnabled('navigationIndicator', false).catch((e: BusinessError) => {
            hilog.error(DOMAIN, TAG, 'navBar hide failed: %{public}s', JSON.stringify(e));
          });
        }
      }
    } catch (e) {
      hilog.error(DOMAIN, TAG, 'applyImmersive ERR: %{public}s', JSON.stringify(e));
    }
  }

  // 切换主区页签: sm 壳走 TabsController (底部 TabBar), md/lg 壳直接 set currentIndex (Stack visibility)
  private switchTab(index: number): void {
    this.currentIndex = index;
    if (this.bp == 'sm') {
      this.tabsController.changeIndex(index);
    }
  }

  // 应用库"打开所在目录": 置 filesDirRequest 并切到文件页 (index 2), 两种壳同一入口
  private openDirInFiles(dirPath: string): void {
    this.filesDirRequest = dirPath;
    this.switchTab(2);
  }

  // 状态点/状态文字配色 (与环境页同款规则): 失败红 / 运行绿 / 就绪或进行中蓝 / 未启动灰
  private stateColor(): Resource {
    if (this.state == 'failed') {
      return $r('app.color.danger');
    }
    if (this.state == 'ready') {
      return this.sessionAlive ? $r('app.color.state_ok') : $r('app.color.state_info');
    }
    if (this.state == 'starting' || this.state == 'stopping' || this.state == 'ready-degraded') {
      return $r('app.color.state_info');
    }
    return $r('app.color.text_tertiary');
  }

  @Builder
  tabBarItem(title: string, index: number) {
    Column({ space: 2 }) {
      SymbolGlyph(this.navIcons[index])
        .fontSize(20)
        .fontColor([this.currentIndex == index ? $r('app.color.accent') : $r('app.color.text_secondary')])
      Text(title)
        .fontSize(10)
        .fontColor(this.currentIndex == index ? $r('app.color.accent') : $r('app.color.text_secondary'))
    }
    .padding({ top: 6, bottom: 6 })
  }

  // 应用库页 (sm/md/lg 两壳共用一份定义, 保证联动行为一致)
  @Builder
  appLibraryContent() {
    AppLibraryView({
      onNavigateToFiles: (): void => {
        this.switchTab(2);
      },
      onOpenDir: (dirPath: string): void => {
        this.openDirInFiles(dirPath);
      }
    })
  }

  // 运行中页 (两壳共用一份定义, 保证行为一致)
  @Builder
  taskListContent() {
    TaskListView()
  }

  // 侧导航项 (md/lg 壳): 图标+文字; 选中项为 accent 淡底圆角块
  // (替代旧版左侧 3vp 竖条 — 竖条选中态太弱); 行高 48vp, 圆角 8
  @Builder
  sideNavItem(title: string, index: number) {
    Row({ space: 10 }) {
      SymbolGlyph(this.navIcons[index])
        .fontSize(20)
        .fontColor([this.currentIndex == index ? $r('app.color.accent') : $r('app.color.text_secondary')])
      Text(title)
        .fontSize(14)
        .fontColor(this.currentIndex == index ? $r('app.color.accent') : $r('app.color.text_secondary'))
    }
    .width('100%').height(48)
    .padding({ left: 12 })
    .alignItems(VerticalAlign.Center)
    .backgroundColor(this.currentIndex == index ? $r('app.color.accent_bg') : Color.Transparent)
    .borderRadius(8)
    .onClick(() => {
      this.currentIndex = index;
    })
  }

  build() {
    Stack() {
    // Layer 0: 启动器 (visibility 显隐, 树常驻不销毁; 置底 — 桌面层在其上,
    // 悬浮窗态桌面小窗浮于启动器上方, 全屏态启动器 Hidden 不参与绘制)
    Column() {
      // -- 标题栏 --
      Row() {
        Text($r('app.string.app_name'))
          .fontWeight(FontWeight.Bold).fontColor($r('app.color.text_primary'))

        // 状态指示: 引擎 busy (初始化/解压/编排) 或 starting/stopping 用转圈动画
        // (与全 app 其它指示一致, 语义=不确定时长等待); 其余状态回退 8vp 状态圆点
        if (this.state == 'starting' || this.state == 'stopping' || this.initBusy || this.opBusy) {
          LoadingProgress()
            .width(14).height(14)
            .color($r('app.color.accent'))
            .margin({ left: 10 })
        } else {
          Circle({ width: 8, height: 8 })
            .fill(this.stateColor())
            .margin({ left: 10 })
        }
        Text(this.statusText)
          .fontSize(13)
          .fontColor(this.stateColor())
          .margin({ left: 6 }).layoutWeight(1)
          .maxLines(1)

        // 引擎启动入口不在标题栏: 首启由 EngineGuide 引导, 二次进入后台
        // 自动拉起, 失败/降级由 EngineGuide 与设置页承接
        // 进桌面只经悬浮窗点击 (Pad 内嵌全屏 / PC 打开独立窗口) —
        // 不再提供标题栏按钮
      }
      .width('100%')
      .padding({ left: 14, right: this.titleBarRightPadding, top: 10, bottom: 10 })
      .backgroundColor($r('app.color.header_bg'))
      .alignItems(VerticalAlign.Center)

      // -- 五导航 (设计 §3): 应用库 / 运行中 / 文件 / 设置 / 关于
      // (断点双壳: sm 底部 Tabs / md·lg 左侧导航 + 右内容) --
      if (this.bp == 'sm') {
        Tabs({ barPosition: BarPosition.End, controller: this.tabsController, index: this.currentIndex }) {
          TabContent() {
            this.appLibraryContent()
          }
          .tabBar(this.tabBarItem('应用库', 0))

          TabContent() {
            this.taskListContent()
          }
          .tabBar(this.tabBarItem('运行中', 1))

          TabContent() {
            FileBrowserView({ filesDirRequest: $filesDirRequest })
          }
          .tabBar(this.tabBarItem('文件', 2))

          TabContent() {
            SettingsView()
          }
          .tabBar(this.tabBarItem('设置', 3))

          TabContent() {
            AboutView()
          }
          .tabBar(this.tabBarItem('关于', 4))
        }
        .barMode(BarMode.Fixed)
        .barBackgroundColor($r('app.color.panel_bg'))
        .onChange((index: number) => {
          this.currentIndex = index;
        })
        .layoutWeight(1)
      } else {
        Row() {
          // 侧导航 (宽 200vp): 内边距让选中圆角块不贴边
          Column() {
            this.sideNavItem('应用库', 0)
            this.sideNavItem('运行中', 1)
            this.sideNavItem('文件', 2)
            this.sideNavItem('设置', 3)
            this.sideNavItem('关于', 4)
          }
          .width(200).height('100%')
          .padding({ left: 8, right: 8, top: 8 })
          .backgroundColor($r('app.color.panel_bg'))

          // 五 view 常驻 Stack, visibility 切换 (保状态, 与启动器 overlay 同款思路)
          Stack() {
            // @Builder 返回 void 不能挂属性, 包一层 Column 再切 visibility
            Column() {
              this.appLibraryContent()
            }
            .width('100%').height('100%')
            .visibility(this.currentIndex == 0 ? Visibility.Visible : Visibility.Hidden)
            Column() {
              this.taskListContent()
            }
            .width('100%').height('100%')
            .visibility(this.currentIndex == 1 ? Visibility.Visible : Visibility.Hidden)
            FileBrowserView({ filesDirRequest: $filesDirRequest })
              .visibility(this.currentIndex == 2 ? Visibility.Visible : Visibility.Hidden)
            SettingsView()
              .visibility(this.currentIndex == 3 ? Visibility.Visible : Visibility.Hidden)
            AboutView()
              .visibility(this.currentIndex == 4 ? Visibility.Visible : Visibility.Hidden)
          }
          .layoutWeight(1).height('100%')
        }
        .width('100%')
        .layoutWeight(1)
      }
    }
    .width('100%').height('100%')
    .visibility(this.launcherVisible ? Visibility.Visible : Visibility.Hidden)
    // Layer 1: 桌面层容器 — 悬浮窗态 (launcherVisible) = FLOAT_W×floatH 缩略
    // 窗浮于启动器右上角 (点击任意处进桌面 / 按住拖动挪位); 全屏态 = 铺满全屏。
    // 缩略显示依赖 renderer letterbox (ComputeFitRect) 等比缩放全屏 Wine 帧,
    // floatH 按屏幕比例折算保证小窗与桌面帧同比例 (无 letterbox 黑边);
    // 悬浮态 desktopLauncherVisible=true 让 DesktopLayer.onSurfaceChanged 跳过
    // root resize, Wine 桌面分辨率保持全屏不被小窗带歪。
    // - Pad/phone: 桌面内嵌, 悬浮窗 ↔ 全屏两态 (现状)
    // - PC 虚拟桌面: 桌面本体在独立 VirtualDesktopAbility 窗口, 初始化完毕
    //   即呈现悬浮窗 (不自动弹独立窗口, desktop_root 只置 desktopActive),
    //   点击悬浮窗 openDesktopWindow 打开独立窗口; VDA 存活期间悬浮窗隐藏
    //   (desktopWindowActive=true), 关窗后悬浮窗复现。
    //   PC 悬浮窗不带手柄/虚拟键盘悬浮按钮 (有物理键鼠), 提示条/拖动同 Pad
    if (this.desktopActive && this.desktopRootId > 0 &&
      (!this.wm.isSeparateDesktopWindow() || !this.desktopWindowActive)) {
      Stack() {
        DesktopLayer({ floating: this.launcherVisible })
        // 悬浮态交互层: 全尺寸透明, Block 拦截不穿透到桌面 XComponent —
        // 轻点 onClick 进桌面 (Pad 内嵌全屏 / PC 打开独立窗口), 按住 PanGesture 拖动
        if (this.launcherVisible) {
          Stack() {
            Row({ space: 4 }) {
              Text('点击进入桌面 · 按住拖动')
                .fontSize(11)
                .fontColor(Color.White)
            }
            .width('100%')
            .justifyContent(FlexAlign.Center)
            .padding({ top: 3, bottom: 3 })
            .backgroundColor('#66000000')
            .position({ x: 0, y: this.floatH - 26 })
            .hitTestBehavior(HitTestMode.None)
          }
          .width('100%').height('100%')
          .onClick(() => {
            if (this.wm.isSeparateDesktopWindow()) {
              // PC: 打开虚拟桌面独立窗口 (悬浮窗由 desktopWindowActive 隐藏)
              this.wm.openDesktopWindow();
            } else {
              // Pad: 隐藏启动器, 桌面内嵌全屏
              this.setLauncherVisible(false);
            }
          })
          .gesture(
            PanGesture()
              .onActionStart(() => {
                this.floatPanStartX = this.floatPosX;
                this.floatPanStartY = this.floatPosY;
              })
              .onActionUpdate((ev: GestureEvent) => {
                // 缩放过渡动画中屏蔽拖动: 动画中改写 floatPosX/Y 会打断
                // position 补间且残留污染坐标 (下次悬浮窗态位置错乱)
                if (this.transitioning) return;
                // 钳制基于实际坐标: 悬浮窗左上角 ∈ [0, screen - FLOAT]
                const maxX = Math.max(this.screenW - Index.FLOAT_W, 0);
                const maxY = Math.max(this.screenH - this.floatH, 0);
                this.floatPosX = Math.min(Math.max(this.floatPanStartX + ev.offsetX, 0), maxX);
                this.floatPosY = Math.min(Math.max(this.floatPanStartY + ev.offsetY, 0), maxY);
              })
          )
          .hitTestBehavior(HitTestMode.Block)
        }
      }
      .width(this.launcherVisible ? Index.FLOAT_W : '100%')
      .height(this.launcherVisible ? this.floatH : '100%')
      // position 绝对定位 (vp, 相对根 Stack 左上角): 初始右上角, 拖动时浮
      // 动窗跟手指走, 全屏态 0,0 无影响 (宽度铺满)
      .position(this.launcherVisible
        ? { x: this.floatPosX, y: this.floatPosY }
        : { x: 0, y: 0 })
      .borderRadius(this.launcherVisible ? 12 : 0)
      // clip 恒 true: 动画期间 borderRadius 12→0 渐变需持续裁剪, 否则圆角
      // 区域露出方角背景; 全屏态 borderRadius=0 + clip(true) 无可见副作用
      .clip(true)
      .border({ width: this.launcherVisible ? 1 : 0, color: '#66FFFFFF' })
      .shadow({ radius: this.launcherVisible ? 12 : 0,
        color: '#66000000', offsetX: 0, offsetY: this.launcherVisible ? 4 : 0 })
    }
    // Layer 2: 桌面悬浮条 + 手柄覆盖层 (仅桌面激活且启动器隐藏时可见)
    if (this.desktopActive && !this.launcherVisible) {
      FloatingBar({
        onHome: (): void => {
          this.setLauncherVisible(true);
        }
      })
      // 覆盖层自门控 (InputSettingsService.gamepadEnabled), 未开时不渲染
      GamepadOverlay()
      // 模拟键盘覆盖层, 同款自门控 (keyboardEnabled); 置于手柄之上
      KeyboardOverlay()
    }
    // Layer 3: 全屏手柄布局编辑页 (模态, 盖住悬浮条与游戏画面防误触)。
    // 仅桌面激活时挂载; 组件内部按 editingLayout 自门控, 桌面退出时随卸载
    // 自动收口 (aboutToDisappear 兜底 finishLayoutEdit)
    if (this.desktopActive) {
      GamepadLayoutEditor()
    }
    // 扫描/字体导入全窗遮罩 (镜像 AppLibraryService.scanning / WineEnvService
    // .fontImporting): 半透明罩吞掉期间的一切触摸, 防误操作与状态交错;
    // 居中卡片样式与 init overlay 一致; 位于 init overlay 之下 (初始化优先级更高)
    if (this.scanning || this.fontImporting) {
      Column() {
      Column({ space: 18 }) {
        LoadingProgress()
          .width(42).height(42)
          .color($r('app.color.accent'))
        Text(this.fontImporting ? '正在导入字体，请稍候...' : this.scanLabel)
          .fontSize(15)
          .fontColor($r('app.color.text_secondary'))
      }
      .width(360)
      .padding({ left: 28, right: 28, top: 26, bottom: 26 })
      .backgroundColor($r('app.color.overlay_bg'))
      .borderRadius(8)
      .alignItems(HorizontalAlign.Center)
      .shadow({ radius: 18, color: $r('app.color.shadow'), offsetX: 0, offsetY: 8 })
      }
      .width('100%').height('100%')
      .justifyContent(FlexAlign.Center)
      .backgroundColor($r('app.color.overlay_mask'))
    }
    // init overlay 原样置顶 (首启引导 4.8: 阶段文案; 失败给 重试/重置 入口)。
    // 显隐只由状态机管理 (state:ready 才隐藏) — 曾附加 !prefixReady 条件,
    // 暖启动时整个初始化/重启编排过程没有任何可见反馈
    if (this.initOverlayVisible) {
      // 模态化: 全屏半透明遮罩吞掉所有触摸 — 此前 overlay 只是居中的 360vp
      // 卡片, 初始化/停止编排期间用户还能点到背后的启动器按钮 (重入风险)
      Column() {
      Column() {
        if (this.initFailed) {
          Text('初始化失败')
            .fontSize(20)
            .fontWeight(FontWeight.Medium)
            .fontColor($r('app.color.danger'))
            .margin({ bottom: 8 })
          Text(this.initOverlayText)
            .fontSize(15)
            .fontColor($r('app.color.text_secondary'))
            .maxLines(2)
            .textOverflow({ overflow: TextOverflow.Ellipsis })
            .margin({ bottom: 18 })
          Row({ space: 12 }) {
            Button('重试')
              .layoutWeight(1)
              .onClick(() => {
                // 走编排重启而非裸 doInit: 失败时可能已有半个会话
                // (explorer 已起但 prefix 轮询超时等), 裸 doInit 会双开桌面
                this.env.restartEngine();
              })
            Button('重置环境')
              .layoutWeight(1)
              .backgroundColor($r('app.color.action_danger'))
              .onClick(() => {
                AlertDialog.show({
                  title: '重置 Wine 引擎',
                  message: '将清空整个 Wine 容器（包括所有已安装程序和存档），且不可恢复。确定继续？',
                  primaryButton: {
                    value: '取消',
                    action: () => {}
                  },
                  secondaryButton: {
                    value: '重置',
                    fontColor: $r('app.color.danger'),
                    action: () => {
                      this.env.doReset();
                    }
                  }
                });
              })
          }
          .width('100%')
          // 失败终态的出口: overlay 常驻会把用户困在卡片上 (连设置页导出日志/
          // 改渲染策略都够不到); 返回后由启动器内 EngineGuide 承接 failed 引导
          Text('返回启动器')
            .fontSize(14)
            .fontColor($r('app.color.text_secondary'))
            .margin({ top: 16 })
            .onClick(() => {
              this.env.dismissFailedInit();
            })
        } else {
          LoadingProgress()
            .width(42)
            .height(42)
            .color($r('app.color.accent'))
            .margin({ bottom: 18 })
          Text('正在准备 Wine')
            .fontSize(20)
            .fontWeight(FontWeight.Medium)
            .fontColor($r('app.color.text_primary'))
            .margin({ bottom: 8 })
          // 初始化等待耗时预期: 仅含解压 (首启/升级重置) 时给"1-2 分钟",
          // 暖启动 (重启引擎/二次进入, 无解压) 不给 — 同等预期会误导
          if (this.initExpectsUnpack) {
            Text('可能需要 1-2 分钟')
              .fontSize(13)
              .fontColor($r('app.color.text_tertiary'))
              .margin({ bottom: 4 })
          }
          Text(this.initOverlayText)
            .fontSize(15)
            .fontColor($r('app.color.text_secondary'))
            .maxLines(2)
            .textOverflow({ overflow: TextOverflow.Ellipsis })
        }
      }
      .width(360)
      .padding({ left: 28, right: 28, top: 26, bottom: 26 })
      .backgroundColor($r('app.color.overlay_bg'))
      .borderRadius(8)
      .alignItems(HorizontalAlign.Center)
      .shadow({ radius: 18, color: $r('app.color.shadow'), offsetX: 0, offsetY: 8 })
      }
      .width('100%').height('100%')
      .justifyContent(FlexAlign.Center)
      .backgroundColor($r('app.color.overlay_mask'))
    }
    // 升级提示 (镜像 WineEnvService.upgradePending): 启动检测到已解压运行时
    // 与 HAP 打包不一致 (升级) 时不自动解压 — 静默换数据会与旧 prefix 不兼容;
    // 检测到立即弹卡片 (不等引擎就绪) 让用户尽早决定, 否则初始化失败时
    // 升级提示会被吞掉。无需担心与 init overlay 叠层: 升级检测只在自动拉起
    // (bgStart) 路径, 该路径 overlay 本就不显示。
    // 稍后 = 本次会话不再显示, 下次启动仍会检测提醒
    if (this.upgradePending) {
      Column() {
      Column({ space: 14 }) {
        Text('Wine 引擎有更新')
          .fontSize(20)
          .fontWeight(FontWeight.Medium)
          .fontColor($r('app.color.text_primary'))
        Text('检测到新版本 Wine 引擎。应用更新将重新安装引擎并清空 C 盘（所有已安装程序和存档将被删除），且不可恢复。')
          .fontSize(14)
          .fontColor($r('app.color.text_secondary'))
          .lineHeight(20)
        Row({ space: 12 }) {
          Button('稍后')
            .layoutWeight(1)
            .onClick(() => {
              this.env.dismissUpgradeNotice();
              // 跳过后的指引: 想升级的用户知道去哪找 (设置页重置环境 = 应用更新)
              promptAction.showToast({
                message: '已跳过更新，后续可在 设置 → 重置环境 中应用'
              });
            })
          Button('立即应用')
            .layoutWeight(1)
            .backgroundColor($r('app.color.action_danger'))
            .onClick(() => {
              AlertDialog.show({
                title: '应用 Wine 引擎更新',
                message: '将重新安装 Wine 引擎并清空整个 C 盘（包括所有已安装程序和存档），且不可恢复。确定继续？',
                primaryButton: {
                  value: '取消',
                  action: () => {}
                },
                secondaryButton: {
                  value: '确定',
                  fontColor: $r('app.color.danger'),
                  action: () => {
                    // 先 doReset (同步置 opBusy 防重入): 成功才 dismiss (清卡片,
                    // 且 autoStartIfIdle 被 opBusy 挡住不会误补拉起); doReset 被
                    // busy 拦截时保留卡片并提示 — 防止「应用」静默降级为「稍后」
                    // (误写跳过记忆, 同版本从此不再提醒)
                    const started = this.env.doReset();
                    if (started) {
                      this.env.dismissUpgradeNotice();
                    } else {
                      promptAction.showToast({ message: '引擎正在忙，请稍后再试' });
                    }
                  }
                }
              });
            })
        }
        .width('100%')
      }
      .width(360)
      .padding({ left: 24, right: 24, top: 22, bottom: 22 })
      .backgroundColor($r('app.color.overlay_bg'))
      .borderRadius(8)
      .alignItems(HorizontalAlign.Center)
      .shadow({ radius: 18, color: $r('app.color.shadow'), offsetX: 0, offsetY: 8 })
      }
      .width('100%').height('100%')
      .justifyContent(FlexAlign.Center)
      .backgroundColor($r('app.color.overlay_mask'))
    }
    }
    .width('100%').height('100%')
    // 断点喂入: Area.width 单位 vp, BreakpointSystem 内部去重 (变化才 notify)
    .onAreaChange((_oldValue: Area, newValue: Area) => {
      BreakpointSystem.getInstance().update(newValue.width as number);
    })
  }
}
