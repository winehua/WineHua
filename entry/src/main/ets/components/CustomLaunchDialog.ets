import { SegmentButton, SegmentButtonOptions } from '@kit.ArkUI';

// 自定义启动对话框 (应用库/文件页右键菜单「自定义启动」弹出): 临时选择
// 渲染策略 + 兼容模式档位, 点「启动」按所选参数启动一次 — 不写入持久化配置。
// 两套选择第一项都是「跟随全局」= 不覆盖 (启动时映射为 undefined 传 onLaunch)。
// 兼容模式为 Box64 动态翻译档位 (仅 ARM 设备生效, x86_64 原生跑无效果)。
// 每次 open 都是新实例, @State 初值 [0] 自动回到跟随全局。
@CustomDialog
export struct CustomLaunchDialog {
  controller?: CustomDialogController;
  exeName: string = '';   // 标题 "自定义启动 · <exe>"
  onLaunch: (graphics?: 'dxvk' | 'wined3d',
             compatMode?: 'compat' | 'balanced' | 'perf') => void = () => {};
  // 渲染策略: 0=跟随全局 1=dxvk 2=wined3d
  @State graphicsSel: number[] = [0];
  // 兼容模式: 0=跟随全局 1=兼容优先 2=均衡 3=性能优先
  @State compatSel: number[] = [0];

  private doLaunch(): void {
    const g = this.graphicsSel[0] ?? 0;
    const c = this.compatSel[0] ?? 0;
    this.onLaunch(g == 1 ? 'dxvk' : g == 2 ? 'wined3d' : undefined,
      c == 1 ? 'compat' : c == 2 ? 'balanced' : c == 3 ? 'perf' : undefined);
    this.controller?.close();
  }

  build() {
    Column({ space: 16 }) {
      Text(`自定义启动 · ${this.exeName}`)
        .fontWeight(FontWeight.Medium)
        .fontColor($r('app.color.text_primary'))
        .maxLines(1)
        .textOverflow({ overflow: TextOverflow.Ellipsis })

      Column({ space: 8 }) {
        Text('渲染策略')
          .fontSize(13)
          .fontColor($r('app.color.text_secondary'))
        SegmentButton({
          options: SegmentButtonOptions.capsule({
            buttons: [{ text: '跟随全局' }, { text: 'DXVK' }, { text: 'WineD3D' }],
            multiply: false
          }),
          selectedIndexes: $graphicsSel,
          onItemClicked: (index: number) => {
            this.graphicsSel = [index];
          }
        })
      }
      .width('100%')
      .alignItems(HorizontalAlign.Start)

      Column({ space: 8 }) {
        Text('兼容模式')
          .fontSize(13)
          .fontColor($r('app.color.text_secondary'))
        SegmentButton({
          options: SegmentButtonOptions.capsule({
            buttons: [
              { text: '跟随全局' }, { text: '兼容优先' }, { text: '均衡' }, { text: '性能优先' }
            ],
            multiply: false
          }),
          selectedIndexes: $compatSel,
          onItemClicked: (index: number) => {
            this.compatSel = [index];
          }
        })
        Text('Box64 动态翻译档位')
          .fontSize(11)
          .fontColor($r('app.color.text_tertiary'))
      }
      .width('100%')
      .alignItems(HorizontalAlign.Start)

      Row({ space: 12 }) {
        Button('取消')
          .layoutWeight(1)
          .backgroundColor($r('app.color.control_bg'))
          .onClick(() => this.controller?.close())
        Button('启动')
          .layoutWeight(1)
          .onClick(() => {
            this.doLaunch();
          })
      }
      .width('100%')
    }
    .padding(18)
    .backgroundColor($r('app.color.overlay_bg'))
    .borderRadius(12)
  }
}
