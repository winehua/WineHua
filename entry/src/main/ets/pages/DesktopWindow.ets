import { hilog } from '@kit.PerformanceAnalysisKit';
import testNapi from 'libentry.so';
import { WineWindowManager } from '../service/WineWindowManager';
import { OH2EVDEV } from '../common/KeyMap';
import { mapMouseButton } from '../common/MouseMap';

const DOMAIN = 0x0000;

class DesktopController extends XComponentController {
  onSurfaceCreated(surfaceId: string): void {
    const rootId = WineWindowManager.getInstance().getDesktopRootId();
    hilog.info(DOMAIN, 'LIFE', '[XComp] onSurfaceCreated rootId=%{public}d surfaceId=%{public}s', rootId, surfaceId);
    if (rootId > 0) {
      try { testNapi.createRenderer(rootId, BigInt(surfaceId)); } catch (_) {}
    }
  }
  onSurfaceChanged(surfaceId: string, rect: SurfaceRect): void {
    const rootId = WineWindowManager.getInstance().getDesktopRootId();
    hilog.info(DOMAIN, 'LIFE', '[XComp] onSurfaceChanged rootId=%{public}d %{public}dx%{public}d', rootId, rect.surfaceWidth, rect.surfaceHeight);
    if (rootId > 0) {
      try {
        testNapi.resizeRenderer(rootId, rect.surfaceWidth, rect.surfaceHeight);
        const s = 2.0;
        testNapi.notifyToplevelResize(rootId,
          Math.round(rect.surfaceWidth / s), Math.round(rect.surfaceHeight / s));
      } catch (_) {}
    }
  }
  onSurfaceDestroyed(_s: string): void {
    const rootId = WineWindowManager.getInstance().getDesktopRootId();
    hilog.warn(DOMAIN, 'LIFE', '[XComp] onSurfaceDestroyed rootId=%{public}d', rootId);
    if (rootId > 0) {
      try { testNapi.destroyRenderer(rootId); } catch (_) {}
    }
  }
}

@Entry
@Component
struct DesktopPage {
  controller: DesktopController = new DesktopController();

  build() {
    Stack() {
      XComponent({
        id: 'wine_desktop_xc',
        type: XComponentType.SURFACE,
        controller: this.controller
      }).width('100%').height('100%').backgroundColor('#000').focusable(false)
    }
    .width('100%').height('100%').backgroundColor('#000')
    .focusable(true).defaultFocus(true)

    .onTouch((ev: TouchEvent) => {
      try {
        const t = ev.changedTouches[0];
        if (!t) return;
        const px = Math.round(vp2px(t.x));
        const py = Math.round(vp2px(t.y));
        // 按坐标查找目标 toplevel (Weston-style: 动态 pick view, 无特殊 root)
        const wpx = Math.round(px / 2);
        const wpy = Math.round(py / 2);
        const tl: number = testNapi.findToplevelAt(wpx, wpy) as number;
        if (tl <= 0) return;
        if (ev.type === TouchType.Down) {
          hilog.info(DOMAIN, 'CLICK-PIPE', '[ArkTS] TOUCH tl=%{public}d px=(%{public}d,%{public}d)', tl, px, py);
          testNapi.raiseToplevel(tl);
          testNapi.sendPointerEvent(tl, MouseAction.Press, px, py, 0x110);
        } else if (ev.type === TouchType.Up) {
          testNapi.sendPointerEvent(tl, MouseAction.Release, px, py, 0x110);
        } else if (ev.type === TouchType.Cancel) {
          testNapi.sendPointerEvent(tl, MouseAction.Release, px, py, 0x110);
        }
      } catch (e) {
        hilog.error(DOMAIN, 'CLICK-PIPE', '[ArkTS] TOUCH handler exception: %{public}s', JSON.stringify(e));
      }
    })

    .onKeyEvent((ev: KeyEvent) => {
      const rootId = WineWindowManager.getInstance().getDesktopRootId();
      if (rootId <= 0) return;
      ev.stopPropagation();
      const code = OH2EVDEV[ev.keyCode] ?? 0;
      if (code <= 0) return;
      testNapi.sendKeyEvent(rootId, code, ev.type === KeyType.Down);
    })

    .onAxisEvent((ev?: AxisEvent) => {
      const rootId = WineWindowManager.getInstance().getDesktopRootId();
      if (rootId <= 0 || !ev) return;
      const wpx = Math.round(vp2px(ev.x) / 2);
      const wpy = Math.round(vp2px(ev.y) / 2);
      const vert = ev.getVerticalAxisValue();
      const horiz = ev.getHorizontalAxisValue();
      const step = ev.scrollStep ?? 0;
      const tl: number = testNapi.findToplevelAt(wpx, wpy) as number;
      if (vert !== 0) testNapi.sendScrollEvent(tl, 0, vert, step, wpx, wpy);
      if (horiz !== 0) testNapi.sendScrollEvent(tl, 1, horiz, step, wpx, wpy);
    })
  }
}
