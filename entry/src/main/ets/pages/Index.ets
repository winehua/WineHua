import { WineEnvService } from '../service/WineEnvService';
import { ProcessService } from '../service/ProcessService';
import { AppLibraryService } from '../service/AppLibraryService';
import { AppLibraryView } from '../components/AppLibraryView';
import { TaskListView } from '../components/TaskListView';
import { FileBrowserView } from '../components/FileBrowserView';
import { EnvPanel } from '../components/EnvPanel';
import { SettingsView } from '../components/SettingsView';
import { WineWindowManager } from '../service/WineWindowManager';
import { DesktopLayer } from '../components/DesktopLayer';
import { FloatingBar } from '../components/FloatingBar';
import { GamepadOverlay } from '../components/GamepadOverlay';
import { InputSettingsService } from '../service/InputSettingsService';
import { Breakpoint, BreakpointSystem } from '../common/Breakpoint';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { BusinessError } from '@kit.BasicServicesKit';

const DOMAIN = 0x0000;
const TAG = 'Index';

@Entry
@Component
struct Index {
  // -- 镜像自 service 的状态 (订阅驱动, 见 aboutToAppear) --
  @State state: string = 'idle';
  @State prefixReady: boolean = false;
  @State statusText: string = '检测中...';
  @State initOverlayVisible: boolean = false;
  @State initOverlayText: string = '';
  // 初始化失败 (首启引导 4.8): overlay 把 spinner 换成 重试/重置 入口
  @State initFailed: boolean = false;
  @State currentIndex: number = 0;
  // -- 桌面三层 (Step 3.5): Layer0 DesktopLayer / Layer1 启动器 / Layer2 FloatingBar --
  // desktopActive 由 WineWindowManager 驱动: desktop_root 出现置 true,
  // root 销毁 (会话停止) 翻回 false 回到启动器
  @State desktopActive: boolean = false;
  @State desktopRootId: number = 0;
  @State @Watch('onLauncherVisibleChange') launcherVisible: boolean = true;
  // -- 断点镜像 (Step 4): sm 底部 Tabs / md·lg 左侧导航 --
  @State bp: Breakpoint = 'lg';
  // 应用库"打开所在目录"联动: 目录路径经此 @State 传给 FileBrowserView (@Link), 消费后自清空
  @State filesDirRequest: string = '';

  private env: WineEnvService = WineEnvService.getInstance();
  private proc: ProcessService = ProcessService.getInstance();
  private tabsController: TabsController = new TabsController();
  // 五导航图标 (与 tabBarItem/sideNavItem 的 index 一一对应):
  // 应用库/运行中/文件/环境/设置 — 系统符号, 替代纯文字导航
  private readonly navIcons: Resource[] = [
    $r('sys.symbol.square_grid_2x2'),
    $r('sys.symbol.play_circle'),
    $r('sys.symbol.folder'),
    $r('sys.symbol.archivebox'),
    $r('sys.symbol.gearshape')
  ];

  aboutToAppear(): void {
    const env = this.env;
    const mirror = () => {
      this.state = env.state;
      this.prefixReady = env.prefixReady;
      this.statusText = env.statusText;
      this.initOverlayText = env.initOverlayText;
      this.initOverlayVisible = env.initOverlayVisible;
      this.initFailed = env.initFailed;
      // 启动程序自动回桌面: 桌面已激活时新程序启动 → 切回桌面层
      const msg = env.lastStateMsg;
      const colonIdx = msg.indexOf(':');
      const suffix = colonIdx >= 0 ? msg.substring(colonIdx + 1) : msg;
      if (suffix == 'wine-running' && this.desktopActive) {
        this.launcherVisible = false;
      }
    };
    env.subscribe(mirror);
    // 桌面状态订阅: desktopActive false→true 时盖住启动器
    // (复刻"桌面出现即盖住启动器"), rootId 镜像进 @State
    const wm = WineWindowManager.getInstance();
    wm.subscribe(() => {
      const wasActive = this.desktopActive;
      this.desktopActive = wm.desktopActive;
      this.desktopRootId = wm.getDesktopRootId();
      if (!wasActive && this.desktopActive) {
        this.launcherVisible = false;
      } else if (wasActive && !this.desktopActive) {
        // 桌面 root 销毁 (会话停止/退出): 翻回启动器, 否则 DesktopLayer
        // 卸载后只剩黑屏
        this.launcherVisible = true;
      }
    });
    this.proc.bindEnv(env);
    env.init(getContext(this));
    AppLibraryService.getInstance().init(getContext(this));
    InputSettingsService.getInstance().init(getContext(this));
    // 断点镜像: 订阅 BreakpointSystem (update 由根 Stack onAreaChange 喂入)
    const bps = BreakpointSystem.getInstance();
    bps.subscribe(() => {
      this.bp = bps.current;
    });
    this.bp = bps.current;
    mirror();
    // 按当前 launcherVisible 应用一次沉浸 (初始 true = 显示状态栏)
    this.applyImmersiveMode();
  }

  // 返回键: 启动器覆盖桌面时 → 回桌面层 (消耗事件); 否则系统默认
  onBackPress(): boolean {
    if (this.desktopActive && this.launcherVisible) {
      this.launcherVisible = false;
      return true;
    }
    return false;
  }

  private onLauncherVisibleChange(): void {
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

  // 状态点/状态文字配色 (标题栏与环境页同款规则)
  private stateColor(): Resource {
    return this.state == 'wine-running' ? $r('app.color.state_ok') :
           this.state == 'exited' ? $r('app.color.state_warn') :
           this.prefixReady ? $r('app.color.state_info') : $r('app.color.text_tertiary');
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
    // Layer 0: 桌面层 (desktopActive 期间挂载, root 销毁 desktopActive 翻回后卸载)
    if (this.desktopActive && this.desktopRootId > 0) {
      DesktopLayer()
    }
    // Layer 1: 启动器 (visibility 显隐, 树常驻不销毁)
    Column() {
      // -- 标题栏 --
      Row() {
        Text($r('app.string.app_name'))
          .fontWeight(FontWeight.Bold).fontColor($r('app.color.text_primary'))

        // 状态点 + 状态文字 (环境页同款状态色)
        Circle({ width: 8, height: 8 })
          .fill(this.stateColor())
          .margin({ left: 10 })
        Text(this.statusText)
          .fontSize(13)
          .fontColor(this.stateColor())
          .margin({ left: 6 }).layoutWeight(1)
          .maxLines(1)

        // 回桌面可见入口 (Step 3.5): 仅桌面激活时显示。
        // 与 onBackPress、启动程序自动回桌面并列的第三条返回路径。
        if (this.desktopActive) {
          Row({ space: 4 }) {
            SymbolGlyph($r('sys.symbol.house'))
              .fontSize(16)
              .fontColor([$r('app.color.accent')])
            Text('回桌面')
              .fontSize(14)
              .fontColor($r('app.color.accent'))
          }
          .padding({ left: 10, right: 10, top: 5, bottom: 5 })
          .backgroundColor($r('app.color.control_bg'))
          .borderRadius(8)
          .onClick(() => {
            this.launcherVisible = false;
          })
        }
      }
      .width('100%')
      .padding({ left: 14, right: 14, top: 10, bottom: 10 })
      .backgroundColor($r('app.color.header_bg'))
      .alignItems(VerticalAlign.Center)

      // -- 五导航 (设计 §3): 应用库 / 运行中 / 文件 / 环境 / 设置
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
            EnvPanel()
          }
          .tabBar(this.tabBarItem('环境', 3))

          TabContent() {
            SettingsView()
          }
          .tabBar(this.tabBarItem('设置', 4))
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
            this.sideNavItem('环境', 3)
            this.sideNavItem('设置', 4)
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
            EnvPanel()
              .visibility(this.currentIndex == 3 ? Visibility.Visible : Visibility.Hidden)
            SettingsView()
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
    // Layer 2: 桌面悬浮条 + 手柄覆盖层 (仅桌面激活且启动器隐藏时可见)
    if (this.desktopActive && !this.launcherVisible) {
      FloatingBar({
        onHome: (): void => {
          this.launcherVisible = true;
        }
      })
      // 覆盖层自门控 (InputSettingsService.gamepadEnabled), 未开时不渲染
      GamepadOverlay()
    }
    // init overlay 原样置顶 (首启引导 4.8: 阶段文案; 失败给 重试/重置 入口)
    if (this.initOverlayVisible && !this.prefixReady) {
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
              .backgroundColor($r('app.color.accent'))
              .onClick(() => {
                this.env.doInit();
              })
            Button('重置环境')
              .layoutWeight(1)
              .backgroundColor($r('app.color.action_danger'))
              .onClick(() => {
                AlertDialog.show({
                  title: '重置 Wine prefix',
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
    }
    .width('100%').height('100%')
    // 断点喂入: Area.width 单位 vp, BreakpointSystem 内部去重 (变化才 notify)
    .onAreaChange((_oldValue: Area, newValue: Area) => {
      BreakpointSystem.getInstance().update(newValue.width as number);
    })
  }
}
