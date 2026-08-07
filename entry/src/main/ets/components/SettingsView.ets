import { bundleManager } from '@kit.AbilityKit';
import { ComposeListItem } from '@kit.ArkUI';
import { WineEnvService } from '../service/WineEnvService';
import { DisplaySettingsService, SCALE_PRESETS } from '../service/DisplaySettingsService';

// 缩放选择对话框: 预设档位单选, 点选即存 (DisplaySettingsService.setScale)。
// 仅启动时生效 (WineWindowManager.init 读取), 故对话框内注明"重启应用后生效"。
@CustomDialog
struct ScalePickerDialog {
  controller?: CustomDialogController;
  current: number = 2.0;
  onPick: (scale: number) => void = () => {};

  private fmt(v: number): string {
    return `${Math.round(v * 100) / 100}x`;
  }

  build() {
    Column({ space: 6 }) {
      Text('DPI 缩放 (全局)')
        .fontWeight(FontWeight.Medium)
        .fontColor($r('app.color.text_primary'))
        .width('100%')
        .margin({ bottom: 6 })
      ForEach(SCALE_PRESETS, (preset: number) => {
        Row() {
          Text(this.fmt(preset))
            .fontColor($r('app.color.text_primary'))
          Blank()
          if (Math.abs(preset - this.current) < 0.001) {
            Text('✓')
              .fontColor($r('app.color.accent'))
              .fontWeight(FontWeight.Medium)
          }
        }
        .width('100%')
        .padding({ top: 10, bottom: 10, left: 12, right: 12 })
        .borderRadius(8)
        .backgroundColor(Math.abs(preset - this.current) < 0.001 ?
          $r('app.color.control_bg') : Color.Transparent)
        .onClick(() => {
          this.onPick(preset);
          this.controller?.close();
        })
      })
      Text('重启应用后生效')
        .fontSize(12)
        .fontColor($r('app.color.text_tertiary'))
        .width('100%')
        .margin({ top: 8 })
    }
    .padding(18)
    .backgroundColor($r('app.color.overlay_bg'))
    .borderRadius(12)
  }
}

// 设置页 (设计 4.6, 骨架版): 显示/开发者/关于三卡。
// 行用系统预置 ComposeListItem (左标题+右值), 统一行高/间距/字级。
// 注: ComposeListItem 不宜挂通用属性 (会落到额外生成的 __Common__ 节点上),
// 故卡片外观由外层 Column 承担。
// 说明: 应用调色板目前只有深色一套, 主题切换后续版本开放。
// DPI 缩放可编辑 (全局, 重启应用后生效, 见 DisplaySettingsService);
// 输入设置在桌面悬浮条与布局编辑里 (设计 P4/P5 已落地)。
@Component
export struct SettingsView {
  @State versionName: string = '';
  @State perfProfile: string = 'baseline';
  @State d3dBackend: string = 'dxvk_legacy';
  @State scaleText: string = '';

  private env: WineEnvService = WineEnvService.getInstance();
  private displaySettings: DisplaySettingsService = DisplaySettingsService.getInstance();
  private scaleController: CustomDialogController | null = null;

  private fmtScale(v: number): string {
    return `${Math.round(v * 100) / 100}x`;
  }

  aboutToAppear(): void {
    this.perfProfile = this.env.getPerfProfile();
    this.d3dBackend = this.env.getD3dBackend();
    this.scaleText = this.fmtScale(this.displaySettings.scale);
    bundleManager.getBundleInfoForSelf(
      bundleManager.BundleFlag.GET_BUNDLE_INFO_WITH_APPLICATION
    ).then((info: bundleManager.BundleInfo) => {
      this.versionName = info.versionName;
    }).catch(() => {
      this.versionName = '';
    });
  }

  // 每次打开前重建 controller: builder 参数在创建时快照,
  // 复用旧 controller 会让"当前档位"高亮停留在上次值
  private openScalePicker(): void {
    this.scaleController = new CustomDialogController({
      builder: ScalePickerDialog({
        current: this.displaySettings.scale,
        onPick: (scale: number) => {
          this.displaySettings.setScale(scale);
          this.scaleText = this.fmtScale(scale);
        }
      }),
      customStyle: true
    });
    this.scaleController.open();
  }

  build() {
    Scroll() {
      Column({ space: 14 }) {
        // ══ 显示 ══
        Column({ space: 8 }) {
          Text('显示')
            .fontWeight(FontWeight.Medium)
            .fontColor($r('app.color.text_primary'))
            .width('100%')
          ComposeListItem({
            contentItem: { primaryText: '主题' },
            operateItem: { text: '深色' }
          })
          ComposeListItem({
            contentItem: { primaryText: 'DPI 缩放 (全局)' },
            operateItem: { text: this.scaleText }
          })
            .onClick(() => this.openScalePicker())
          Text('缩放调节重启应用后生效; 浅色主题将在后续版本开放')
            .fontSize(12)
            .fontColor($r('app.color.text_tertiary'))
            .width('100%')
        }
        .width('100%')
        .padding(14)
        .backgroundColor($r('app.color.card_bg'))
        .borderRadius(12)

        // ══ 开发者 ══
        Column({ space: 8 }) {
          Text('开发者')
            .fontWeight(FontWeight.Medium)
            .fontColor($r('app.color.text_primary'))
            .width('100%')
          ComposeListItem({
            contentItem: { primaryText: '诊断 profile' },
            operateItem: { text: this.perfProfile }
          })
          ComposeListItem({
            contentItem: { primaryText: '渲染后端' },
            operateItem: { text: this.d3dBackend }
          })
          Text('自动化测试经 hdc 以 want 参数启动 (winehua.mode=smoke)')
            .fontSize(12)
            .fontColor($r('app.color.text_tertiary'))
            .width('100%')
        }
        .width('100%')
        .padding(14)
        .backgroundColor($r('app.color.card_bg'))
        .borderRadius(12)

        // ══ 关于 ══
        Column({ space: 8 }) {
          Text('关于')
            .fontWeight(FontWeight.Medium)
            .fontColor($r('app.color.text_primary'))
            .width('100%')
          ComposeListItem({
            contentItem: { primaryText: '万花境' },
            operateItem: { text: this.versionName ? `v${this.versionName}` : '' }
          })
          Text('开源组件')
            .fontColor($r('app.color.text_secondary'))
            .width('100%')
            .margin({ top: 4 })
          Text('Wine · Box64 · DXVK · VirGL Renderer · Mesa')
            .fontSize(12)
            .fontColor($r('app.color.text_tertiary'))
            .width('100%')
        }
        .width('100%')
        .padding(14)
        .backgroundColor($r('app.color.card_bg'))
        .borderRadius(12)
      }
      .width('100%')
      .padding(14)
    }
    .width('100%').height('100%')
    .scrollBar(BarState.Off)
    .backgroundColor($r('app.color.window_bg'))
  }
}
