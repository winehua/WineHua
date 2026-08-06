import { bundleManager } from '@kit.AbilityKit';
import { WineEnvService } from '../service/WineEnvService';
import { WineWindowManager } from '../service/WineWindowManager';

// 设置页 (设计 4.6, 骨架版): 显示/开发者/关于三卡。
// 说明: 应用调色板目前只有深色一套, 主题切换与 DPI 缩放、输入设置
// (手柄模拟/触控板, 设计 P4/P5) 后续版本开放 — 这里只做诚实占位, 不假开关。
@Component
export struct SettingsView {
  @State versionName: string = '';
  @State perfProfile: string = 'baseline';
  @State d3dBackend: string = 'dxvk_legacy';
  @State effectiveScale: string = '';

  private env: WineEnvService = WineEnvService.getInstance();

  aboutToAppear(): void {
    this.perfProfile = this.env.getPerfProfile();
    this.d3dBackend = this.env.getD3dBackend();
    this.effectiveScale = WineWindowManager.getInstance().getEffectiveScale().toFixed(1);
    bundleManager.getBundleInfoForSelf(
      bundleManager.BundleFlag.GET_BUNDLE_INFO_WITH_APPLICATION
    ).then((info: bundleManager.BundleInfo) => {
      this.versionName = info.versionName;
    }).catch(() => {
      this.versionName = '';
    });
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
          Row() {
            Text('主题')
              .fontColor($r('app.color.text_secondary'))
              .layoutWeight(1)
            Text('深色')
              .fontColor($r('app.color.text_tertiary'))
          }
          .width('100%')
          Row() {
            Text('DPI 缩放 (全局)')
              .fontColor($r('app.color.text_secondary'))
              .layoutWeight(1)
            Text(`${this.effectiveScale}x`)
              .fontColor($r('app.color.text_tertiary'))
          }
          .width('100%')
          Text('浅色主题与缩放调节将在后续版本开放')
            .fontSize(12)
            .fontColor($r('app.color.text_tertiary'))
            .width('100%')
        }
        .width('100%')
        .padding(14)
        .backgroundColor($r('app.color.card_bg'))
        .borderRadius(8)

        // ══ 开发者 ══
        Column({ space: 8 }) {
          Text('开发者')
            .fontWeight(FontWeight.Medium)
            .fontColor($r('app.color.text_primary'))
            .width('100%')
          Row() {
            Text('诊断 profile')
              .fontColor($r('app.color.text_secondary'))
              .layoutWeight(1)
            Text(this.perfProfile)
              .fontSize(12)
              .fontColor($r('app.color.text_tertiary'))
          }
          .width('100%')
          Row() {
            Text('渲染后端')
              .fontColor($r('app.color.text_secondary'))
              .layoutWeight(1)
            Text(this.d3dBackend)
              .fontSize(12)
              .fontColor($r('app.color.text_tertiary'))
          }
          .width('100%')
          Text('自动化测试经 hdc 以 want 参数启动 (winehua.mode=smoke)')
            .fontSize(12)
            .fontColor($r('app.color.text_tertiary'))
            .width('100%')
        }
        .width('100%')
        .padding(14)
        .backgroundColor($r('app.color.card_bg'))
        .borderRadius(8)

        // ══ 关于 ══
        Column({ space: 8 }) {
          Text('关于')
            .fontWeight(FontWeight.Medium)
            .fontColor($r('app.color.text_primary'))
            .width('100%')
          Row() {
            Text('万花境')
              .fontColor($r('app.color.text_secondary'))
              .layoutWeight(1)
            Text(this.versionName ? `v${this.versionName}` : '')
              .fontColor($r('app.color.text_tertiary'))
          }
          .width('100%')
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
