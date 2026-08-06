import { deviceInfo } from '@kit.BasicServicesKit';
import { promptAction, SegmentButton, SegmentButtonOptions } from '@kit.ArkUI';
import { WineEnvService } from '../service/WineEnvService';

const WINE_PREFIX = '/data/storage/el2/base/files/.wine';

// 环境页 (设计 4.5): 三卡结构 — Wine 引擎卡 (状态 + 渲染策略分段开关) /
// 会话维护卡 (重启引擎 / 停止全部会话 / 导出日志) / 红色危险区 (重置 prefix 二次确认);
// 开发者分区折叠 (诊断 profile / d3dBackend / prefix / Z: / 设备类型)。
@Component
export struct EnvPanel {
  @State state: string = 'idle';
  @State prefixReady: boolean = false;
  @State statusText: string = '检测中...';
  @State zHome: string = '';
  // 渲染策略镜像 (dxvk_legacy / wined3d); 切换后提示需重启引擎才生效
  @State d3dBackend: string = 'dxvk_legacy';
  // SegmentButton 选中项 (@Link 双向): 0=dxvk_legacy 1=wined3d, 与 d3dBackend 同步
  @State backendSel: number[] = [0];
  @State restartHint: boolean = false;
  @State devExpanded: boolean = false;
  @State perfProfile: string = 'baseline';

  private env: WineEnvService = WineEnvService.getInstance();

  private mirror = (): void => {
    this.state = this.env.state;
    this.prefixReady = this.env.prefixReady;
    this.statusText = this.env.statusText;
    this.zHome = this.env.zHome;
    this.d3dBackend = this.env.getD3dBackend();
    // 派生选中项; 仅在值变化时赋值, 避免 notify 覆盖用户刚点击的 @Link 状态
    const sel = this.d3dBackend == 'dxvk_legacy' ? 0 : 1;
    if (this.backendSel[0] != sel) {
      this.backendSel = [sel];
    }
    this.perfProfile = this.env.getPerfProfile();
  }

  aboutToAppear(): void {
    this.env.subscribe(this.mirror);
    this.mirror();
  }

  // 状态文本配色, 与原标题栏/侧边栏同一套规则
  private statusColor(): Resource {
    return this.state == 'wine-running' ? $r('app.color.state_ok') :
           this.state == 'exited' ? $r('app.color.state_warn') :
           this.prefixReady ? $r('app.color.state_info') : $r('app.color.text_tertiary');
  }

  private engineBusy(): boolean {
    return this.state == 'wineserver-starting' || this.state == 'wineboot-starting';
  }

  // 渲染策略分段控件: 系统预置 SegmentButton 胶囊单选 (替代手写双 Button 行);
  // 切换仅写配置 (service 内持久化), 重启 Wine 引擎后生效
  private selectBackend(backend: string): void {
    if (backend == this.d3dBackend) {
      return;
    }
    this.env.setD3dPreference(backend);
    this.restartHint = true;
  }

  private confirmReset(): void {
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
  }

  private confirmStopAll(): void {
    AlertDialog.show({
      title: '停止全部 Windows 会话',
      message: '将结束所有正在运行的 Windows 程序（未保存数据会丢失）。确定继续？',
      primaryButton: {
        value: '取消',
        action: () => {}
      },
      secondaryButton: {
        value: '停止全部',
        fontColor: $r('app.color.danger'),
        action: () => {
          this.env.stopAllSessions();
        }
      }
    });
  }

  private doExportLogs(): void {
    const path = this.env.exportLogs();
    promptAction.showToast({
      message: path ? `日志已导出:\n${path}` : '日志导出失败'
    });
  }

  build() {
    Scroll() {
      Column({ space: 14 }) {
        // ══ 卡 1: Wine 引擎 ══
        Column({ space: 10 }) {
          Row() {
            Circle({ width: 8, height: 8 })
              .fill(this.prefixReady ? $r('app.color.state_ok') : $r('app.color.state_warn'))
              .margin({ right: 6 })
            Text('Wine 引擎')
              .fontWeight(FontWeight.Medium)
              .fontColor($r('app.color.text_primary'))
              .layoutWeight(1)
            Text(this.statusText)
              .fontSize(13)
              .fontColor(this.statusColor())
              .maxLines(1).textOverflow({ overflow: TextOverflow.Ellipsis })
          }
          .width('100%').alignItems(VerticalAlign.Center)

          Text('渲染策略')
            .fontSize(13)
            .fontColor($r('app.color.text_secondary'))
            .width('100%')
          SegmentButton({
            options: SegmentButtonOptions.capsule({
              buttons: [{ text: 'DXVK (推荐)' }, { text: 'WineD3D' }],
              multiply: false
            }),
            selectedIndexes: $backendSel,
            onItemClicked: (index: number) => {
              this.selectBackend(index == 0 ? 'dxvk_legacy' : 'wined3d');
            }
          })
          .width('100%')
          Text('DXVK 性能更好; 个别程序异常时可试 WineD3D')
            .fontSize(12)
            .fontColor($r('app.color.text_tertiary'))
            .width('100%')
          if (this.restartHint) {
            Text('渲染策略已更改, 重启 Wine 引擎后生效')
              .fontSize(12)
              .fontColor($r('app.color.state_warn'))
              .width('100%')
          }
        }
        .width('100%')
        .padding(14)
        .backgroundColor($r('app.color.card_bg'))
        .borderRadius(8)

        // ══ 卡 2: 会话维护 ══
        Column({ space: 10 }) {
          Text('会话维护')
            .fontWeight(FontWeight.Medium)
            .fontColor($r('app.color.text_primary'))
            .width('100%')
          Button(this.prefixReady ? '重启 Wine 引擎' : '初始化 Wine')
            .width('100%').backgroundColor($r('app.color.action_primary'))
            .enabled(!this.engineBusy() && this.state != 'wine-running')
            .onClick(() => {
              this.restartHint = false;
              this.env.doInit();
            })
          Button('停止全部 Windows 会话')
            .width('100%').backgroundColor($r('app.color.action_warning'))
            // 引擎活着即可用: 仅桌面会话(无用户程序)时 state 不是 wine-running,
            // 但桌面本身就是该停的会话
            .enabled(!this.engineBusy() && this.prefixReady)
            .onClick(() => this.confirmStopAll())
          Button('导出日志')
            .width('100%').backgroundColor($r('app.color.control_bg'))
            .onClick(() => this.doExportLogs())
        }
        .width('100%')
        .padding(14)
        .backgroundColor($r('app.color.card_bg'))
        .borderRadius(8)

        // ══ 卡 3: 危险区 (红色) ══
        Column({ space: 10 }) {
          Text('危险区')
            .fontWeight(FontWeight.Medium)
            .fontColor($r('app.color.danger'))
            .width('100%')
          Text('重置会删除容器内全部应用数据, 操作不可恢复')
            .fontSize(12)
            .fontColor($r('app.color.text_tertiary'))
            .width('100%')
          Button('重置 Wine prefix')
            .width('100%').backgroundColor($r('app.color.action_danger'))
            .fontColor($r('app.color.danger'))
            .enabled(!this.engineBusy() && this.state != 'wine-running')
            .onClick(() => this.confirmReset())
        }
        .width('100%')
        .padding(14)
        .backgroundColor($r('app.color.card_bg'))
        .borderRadius(8)
        .border({ width: 1, color: $r('app.color.action_danger') })

        // ══ 开发者分区 (折叠) ══
        Column() {
          Row() {
            Text('开发者')
              .fontWeight(FontWeight.Medium)
              .fontColor($r('app.color.text_secondary'))
              .layoutWeight(1)
            SymbolGlyph(this.devExpanded ? $r('sys.symbol.chevron_down') : $r('sys.symbol.chevron_right'))
              .fontSize(16)
              .fontColor([$r('app.color.text_secondary')])
          }
          .width('100%')
          .padding({ top: 4, bottom: 4 })
          .onClick(() => {
            this.devExpanded = !this.devExpanded;
          })
          if (this.devExpanded) {
            Text(`诊断 profile: ${this.perfProfile}`)
              .fontSize(12).fontColor($r('app.color.text_tertiary'))
              .width('100%').margin({ top: 6 })
            Text(`d3dBackend: ${this.d3dBackend}`)
              .fontSize(12).fontColor($r('app.color.text_tertiary'))
              .width('100%').margin({ top: 4 })
            Text(`Prefix: ${WINE_PREFIX}`)
              .fontSize(12).fontColor($r('app.color.text_tertiary'))
              .width('100%').margin({ top: 4 })
            Text(`Z: 映射: ${this.zHome}`)
              .fontSize(12).fontColor($r('app.color.text_tertiary'))
              .width('100%').margin({ top: 4 })
            Text(`设备类型: ${deviceInfo.deviceType}`)
              .fontSize(12).fontColor($r('app.color.text_tertiary'))
              .width('100%').margin({ top: 4 })
          }
        }
        .width('100%')
        .padding(14)
        .backgroundColor($r('app.color.card_bg'))
        .borderRadius(8)
      }
      .width('100%')
      .padding(14)
    }
    .width('100%').height('100%')
    .scrollBar(BarState.Off)
    .backgroundColor($r('app.color.window_bg'))
  }
}
