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
  @State sessionAlive: boolean = false;
  @State busy: boolean = false;
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
    this.state = this.env.engineState;
    this.sessionAlive = this.env.sessionAlive;
    this.busy = this.env.isBusy();
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

  // 状态文本配色, 与标题栏同一套规则: 失败红 / 运行绿 / 就绪或进行中蓝 / 未启动灰
  private statusColor(): Resource {
    if (this.state == 'failed') {
      return $r('app.color.danger');
    }
    if (this.state == 'ready') {
      return this.sessionAlive ? $r('app.color.state_ok') : $r('app.color.state_info');
    }
    if (this.state == 'starting' || this.state == 'stopping') {
      return $r('app.color.state_info');
    }
    return $r('app.color.text_tertiary');
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
          promptAction.showToast({ message: '正在停止全部会话...' });
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
          Button(this.sessionAlive ? '重启 Wine 引擎' : (this.prefixReady ? '启动 Wine 引擎' : '初始化 Wine'))
            .width('100%').backgroundColor($r('app.color.action_primary'))
            // 判据 = 状态机: 仅 空闲/就绪/失败 可进入; 会话存活时走
            // restartEngine 编排 (先停后启), 不再直接 doInit 双开桌面
            .enabled(!this.busy &&
              (this.state == 'idle' || this.state == 'ready' || this.state == 'failed'))
            .onClick(() => {
              this.restartHint = false;
              this.env.restartEngine();
            })
          Button('停止全部 Windows 会话')
            .width('100%').backgroundColor($r('app.color.action_warning'))
            // 判据 = 会话存活 (桌面 root 或用户进程), 与 prefix 文件就绪无关
            .enabled(this.sessionAlive && !this.busy)
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
            // 编排内含"先停会话再删 prefix", 任何非忙状态都可用
            .enabled(!this.busy)
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
