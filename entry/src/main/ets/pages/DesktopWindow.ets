import { DesktopLayer } from '../components/DesktopLayer';

// Pad x11 虚拟桌面承载页 (DesktopAbility loadContent)。
// 出图/输入全部走 DesktopLayer 承载统一 seam (#173): 起链/画布登记/
// 手势判定/键鼠轴注入都在组件内, 语义与 PC VDA (VirtualDesktop.ets)
// 同源 —— 本页此前是 pre-seam 旧实现 (内联起链 + 双手势状态机副本),
// 退役后 Pad 承载几何语义随之升级为 present 层单点 fit (letterbox) +
// displayRouteCanvasResized 强帧 (桌面承载统一, #174/#175 同族)。
@Entry
@Component
struct DesktopPage {
  build() {
    Stack() {
      DesktopLayer({ floating: false })
    }
    .width('100%').height('100%').backgroundColor('#000')
  }
}
