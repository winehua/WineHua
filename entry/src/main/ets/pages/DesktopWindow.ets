import hgsNapi from 'libhgs.so';
import { HgsSurface } from 'hgs';
import { WineWindowManager } from '../service/WineWindowManager';

// Pad/手机的全屏桌面页 (DesktopAbility 承载, singleton)。
// surface 生命周期与输入状态机由 WineSurface/WinePointerMachine 公共实现;
// 本页差异配置: 桌面命中模式 (hitTest = findToplevelAt, 触摸/滚轮先裁决
// 目标窗口)、鼠标捕获-配对型 (跨窗口按下抬起)、键盘发 rootId、反写桌面尺寸。
// (演进说明: 本页为 master 形态; main-ui 的组件化重写版见 DesktopLayer,
//  由 VirtualDesktop 复用 — 后续合并时两者统一)
@Entry
@Component
struct DesktopPage {
  build() {
    Column() {
      HgsSurface({
        xcId: 'wine_desktop_xc',
        resolveId: () => WineWindowManager.getInstance().getDesktopRootId(),
        mouseMode: 'desktop',
        onSurfaceCreatedCallback: (sid: string, id: number) => {
          try { hgsNapi.createRenderer(id, BigInt(sid)); } catch (_) {}
        },
        onSurfaceChangedCallback: (_sid: string, id: number, w: number, h: number) => {
          try {
            hgsNapi.resizeRenderer(id, w, h);
            const s = WineWindowManager.getInstance().getEffectiveScale();
            hgsNapi.notifyToplevelResize(id, Math.round(w / s), Math.round(h / s), false);
          } catch (_) {}
        },
        onSurfaceDestroyedCallback: (id: number) => {
          try { hgsNapi.destroyRenderer(id); } catch (_) {}
        }
      })
    }
    .width('100%')
    .height('100%')
    .backgroundColor('#000')
  }
}
