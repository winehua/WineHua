import { ProcessService, ProcessInfo } from '../service/ProcessService';

// 任务页: 运行中进程列表 (原侧边栏区块升级为整页)。
// 标题行 + ↻ 刷新; 只有"结束"操作 (✕ kill), 不提供"启动"。
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

  build() {
    Column() {
      // -- 标题行 + 刷新 --
      Row() {
        Text('运行中进程')
          .fontWeight(FontWeight.Bold).fontColor($r('app.color.text_secondary'))
          .layoutWeight(1)
        Button({ type: ButtonType.Circle, stateEffect: true }) {
          Text('↻')
        }
        .width(28).height(28).backgroundColor($r('app.color.control_bg'))
        .onClick(this.proc.refresh)
      }
      .width('100%').padding({ left: 14, right: 14, top: 10, bottom: 8 })
      .alignItems(VerticalAlign.Center)

      if (this.processList.length > 0) {
        List({ space: 1 }) {
          ForEach(this.processList, (proc: ProcessInfo) => {
            ListItem() {
              Row() {
                Column() {
                  Text(proc.name)
                    .fontColor($r('app.color.accent_soft'))
                    .maxLines(1).textOverflow({ overflow: TextOverflow.Ellipsis })
                }
                .layoutWeight(1).alignItems(HorizontalAlign.Start)

                if (proc.state === 'running') {
                  Circle({ width: 6, height: 6 }).fill($r('app.color.state_ok')).margin({ right: 10 })
                }
                Text('✕')
                  .fontColor($r('app.color.danger'))
                  .onClick(() => this.proc.kill(proc.pid))
              }
              .width('100%').padding({ left: 14, right: 14, top: 10, bottom: 10 })
              .alignItems(VerticalAlign.Center)
            }
            .backgroundColor($r('app.color.card_bg'))
          }, (proc: ProcessInfo) => proc.pid.toString())
        }
        .width('100%').layoutWeight(1)
        .scrollBar(BarState.Off)
      } else {
        Column() {
          Text('无运行中进程')
            .fontColor($r('app.color.text_disabled'))
        }
        .width('100%').layoutWeight(1).justifyContent(FlexAlign.Center)
      }
    }
    .width('100%').height('100%')
    .backgroundColor($r('app.color.window_bg'))
  }
}
