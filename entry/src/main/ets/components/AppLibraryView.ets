import { AppLibraryService, AppEntry } from '../service/AppLibraryService';
import { WineEnvService } from '../service/WineEnvService';
import { Breakpoint, BreakpointSystem } from '../common/Breakpoint';
import { promptAction } from '@kit.ArkUI';

// 应用库页: 已收藏 exe 列表 (M1 无图标)。断点形态: lg 五列 Grid 大卡 / sm·md 现 List。
// 点击条目启动; 右键(PC)/长按(Pad)弹同一份 bindContextMenu (启动/打开所在目录/从库中移除),
// List 模式另保留 ✕ 快捷移除 (AlertDialog 确认)。空态给引导文案 + 可选"去文件页"按钮。
@Component
export struct AppLibraryView {
  @State entries: AppEntry[] = [];
  // env.state 镜像: 仅用于启动守卫 (wine 初始化中禁止启动, 与文件页"启动"按钮 enabled 条件一致)
  @State envState: string = 'idle';
  // 断点镜像: lg → Grid 大卡, sm/md → List
  @State bp: Breakpoint = 'lg';
  // 空态"去文件页"回调, 由 Index 注入 (切换 Tabs 到文件页), 有才显示按钮
  onNavigateToFiles?: () => void;
  // "打开所在目录"回调, 由 Index 注入 (定位文件页到目录), 有才显示该菜单项
  onOpenDir?: (dirPath: string) => void;

  private lib: AppLibraryService = AppLibraryService.getInstance();
  private env: WineEnvService = WineEnvService.getInstance();

  private mirror = (): void => {
    this.entries = this.lib.entries;
    this.envState = this.env.state;
  }

  aboutToAppear(): void {
    this.lib.subscribe(this.mirror);
    this.env.subscribe(this.mirror);
    const bps = BreakpointSystem.getInstance();
    bps.subscribe(() => {
      this.bp = bps.current;
    });
    this.bp = bps.current;
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

  // exe 路径 → 所在目录 (去掉最后一段文件名)
  private dirOf(path: string): string {
    const idx = path.lastIndexOf('/');
    return idx > 0 ? path.substring(0, idx) : path;
  }

  // 条目菜单 (右键与长按通用): 启动 / 打开所在目录 (有回调才显示) / 从库中移除 (danger)
  @Builder
  entryMenu(entry: AppEntry) {
    Menu() {
      MenuItem({ content: '启动' })
        .onChange(() => {
          this.launch(entry);
        })
      if (this.onOpenDir) {
        MenuItem({ content: '打开所在目录' })
          .onChange(() => {
            if (this.onOpenDir) {
              this.onOpenDir(this.dirOf(entry.path));
            }
          })
      }
      MenuItem({ content: '从库中移除' })
        .contentFontColor($r('app.color.danger'))
        .onChange(() => {
          this.confirmRemove(entry);
        })
    }
  }

  build() {
    Column() {
      // 顶行: 标题 + 扫描快捷方式入口 (M3, 手动触发, 结果 toast)
      Row() {
        Text('应用库')
          .fontWeight(FontWeight.Medium)
          .fontColor($r('app.color.text_primary'))
          .layoutWeight(1)
        Text('扫描快捷方式')
          .fontColor($r('app.color.accent'))
          .padding({ left: 12, top: 4, bottom: 4 })
          .onClick(() => {
            const added = this.lib.scanShortcuts();
            promptAction.showToast({ message: added > 0 ? `新增 ${added} 个应用` : '没有发现新快捷方式' });
          })
      }
      .width('100%')
      .padding({ left: 14, right: 14, top: 8, bottom: 8 })

      Stack() {
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
      } else if (this.bp == 'lg') {
        // lg 大卡栅格 (Pad 全屏 / PC 宽窗): 五列 1fr, 卡片高 120vp
        Grid() {
          ForEach(this.entries, (entry: AppEntry) => {
            GridItem() {
              Column() {
                // 图标 (M3): 提取完成前回退 ▶
                if (entry.icon) {
                  Image(`file://${entry.icon}`)
                    .width(40).height(40)
                } else {
                  Text('▶')
                    .fontSize(32)
                    .fontColor($r('app.color.accent'))
                }
                Text(entry.name)
                  .fontColor($r('app.color.accent_soft'))
                  .margin({ top: 8 })
                  .maxLines(1).textOverflow({ overflow: TextOverflow.Ellipsis })
              }
              .width('100%').height(120)
              .justifyContent(FlexAlign.Center)
              .alignItems(HorizontalAlign.Center)
              .backgroundColor($r('app.color.card_bg'))
              .borderRadius(8)
              .onClick(() => this.launch(entry))
              .hoverEffect(HoverEffect.Highlight)
              .bindContextMenu(this.entryMenu(entry), ResponseType.LongPress)
            }
          }, (entry: AppEntry) => entry.path)
        }
        .columnsTemplate('1fr 1fr 1fr 1fr 1fr')
        .columnsGap(12)
        .rowsGap(12)
        .padding(12)
        .width('100%').height('100%')
        .backgroundColor($r('app.color.window_bg'))
      } else {
        // sm/md 列表 (原样 + hover/右键长按菜单)
        List({ space: 1 }) {
          ForEach(this.entries, (entry: AppEntry) => {
            ListItem() {
              Row() {
                if (entry.icon) {
                  Image(`file://${entry.icon}`)
                    .width(24).height(24).margin({ right: 6 })
                } else {
                  Text('▶').width(28)
                }
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
              .hoverEffect(HoverEffect.Highlight)
              .bindContextMenu(this.entryMenu(entry), ResponseType.LongPress)
            }
            .backgroundColor($r('app.color.card_bg'))
          }, (entry: AppEntry) => entry.path)
        }
        .width('100%').height('100%')
        .scrollBar(BarState.Off)
        .backgroundColor($r('app.color.window_bg'))
      }
      }
      .layoutWeight(1).width('100%')
    }
    .width('100%').height('100%')
    // 根容器统一背景: 启动器下层是桌面 XComponent, 根透明会透出桌面 (顶行/分支不再单设)
    .backgroundColor($r('app.color.window_bg'))
  }
}
