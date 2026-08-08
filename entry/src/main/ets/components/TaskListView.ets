import { ProcessService, ProcessInfo } from '../service/ProcessService';
import { AppLibraryService } from '../service/AppLibraryService';
import { WineEnvService } from '../service/WineEnvService';
import { EmptyState } from './EmptyState';
import { EngineGuide } from './EngineGuide';

// 运行中页: 会话卡片 (图标/名称/状态点/结束操作)。
// 卡片无点击行为 (回桌面走悬浮条 Home / 标题栏"回桌面"), "结束"二次确认后 kill;
// 不提供"停止全部"(批量操作误伤面大, 逐个结束更可控)。
// 下拉刷新: Refresh 包 List (Pad 触控场景); ↻ 按钮保留 (PC 无下拉习惯)。
// 注: 曾有的"开发者·进程列表"折叠区已删除 — native getProcessList 只返回
// running 进程 (napi_init.cpp 已过滤), 与上方卡片集合恒等, 纯属冗余。
// 注: 行未用系统 ComposeListItem — 其右侧 button 槽无 action 回调,
// 仅 icon 槽有点击, 而 icon 槽 value 必填图片资源 (不接受符号), 无法表达
// "结束"这个可点操作, 故保留自绘行。
@Component
export struct TaskListView {
  @State processList: ProcessInfo[] = [];
  // Refresh 下拉刷新状态 ($$ 双向绑定)
  @State refreshing: boolean = false;
  // 引擎状态镜像: 空列表时区分 "引擎在跑但没程序" (原空态) 与
  // "引擎未跑/启动中/失败" (共享 EngineGuide 引导)
  @State engineState: string = 'idle';

  private proc: ProcessService = ProcessService.getInstance();
  private env: WineEnvService = WineEnvService.getInstance();

  private mirror = (): void => {
    // 截新数组: 本 mirror 同时挂在 AppLibraryService 上 (进程图标异步提取完成
    // 会 notify), proc.processList 引用未变也要触发列表重渲染
    this.processList = this.proc.processList.slice();
  }

  private mirrorEnv = (): void => {
    this.engineState = this.env.engineState;
  }

  aboutToAppear(): void {
    this.proc.subscribe(this.mirror);
    // 进程图标经 AppLibraryService 解析 (库内借用 / 库外按需提取), 提取完成
    // 经其 notify 刷新, 必须订阅
    AppLibraryService.getInstance().subscribe(this.mirror);
    this.env.subscribe(this.mirrorEnv);
    this.proc.refresh();
    this.mirror();
    this.mirrorEnv();
  }

  private runningList(): ProcessInfo[] {
    return this.processList.filter((p: ProcessInfo) => p.state === 'running');
  }

  // 会话卡片图标: 应用库命中借图标 / 真实 exe 路径按需提取
  // (AppLibraryService.iconForProc 统一解析), 否则回退系统播放符号
  private iconOf(proc: ProcessInfo): string {
    return AppLibraryService.getInstance().iconForProc(proc.path);
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
          SymbolGlyph($r('sys.symbol.arrow_clockwise'))
            .fontSize(18)
            .fontColor([$r('app.color.text_primary')])
        }
        .width(28).height(28).backgroundColor($r('app.color.control_bg'))
        .onClick(this.proc.refresh)
      }
      .width('100%').padding({ left: 14, right: 14, top: 10, bottom: 8 })
      .alignItems(VerticalAlign.Center)

      // -- 会话卡片 --
      if (this.runningList().length > 0) {
        Refresh({ refreshing: $$this.refreshing }) {
          List({ space: 8 }) {
            ForEach(this.runningList(), (proc: ProcessInfo) => {
              ListItem() {
                Row() {
                  if (this.iconOf(proc)) {
                    Image(`file://${this.iconOf(proc)}`)
                      .width(32).height(32).margin({ right: 10 })
                  } else {
                    SymbolGlyph($r('sys.symbol.play_fill'))
                      .fontSize(24)
                      .fontColor([$r('app.color.accent')])
                      .width(32).margin({ right: 10 })
                  }
                  Column({ space: 2 }) {
                    Text(proc.name)
                      .fontColor($r('app.color.text_primary'))
                      .maxLines(1).textOverflow({ overflow: TextOverflow.Ellipsis })
                    if (proc.path) {
                      Text(proc.path)
                        .fontSize(11)
                        .fontColor($r('app.color.text_tertiary'))
                        .maxLines(1).textOverflow({ overflow: TextOverflow.Ellipsis })
                    }
                  }
                  .layoutWeight(1).alignItems(HorizontalAlign.Start)
                  Circle({ width: 8, height: 8 })
                    .fill($r('app.color.state_ok'))
                    .margin({ right: 10 })
                  // 结束: 二次确认后 kill (与环境页"停止全部"同级别确认 —
                  // 未保存数据会丢失, 上架标准不允许无确认直接杀)。
                  // 桌面 shell 基础进程除外 (native 在桌面首次出现时打标的
                  // desktopShell 进程): 结束会破坏桌面运行, 故隐藏结束按钮,
                  // 以灰色"桌面"标签替代说明。
                  if (this.proc.isKillable(proc)) {
                    Text('结束')
                      .fontColor($r('app.color.danger'))
                      .padding({ left: 12, top: 6, bottom: 6 })
                      .onClick(() => {
                        AlertDialog.show({
                          title: '结束程序',
                          message: `确定结束「${proc.name}」吗？未保存的数据会丢失。`,
                          primaryButton: {
                            value: '取消',
                            action: () => {}
                          },
                          secondaryButton: {
                            value: '结束',
                            fontColor: $r('app.color.danger'),
                            action: () => this.proc.kill(proc.pid)
                          }
                        });
                      })
                  } else {
                    Text('桌面')
                      .fontSize(12)
                      .fontColor($r('app.color.text_tertiary'))
                      .padding({ left: 12, top: 6, bottom: 6 })
                  }
                }
                .width('100%').padding({ left: 14, right: 14, top: 12, bottom: 12 })
                .alignItems(VerticalAlign.Center)
              }
              .backgroundColor($r('app.color.card_bg'))
              .borderRadius(12)
            // key 带图标状态 + desktopShell: ArkUI ForEach 同 key 复用子项不重跑
            // builder — 图标异步提取完成后 key 变化强制重建 (否则回退 ▶ 永久滞留,
            // Pal2 根因); desktopShell 同理: 桌面首次出现时从 false 变 true, key
            // 不变则行复用, "结束"按钮不会切换为"桌面"标签 (保护失效的症状)
            }, (proc: ProcessInfo) => `${proc.pid}|${this.iconOf(proc)}|${proc.desktopShell}`)
          }
          .width('100%')
          .padding({ left: 14, right: 14 })
          .scrollBar(BarState.Off)
        }
        .layoutWeight(1)
        .onRefreshing(() => {
          this.proc.refresh();
          this.refreshing = false;
        })
      } else if (this.engineState == 'ready' || this.engineState == 'ready-degraded') {
        // 引擎在跑但没程序: 原空态
        Column() {
          EmptyState({
            icon: $r('sys.symbol.play_circle'),
            title: '没有正在运行的 Windows 程序'
          })
        }
        .width('100%').layoutWeight(1).justifyContent(FlexAlign.Center)
      } else {
        // 引擎未跑/启动中/失败: 共享引导 — 此前一律显示"没有正在运行的
        // Windows 程序", 引擎未启动时会误导用户以为程序已退出
        Column() {
          EngineGuide()
        }
        .width('100%').layoutWeight(1)
      }
    }
    .width('100%').height('100%')
    .backgroundColor($r('app.color.window_bg'))
  }
}
