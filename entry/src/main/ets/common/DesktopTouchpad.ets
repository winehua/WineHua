import testNapi from 'libentry.so';
import { DesktopInputSink } from '../components/DesktopInputSink';
import { DesktopPointerService, sendDesktopPointerEvent } from '../service/DesktopPointerService';
import { mapMouseButton } from './MouseMap';

// One instance per desktop surface. Relative motion retains the shared cursor
// across finger lifts and mode changes; bounds are XComponent-local physical px.
export class DesktopTouchpad {
  private static readonly LONG_PRESS_MS: number = 500;
  private static readonly DOUBLE_TAP_MS: number = 400;
  private static readonly DOUBLE_TAP_DIST: number = 30;
  private static readonly DRAG_THRESHOLD: number = 10;
  private static readonly SCROLL_STEP_PX: number = 40;
  private vcX: number = 0;   // 虚拟光标 (物理 px)
  private vcY: number = 0;
  private scrWPx: number = 1280;
  private scrHPx: number = 800;
  cursorSpeed: number = 1.0;
  // 触控板手势状态 (与直接触控状态机字段互不共用)
  private tpDown: boolean = false;
  private tpDownX: number = 0;   // Down 时触摸点 (vp)
  private tpDownY: number = 0;
  private tpLastX: number = 0;
  private tpLastY: number = 0;
  private tpMoved: boolean = false;
  private tpDragArmed: boolean = false;  // 双击后按住: 拖拽预备
  private tpDragging: boolean = false;
  private tpDragTl: number = 0;
  private tpLastTapTime: number = 0;
  private tpLastTapVcX: number = 0;
  private tpLastTapVcY: number = 0;
  private tpTwoFinger: boolean = false;
  private tpTwoFingerLastMidX: number = 0;
  private tpTwoFingerLastMidY: number = 0;
  private tpScrollAccumX: number = 0;
  private tpScrollAccumY: number = 0;

  private tpFinger: number = -1;
  private holdTimer: number = -1;

  private inputSink: DesktopInputSink | undefined;
  constructor(inputSink?: DesktopInputSink) { this.inputSink = inputSink; }

  private dispatchPointer(target: number, action: number, x: number, y: number, button: number): void {
    DesktopPointerService.getInstance().move(x, y);
    if (!this.inputSink) { sendDesktopPointerEvent(target, action, x, y, button); return; }
    if (action === MouseAction.Press) this.inputSink.pressAt(target, x, y, button, false);
    else if (action === MouseAction.Release) this.inputSink.releaseAt(target, x, y, button);
    else this.inputSink.strokeMove(target, x, y);
  }

  private dispatchScroll(target: number, axis: number, steps: number, stepPx: number,
    x: number, y: number): void {
    if (this.inputSink) this.inputSink.scrollAt(target, axis, steps, stepPx, x, y);
    else testNapi.sendScrollEvent(target, axis, steps, stepPx, x, y);
  }

  private raiseTarget(target: number): void {
    if (this.inputSink) this.inputSink.beginStroke(Math.round(this.vcX), Math.round(this.vcY));
    else testNapi.raiseToplevel(target);
  }

  setBounds(width: number, height: number): void {
    if (!(width > 0 && height > 0)) return;
    if (this.scrWPx !== width || this.scrHPx !== height) this.reset();
    this.scrWPx = width;
    this.scrHPx = height;
    this.vcX = Math.min(Math.max(this.vcX, 0), width - 1);
    this.vcY = Math.min(Math.max(this.vcY, 0), height - 1);
  }

  activate(): void {
    const cursor = DesktopPointerService.getInstance();
    this.vcX = Math.min(Math.max(cursor.positioned ? cursor.x : this.scrWPx / 2, 0), this.scrWPx - 1);
    this.vcY = Math.min(Math.max(cursor.positioned ? cursor.y : this.scrHPx / 2, 0), this.scrHPx - 1);
    this.syncCursorIndicator();
  }

  private syncCursorIndicator(): void {
    DesktopPointerService.getInstance().move(this.vcX, this.vcY);
  }

  private tlAtCursor(): number {
    if (this.inputSink) return this.inputSink.hitTest(Math.round(this.vcX), Math.round(this.vcY));
    return (testNapi.findToplevelAt(Math.round(this.vcX), Math.round(this.vcY)) as number) ?? 0;
  }

  private clickAtCursor(btn: number): void {
    const tl = this.tlAtCursor();
    if (tl <= 0) return;
    this.raiseTarget(tl);
    const x = Math.round(this.vcX);
    const y = Math.round(this.vcY);
    this.dispatchPointer(tl, MouseAction.Press, x, y, btn);
    this.dispatchPointer(tl, MouseAction.Release, x, y, btn);
  }

  private clearHoldTimer(): void {
    if (this.holdTimer >= 0) clearTimeout(this.holdTimer);
    this.holdTimer = -1;
  }

  private beginHold(): void {
    this.clearHoldTimer();
    this.tpDragging = true;
    this.tpDragTl = this.tlAtCursor();
    if (this.tpDragTl > 0) {
      this.raiseTarget(this.tpDragTl);
      this.dispatchPointer(this.tpDragTl, MouseAction.Press,
        Math.round(this.vcX), Math.round(this.vcY), mapMouseButton(1));
    }
  }

  reset(): void {
    this.clearHoldTimer();
    if (this.tpDragging && this.tpDragTl > 0) {
      this.dispatchPointer(this.tpDragTl, MouseAction.Release,
        Math.round(this.vcX), Math.round(this.vcY), mapMouseButton(1));
    }
    this.tpFinger = -1;
    this.tpDown = false;
    this.tpScrollAccumX = 0;
    this.tpScrollAccumY = 0;
    this.tpMoved = false;
    this.tpDragArmed = false;
    this.tpDragging = false;
    this.tpDragTl = 0;
    this.tpTwoFinger = false;
    this.tpLastTapTime = 0;
  }

  // 触控板模式 onTouch 总入口 (touchpadOn 时替代单/双指手势状态机)
  handle(ev: TouchEvent): void {
    if (ev.type === TouchType.Cancel) { this.reset(); return; }

    // 双指: 滚动作用于光标处 (触控板语义), 并打断单指/拖拽
    if (ev.touches.length >= 2) {
      let midX = 0, midY = 0;
      for (let i = 0; i < ev.touches.length; i++) {
        midX += vp2px(ev.touches[i].x);
        midY += vp2px(ev.touches[i].y);
      }
      midX /= ev.touches.length;
      midY /= ev.touches.length;
      if (!this.tpTwoFinger) {
        this.clearHoldTimer();
        this.tpLastTapTime = 0;
        if (this.tpDragging && this.tpDragTl > 0) {
          this.dispatchPointer(this.tpDragTl, MouseAction.Release,
            Math.round(this.vcX), Math.round(this.vcY), mapMouseButton(1));
        }
        this.tpFinger = -1;
        this.tpDown = false;
        this.tpDragging = false;
        this.tpDragTl = 0;
        this.tpDragArmed = false;
        this.tpTwoFinger = true;
        this.tpTwoFingerLastMidX = midX;
        this.tpTwoFingerLastMidY = midY;
        this.tpScrollAccumX = 0;
        this.tpScrollAccumY = 0;
      } else {
        this.tpScrollAccumY += this.tpTwoFingerLastMidY - midY;
        this.tpScrollAccumX += midX - this.tpTwoFingerLastMidX;
        this.tpTwoFingerLastMidX = midX;
        this.tpTwoFingerLastMidY = midY;
        const tl = this.tlAtCursor();
        if (tl > 0) {
          while (Math.abs(this.tpScrollAccumY) >= DesktopTouchpad.SCROLL_STEP_PX) {
            const sign = this.tpScrollAccumY > 0 ? 1 : -1;
            this.dispatchScroll(tl, 0, sign, DesktopTouchpad.SCROLL_STEP_PX,
              Math.round(this.vcX), Math.round(this.vcY));
            this.tpScrollAccumY -= sign * DesktopTouchpad.SCROLL_STEP_PX;
          }
          while (Math.abs(this.tpScrollAccumX) >= DesktopTouchpad.SCROLL_STEP_PX) {
            const sign = this.tpScrollAccumX > 0 ? 1 : -1;
            this.dispatchScroll(tl, 1, sign, DesktopTouchpad.SCROLL_STEP_PX,
              Math.round(this.vcX), Math.round(this.vcY));
            this.tpScrollAccumX -= sign * DesktopTouchpad.SCROLL_STEP_PX;
          }
        }
      }
      return;
    }
    if (this.tpTwoFinger) {
      this.tpTwoFinger = false;
      return;
    }

    const t = ev.changedTouches.find((touch: TouchObject) => this.tpFinger < 0 || touch.id === this.tpFinger);
    if (!t) return;

    if (ev.type === TouchType.Down) {
      if (this.tpDown) return;
      this.activate();
      this.tpFinger = t.id;
      this.tpDown = true;
      this.tpMoved = false;
      this.tpDownX = t.x;
      this.tpDownY = t.y;
      this.tpLastX = t.x;
      this.tpLastY = t.y;
      // 双击后按住 → 拖拽预备 (标准触控板 drag 手势: 第二次 Down 且光标没怎么挪)
      const dvc = Math.abs(this.vcX - this.tpLastTapVcX) + Math.abs(this.vcY - this.tpLastTapVcY);
      this.tpDragArmed = this.tpLastTapTime > 0
        && (Date.now() - this.tpLastTapTime) < DesktopTouchpad.DOUBLE_TAP_MS
        && dvc < DesktopTouchpad.DOUBLE_TAP_DIST * 2;
      if (this.tpDragArmed) this.beginHold();
      else this.holdTimer = setTimeout(() => {
        this.holdTimer = -1;
        if (this.tpDown && !this.tpMoved) this.beginHold();
      }, DesktopTouchpad.LONG_PRESS_MS);
    } else if (ev.type === TouchType.Move) {
      if (!this.tpDown) return;
      const dxPx = vp2px(t.x - this.tpLastX) * this.cursorSpeed;
      const dyPx = vp2px(t.y - this.tpLastY) * this.cursorSpeed;
      this.tpLastX = t.x;
      this.tpLastY = t.y;
      if (dxPx == 0 && dyPx == 0) return;
      if (!this.tpMoved) {
        const moved = Math.abs(vp2px(t.x - this.tpDownX)) + Math.abs(vp2px(t.y - this.tpDownY));
        if (moved >= DesktopTouchpad.DRAG_THRESHOLD) {
          this.tpMoved = true;
          this.clearHoldTimer();
        }
      }
      this.vcX = Math.min(Math.max(this.vcX + dxPx, 0), this.scrWPx - 1);
      this.vcY = Math.min(Math.max(this.vcY + dyPx, 0), this.scrHPx - 1);
      this.syncCursorIndicator();
      const moveTl = this.tpDragging ? this.tpDragTl : this.tlAtCursor();
      if (moveTl > 0) {
        this.dispatchPointer(moveTl, MouseAction.Move,
          Math.round(this.vcX), Math.round(this.vcY), 0);
      }
    } else if (ev.type === TouchType.Up) {
      if (!this.tpDown) return;
      this.clearHoldTimer();
      this.tpDown = false;
      this.tpFinger = -1;
      if (this.tpDragging) {
        if (this.tpDragTl > 0) {
          this.dispatchPointer(this.tpDragTl, MouseAction.Release,
            Math.round(this.vcX), Math.round(this.vcY), mapMouseButton(1));
        }
        this.tpDragging = false;
        this.tpDragTl = 0;
        this.tpDragArmed = false;
        this.tpLastTapTime = 0;
        return;
      }
      this.tpDragArmed = false;
      if (!this.tpMoved) {
        this.clickAtCursor(mapMouseButton(1));
        this.tpLastTapTime = Date.now();
        this.tpLastTapVcX = this.vcX;
        this.tpLastTapVcY = this.vcY;
      }
    }
  }

}
