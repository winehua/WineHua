import { WineEnvService } from '../service/WineEnvService';
import { ProcessService } from '../service/ProcessService';
import { AppLibraryService } from '../service/AppLibraryService';
import { AppLibraryView } from '../components/AppLibraryView';
import { TaskListView } from '../components/TaskListView';
import { FileBrowserView } from '../components/FileBrowserView';
import { EnvPanel } from '../components/EnvPanel';
import { WineWindowManager } from '../service/WineWindowManager';
import { DesktopLayer } from '../components/DesktopLayer';
import { HomeBall } from '../components/HomeBall';
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
  @State currentIndex: number = 0;
  // -- 桌面三层 (Step 3.5): Layer0 DesktopLayer / Layer1 启动器 / Layer2 HomeBall --
  // desktopActive 一旦 true 不再翻回 false (复刻旧桌面 Ability 永不关闭语义)
  @State desktopActive: boolean = false;
  @State desktopRootId: number = 0;
  @State @Watch('onLauncherVisibleChange') launcherVisible: boolean = true;

  private env: WineEnvService = WineEnvService.getInstance();
  private proc: ProcessService = ProcessService.getInstance();
  private tabsController: TabsController = new TabsController();

  aboutToAppear(): void {
    const env = this.env;
    const mirror = () => {
      this.state = env.state;
      this.prefixReady = env.prefixReady;
      this.statusText = env.statusText;
      this.initOverlayText = env.initOverlayText;
      this.initOverlayVisible = env.initOverlayVisible;
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
      }
    });
    this.proc.bindEnv(env);
    env.init(getContext(this));
    AppLibraryService.getInstance().init(getContext(this));
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

  @Builder
  tabBarItem(title: string, index: number) {
    Text(title)
      .fontColor(this.currentIndex == index ? $r('app.color.accent') : $r('app.color.text_secondary'))
      .padding({ top: 12, bottom: 12 })
  }

  build() {
    Stack() {
    // Layer 0: 桌面层 (desktopActive 后挂载常驻, root 销毁不卸载)
    if (this.desktopActive && this.desktopRootId > 0) {
      DesktopLayer()
    }
    // Layer 1: 启动器 (visibility 显隐, 树常驻不销毁)
    Column() {
      // -- 标题栏 --
      Row() {
        Text($r('app.string.app_name'))
          .fontWeight(FontWeight.Bold).fontColor($r('app.color.text_primary'))

        Text(this.statusText)
          .fontColor(this.state == 'wine-running' ? $r('app.color.state_ok') :
                     this.state == 'exited' ? $r('app.color.state_warn') :
                     this.prefixReady ? $r('app.color.state_info') : $r('app.color.text_tertiary'))
          .margin({ left: 12 }).layoutWeight(1)
          .maxLines(1)

        // 回桌面可见入口 (Step 3.5): 仅桌面激活时显示。
        // 与 onBackPress、启动程序自动回桌面并列的第三条返回路径。
        if (this.desktopActive) {
          Text('回桌面 ⌂')
            .fontColor($r('app.color.accent'))
            .padding({ left: 12, top: 4, bottom: 4 })
            .onClick(() => {
              this.launcherVisible = false;
            })
        }
      }
      .width('100%')
      .padding({ left: 14, right: 14, top: 10, bottom: 10 })
      .backgroundColor($r('app.color.header_bg'))
      .alignItems(VerticalAlign.Center)

      // -- 四 Tab: 应用库 / 任务 / 文件 / 环境 --
      Tabs({ barPosition: BarPosition.End, controller: this.tabsController, index: this.currentIndex }) {
        TabContent() {
          AppLibraryView({
            onNavigateToFiles: (): void => {
              this.currentIndex = 2;
              this.tabsController.changeIndex(2);
            }
          })
        }
        .tabBar(this.tabBarItem('应用库', 0))

        TabContent() {
          TaskListView()
        }
        .tabBar(this.tabBarItem('任务', 1))

        TabContent() {
          FileBrowserView()
        }
        .tabBar(this.tabBarItem('文件', 2))

        TabContent() {
          EnvPanel()
        }
        .tabBar(this.tabBarItem('环境', 3))
      }
      .barMode(BarMode.Fixed)
      .barBackgroundColor($r('app.color.panel_bg'))
      .onChange((index: number) => {
        this.currentIndex = index;
      })
      .layoutWeight(1)
    }
    .width('100%').height('100%')
    .visibility(this.launcherVisible ? Visibility.Visible : Visibility.Hidden)
    // Layer 2: 悬浮球 (仅桌面激活且启动器隐藏时可见)
    if (this.desktopActive && !this.launcherVisible) {
      HomeBall({
        onHome: (): void => {
          this.launcherVisible = true;
        }
      })
    }
    // init overlay 原样置顶
    if (this.initOverlayVisible && !this.prefixReady) {
      Column() {
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
      .width(360)
      .padding({ left: 28, right: 28, top: 26, bottom: 26 })
      .backgroundColor($r('app.color.overlay_bg'))
      .borderRadius(8)
      .alignItems(HorizontalAlign.Center)
      .shadow({ radius: 18, color: $r('app.color.shadow'), offsetX: 0, offsetY: 8 })
    }
    }
    .width('100%').height('100%')
  }
}
