import { deviceInfo } from '@kit.BasicServicesKit';
import { WineEnvService } from '../service/WineEnvService';

const WINE_PREFIX = '/data/storage/el2/base/files/.wine';

// 环境页: Wine 状态卡 (状态点/状态文本/prefix/Z: 映射/设备类型) + 初始化/重启/重置操作。
// 重置带 AlertDialog 二次确认; 各按钮 enabled 条件照抄原 Index 侧边栏。
@Component
export struct EnvPanel {
  @State state: string = 'idle';
  @State prefixReady: boolean = false;
  @State statusText: string = '检测中...';
  @State zHome: string = '';

  private env: WineEnvService = WineEnvService.getInstance();

  private mirror = (): void => {
    this.state = this.env.state;
    this.prefixReady = this.env.prefixReady;
    this.statusText = this.env.statusText;
    this.zHome = this.env.zHome;
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

  private confirmReset(): void {
    AlertDialog.show({
      title: '重置 Wine',
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

  build() {
    Column() {
      // -- 状态卡 --
      Column() {
        Row() {
          Circle({ width: 8, height: 8 })
            .fill(this.prefixReady ? $r('app.color.state_ok') : $r('app.color.state_warn'))
            .margin({ right: 6 })
          Text(this.statusText)
            .fontColor(this.statusColor())
            .layoutWeight(1)
            .maxLines(2).textOverflow({ overflow: TextOverflow.Ellipsis })
        }
        .width('100%').alignItems(VerticalAlign.Center)

        Text(`Prefix: ${WINE_PREFIX}`)
          .fontSize(12).fontColor($r('app.color.text_tertiary'))
          .width('100%').margin({ top: 10 })
        Text(`Z: 映射: ${this.zHome}`)
          .fontSize(12).fontColor($r('app.color.text_tertiary'))
          .width('100%').margin({ top: 4 })
        Text(`设备类型: ${deviceInfo.deviceType}`)
          .fontSize(12).fontColor($r('app.color.text_tertiary'))
          .width('100%').margin({ top: 4 })
      }
      .width('100%')
      .padding(14)
      .backgroundColor($r('app.color.card_bg'))
      .borderRadius(8)

      // -- 操作区 (enabled 条件照抄原 Index) --
      if (!this.prefixReady) {
        Button('初始化 Wine')
          .width('100%').backgroundColor($r('app.color.action_primary'))
          .enabled(this.state != 'wineserver-starting' && this.state != 'wineboot-starting')
          .onClick(this.env.doInit)
          .margin({ top: 14 })
      } else {
        Button('重启 Wine')
          .width('100%').backgroundColor($r('app.color.action_warning'))
          .enabled(this.state != 'wineserver-starting' && this.state != 'wineboot-starting' && this.state != 'wine-running')
          .onClick(this.env.doInit)
          .margin({ top: 14 })
        Button('重置 Wine')
          .width('100%').backgroundColor($r('app.color.action_danger'))
          .enabled(this.state != 'wineserver-starting' && this.state != 'wineboot-starting' && this.state != 'wine-running')
          .onClick(() => this.confirmReset())
          .margin({ top: 10 })
      }
    }
    .width('100%').height('100%')
    .padding(14)
    .backgroundColor($r('app.color.window_bg'))
  }
}
