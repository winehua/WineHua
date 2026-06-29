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
  // 长按转右键状态
  private touchPending: boolean = false;
  private longPressTriggered: boolean = false;
  private pendingTl: number = 0;
  private pendingPx: number = 0;
  private pendingPy: number = 0;
  // 双击检测
  private lastTapTime: number = 0;
  private lastTapPx: number = 0;
  private lastTapPy: number = 0;
  private lastTapTl: number = 0;
  private static readonly DOUBLE_TAP_MS: number = 400;
  private static readonly DOUBLE_TAP_DIST: number = 30;

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

    // -- 长按转右键 (Pad) --
    // 触摸不立即发送: 抬手前 < 500ms 发左键, >=500ms 发右键, 互斥
    .gesture(
      LongPressGesture({ duration: 500, fingers: 1 })
        .onAction((event: GestureEvent) => {
          if (!this.touchPending) return;
          this.longPressTriggered = true;
          this.lastTapTime = 0; // 长按打断双击序列
          const btnRight = mapMouseButton(2); // MouseButton.Right
          testNapi.sendPointerEvent(this.pendingTl, MouseAction.Press, this.pendingPx, this.pendingPy, btnRight);
          testNapi.sendPointerEvent(this.pendingTl, MouseAction.Release, this.pendingPx, this.pendingPy, btnRight);
          hilog.info(DOMAIN, 'CLICK-PIPE', '[ArkTS] LONGPRESS->RIGHTCLICK tl=%{public}d px=(%{public}d,%{public}d)',
            this.pendingTl, this.pendingPx, this.pendingPy);
        })
    )

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
          // 暂不发送 Press: 等长按判定
          this.touchPending = true;
          this.longPressTriggered = false;
          this.pendingTl = tl;
          this.pendingPx = px;
          this.pendingPy = py;
        } else if (ev.type === TouchType.Up) {
          if (this.longPressTriggered) {
            // 长按已发右键, 抑制左键
            this.touchPending = false;
            this.longPressTriggered = false;
          } else if (this.touchPending) {
            // 短按 (< 500ms): 检测双击后发左键点击
            this.touchPending = false;
            const btnLeft = mapMouseButton(1);
            const now = Date.now();
            const dist = Math.abs(this.pendingPx - this.lastTapPx) + Math.abs(this.pendingPy - this.lastTapPy);
            const isDoubleTap = (this.lastTapTime > 0 && (now - this.lastTapTime) < DesktopPage.DOUBLE_TAP_MS
                                && dist < DesktopPage.DOUBLE_TAP_DIST && this.pendingTl === this.lastTapTl);

            if (isDoubleTap) {
              // 双击: 两个连续点击, 使用同一位置 (解决手指位移导致 Wine 不识别)
              this.lastTapTime = 0; // 防止三击
              testNapi.sendPointerEvent(this.pendingTl, MouseAction.Press, this.lastTapPx, this.lastTapPy, btnLeft);
              testNapi.sendPointerEvent(this.pendingTl, MouseAction.Release, this.lastTapPx, this.lastTapPy, btnLeft);
              testNapi.sendPointerEvent(this.pendingTl, MouseAction.Press, this.lastTapPx, this.lastTapPy, btnLeft);
              testNapi.sendPointerEvent(this.pendingTl, MouseAction.Release, this.lastTapPx, this.lastTapPy, btnLeft);
              hilog.info(DOMAIN, 'CLICK-PIPE', '[ArkTS] DOUBLE-TAP tl=%{public}d px=(%{public}d,%{public}d)',
                this.pendingTl, this.lastTapPx, this.lastTapPy);
            } else {
              // 单击
              testNapi.sendPointerEvent(this.pendingTl, MouseAction.Press, this.pendingPx, this.pendingPy, btnLeft);
              testNapi.sendPointerEvent(this.pendingTl, MouseAction.Release, this.pendingPx, this.pendingPy, btnLeft);
              // 记录本次 tap 用于潜在双击
              this.lastTapTime = now;
              this.lastTapPx = this.pendingPx;
              this.lastTapPy = this.pendingPy;
              this.lastTapTl = this.pendingTl;
              hilog.info(DOMAIN, 'CLICK-PIPE', '[ArkTS] TAP->LEFTCLICK tl=%{public}d px=(%{public}d,%{public}d)',
                this.pendingTl, this.pendingPx, this.pendingPy);
            }
          }
        } else if (ev.type === TouchType.Cancel) {
          // 取消: 直接清状态 (Press 未发送, 无需 Release)
          this.touchPending = false;
          this.longPressTriggered = false;
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
