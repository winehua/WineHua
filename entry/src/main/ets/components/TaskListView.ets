import { ProcessService, ProcessInfo } from '../service/ProcessService';
import { AppLibraryService, AppEntry } from '../service/AppLibraryService';

// 运行中页: 会话卡片 (图标/名称/状态点/结束操作)。
// 卡片无点击行为 (回桌面走 HomeBall / 标题栏"回桌面"), "结束"直接 kill;
// 不提供"停止全部"(批量操作误伤面大, 逐个结束更可控)。
// 注: 曾有的"开发者·进程列表"折叠区已删除 — native getProcessList 只返回
// running 进程 (napi_init.cpp 已过滤), 与上方卡片集合恒等, 纯属冗余。
@Component
export struct TaskListView {
  @State processList: ProcessInfo[] = [];

  private proc: ProcessService = ProcessService.getInstance();

  private mirror = (): void => {
    this.processList = this.proc.processList;
  }

  aboutToAppear(): void {
    this.proc.subscribe(this.mirror);
    this.proc.refresh();
    this.mirror();
  }

  private runningList(): ProcessInfo[] {
    return this.processList.filter((p: ProcessInfo) => p.state === 'running');
  }

  // 会话卡片图标: 进程路径命中应用库条目则借其图标, 否则回退 ▶
  private iconOf(proc: ProcessInfo): string {
    if (proc.path) {
      const hit = AppLibraryService.getInstance().entries.find((e: AppEntry) => e.path == proc.path);
      if (hit?.icon) {
        return hit.icon;
      }
    }
    return '';
  }

  build() {
    Column() {
      // -- 标题行: 刷新 --
      Row() {
        Text('运行中')
          .fontWeight(FontWeight.Medium)
          .fontColor($r('app.color.text_primary'))
          .layoutWeight(1)
        Button({ type: ButtonType.Circle, stateEffect: true }) {
          Text('↻')
        }
        .width(28).height(28).backgroundColor($r('app.color.control_bg'))
        .onClick(this.proc.refresh)
      }
      .width('100%').padding({ left: 14, right: 14, top: 10, bottom: 8 })
      .alignItems(VerticalAlign.Center)

      // -- 会话卡片 --
      if (this.runningList().length > 0) {
        List({ space: 8 }) {
          ForEach(this.runningList(), (proc: ProcessInfo) => {
            ListItem() {
              Row() {
                if (this.iconOf(proc)) {
                  Image(`file://${this.iconOf(proc)}`)
                    .width(32).height(32).margin({ right: 10 })
                } else {
                  Text('▶')
                    .fontSize(22)
                    .fontColor($r('app.color.accent'))
                    .width(32).margin({ right: 10 })
                }
                Column() {
                  Text(proc.name)
                    .fontColor($r('app.color.text_primary'))
                    .maxLines(1).textOverflow({ overflow: TextOverflow.Ellipsis })
                }
                .layoutWeight(1).alignItems(HorizontalAlign.Start)
                Circle({ width: 8, height: 8 })
                  .fill($r('app.color.state_ok'))
                  .margin({ right: 10 })
                // 结束: 直接 kill (旧版 ✕ 的同款行为)
                Text('结束')
                  .fontColor($r('app.color.danger'))
                  .padding({ left: 12, top: 6, bottom: 6 })
                  .onClick(() => this.proc.kill(proc.pid))
              }
              .width('100%').padding({ left: 14, right: 14, top: 12, bottom: 12 })
              .alignItems(VerticalAlign.Center)
              .backgroundColor($r('app.color.card_bg'))
              .borderRadius(8)
            }
          }, (proc: ProcessInfo) => proc.pid.toString())
        }
        .width('100%')
        .layoutWeight(1)
        .padding({ left: 14, right: 14 })
        .scrollBar(BarState.Off)
      } else {
        Column() {
          Text('没有正在运行的 Windows 程序')
            .fontColor($r('app.color.text_disabled'))
        }
        .width('100%').layoutWeight(1).justifyContent(FlexAlign.Center)
      }
    }
    .width('100%').height('100%')
    .backgroundColor($r('app.color.window_bg'))
  }
}
