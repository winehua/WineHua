import { WineEnvService } from '../service/WineEnvService';
import { ProcessService } from '../service/ProcessService';
import { AppLibraryService } from '../service/AppLibraryService';
import { AppLibraryView } from '../components/AppLibraryView';
import { TaskListView } from '../components/TaskListView';
import { FileBrowserView } from '../components/FileBrowserView';
import { EnvPanel } from '../components/EnvPanel';

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
    };
    env.subscribe(mirror);
    this.proc.bindEnv(env);
    env.init(getContext(this));
    AppLibraryService.getInstance().init(getContext(this));
    mirror();
  }

  @Builder
  tabBarItem(title: string, index: number) {
    Text(title)
      .fontColor(this.currentIndex == index ? $r('app.color.accent') : $r('app.color.text_secondary'))
      .padding({ top: 12, bottom: 12 })
  }

  build() {
    Stack() {
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
