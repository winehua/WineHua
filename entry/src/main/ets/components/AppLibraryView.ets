import { AppLibraryService, AppEntry } from '../service/AppLibraryService';
import { WineEnvService } from '../service/WineEnvService';

// 应用库页: 已收藏 exe 列表 (M1 无图标, 沿用现列表样式)。
// 点击条目启动; 右侧 ✕ 或长按移除 (AlertDialog 确认)。空态给引导文案 + 可选"去文件页"按钮。
@Component
export struct AppLibraryView {
  @State entries: AppEntry[] = [];
  // env.state 镜像: 仅用于启动守卫 (wine 初始化中禁止启动, 与文件页"启动"按钮 enabled 条件一致)
  @State envState: string = 'idle';
  // 空态"去文件页"回调, 由 Index 注入 (切换 Tabs 到文件页), 有才显示按钮
  onNavigateToFiles?: () => void;

  private lib: AppLibraryService = AppLibraryService.getInstance();
  private env: WineEnvService = WineEnvService.getInstance();

  private mirror = (): void => {
    this.entries = this.lib.entries;
    this.envState = this.env.state;
  }

  aboutToAppear(): void {
    this.lib.subscribe(this.mirror);
    this.env.subscribe(this.mirror);
    this.mirror();
  }

  private launch(entry: AppEntry): void {
    if (this.envState == 'wineserver-starting' || this.envState == 'wineboot-starting') {
      return;
    }
    this.lib.launch(entry.path);
  }

  private confirmRemove(entry: AppEntry): void {
    AlertDialog.show({
      title: '移除应用',
      message: `确定从应用库移除「${entry.name}」吗？`,
      primaryButton: {
        value: '取消',
        action: () => {}
      },
      secondaryButton: {
        value: '移除',
        fontColor: $r('app.color.danger'),
        action: () => {
          this.lib.remove(entry.path);
        }
      }
    });
  }

  build() {
    if (this.entries.length == 0) {
      Column() {
        Text('应用库为空')
          .fontSize(20)
          .fontWeight(FontWeight.Medium)
          .fontColor($r('app.color.text_primary'))
          .margin({ bottom: 8 })
        Text('到「文件」页找到程序的 .exe，点「加入应用库」')
          .fontColor($r('app.color.text_secondary'))
          .margin({ bottom: 16 })
        if (this.onNavigateToFiles) {
          Button('去文件页')
            .backgroundColor($r('app.color.control_bg'))
            .onClick(() => {
              if (this.onNavigateToFiles) {
                this.onNavigateToFiles();
              }
            })
        }
      }
      .width('100%').height('100%')
      .justifyContent(FlexAlign.Center)
      .backgroundColor($r('app.color.window_bg'))
    } else {
      List({ space: 1 }) {
        ForEach(this.entries, (entry: AppEntry) => {
          ListItem() {
            Row() {
              Text('▶').width(28)
              Text(entry.name)
                .fontColor($r('app.color.accent_soft'))
                .layoutWeight(1)
                .maxLines(1).textOverflow({ overflow: TextOverflow.Ellipsis })
              Text('✕')
                .fontColor($r('app.color.danger'))
                .padding({ left: 12, right: 2 })
                .onClick(() => this.confirmRemove(entry))
            }
            .width('100%').height(52).padding({ left: 14, right: 14 })
            .alignItems(VerticalAlign.Center)
            .onClick(() => this.launch(entry))
            .gesture(LongPressGesture({ repeat: false })
              .onAction(() => this.confirmRemove(entry)))
          }
          .backgroundColor($r('app.color.card_bg'))
        }, (entry: AppEntry) => entry.path)
      }
      .width('100%').height('100%')
      .scrollBar(BarState.Off)
      .backgroundColor($r('app.color.window_bg'))
    }
  }
}
